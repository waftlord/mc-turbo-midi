#!/usr/bin/env python3
"""Build and package a matched MegaCommand DIY firmware package."""

import argparse
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
from zipfile import ZipFile, ZIP_DEFLATED


ROOT = Path(__file__).resolve().parent.parent
USB = "megacommand-diy-dual-atmega16u2-dfu.hex"
MAIN = "megacommand-diy-dual-atmega2560-update.hex"
AVR_PACKAGE = "platformio/toolchain-atmelavr@1.70300.191015"


def build(bundle: Path, output: Path, pio: str) -> None:
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix=".package-", dir=output) as temporary:
        work = Path(temporary)
        snapshot = work / "source"
        snapshot.mkdir()
        for folder in ("src", "tools", "examples", "tests", "LICENSES"):
            shutil.copytree(ROOT / folder, snapshot / folder,
                            ignore=shutil.ignore_patterns(".DS_Store", "__pycache__"))
        for name in ("README.md", "PROTOCOL.md",
                     "LICENSE", "THIRD_PARTY_NOTICES.md", "platformio.ini"):
            shutil.copy2(ROOT / name, snapshot / name)

        pio_build = work / "pio"
        environment = {**os.environ, "PLATFORMIO_BUILD_DIR": str(pio_build)}
        subprocess.run([pio, "run", "-e", "megacommand_diy"],
                       cwd=snapshot, env=environment, check=True)
        images = snapshot / "build"
        images.mkdir()
        shutil.copyfile(pio_build / "megacommand_diy" / "firmware.hex", images / MAIN)
        subprocess.run([pio, "pkg", "exec", "-p", AVR_PACKAGE, "--", sys.executable,
                        "tools/build_usb_firmware.py", "--bundle", str(bundle)],
                       cwd=snapshot, env=environment, check=True)

        digests = {name: hashlib.sha256((images / name).read_bytes()).hexdigest()
                   for name in (USB, MAIN)}
        manifest = "".join(f"{digest}  build/{name}\n" for name, digest in digests.items())
        (snapshot / "SHA256SUMS").write_text(manifest, encoding="ascii")
        image_ids = "-".join(digest[:8] for digest in digests.values())
        name = f"megacommand-diy-dual-midi-{image_ids}"
        destination = output / name
        archive = output / f"{name}.zip"
        if destination.exists() or archive.exists():
            raise FileExistsError(f"Package already exists. Choose a different --output directory: {name}")
        staged_archive = work / archive.name
        with ZipFile(staged_archive, "w", ZIP_DEFLATED) as zipped:
            for path in sorted(snapshot.rglob("*")):
                if path.is_file():
                    zipped.write(path, Path(name) / path.relative_to(snapshot))
        with ZipFile(staged_archive) as zipped:
            if zipped.testzip() is not None:
                raise ValueError("Package ZIP failed its integrity check")
            for image, digest in digests.items():
                if hashlib.sha256(zipped.read(f"{name}/build/{image}")).hexdigest() != digest:
                    raise ValueError(f"Package ZIP hash mismatch: {image}")
        snapshot.rename(destination)
        staged_archive.rename(archive)

    print(f"Package: {destination}")
    print(f"ZIP: {archive}")
    print(manifest, end="")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True, help="upstream USB firmware 1.0.4 ZIP")
    parser.add_argument("--output", type=Path, default=ROOT / "build" / "packages")
    parser.add_argument("--pio", default="pio", help="path to the PlatformIO executable")
    args = parser.parse_args()
    pio = str(Path(args.pio).resolve()) if os.sep in args.pio else args.pio
    try:
        build(args.bundle.resolve(strict=True), args.output.resolve(), pio)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Package build failed: {error}\n")


if __name__ == "__main__":
    main()
