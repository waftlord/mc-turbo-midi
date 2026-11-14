#!/usr/bin/env python3
"""Build the USB flow-control HEX from the pinned MegaCommand DIY release."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
from zipfile import BadZipFile, ZipFile


DIY_IMAGE = "megacmd_usb_dfu_1.0.4/megacommand-diy-usb-midi-1.0.4.hex"
DIY_SHA256 = "5262a2ffeff37b0a6698675dbabb4916096a96fc806c59a37aaf942861c5fa8e"
OUTPUT = Path("build/megacommand-diy-atmega16u2.hex")
OUTPUT_SHA256 = "f77a496df35f443912e4bb4a6a3d308298d2e196a1f26d07453ea046b1f98b6a"
APP_END = 0x3000  # Earliest possible boot section on the ATmega16U2.
PATCH_START = 0x1914

# Expected flash bytes at each overwritten instruction sequence.
HOOKS = {
    0x02D4: (".midi_in_flush_hook", "8e b3 84 30"),
    0x0300: (".midi_in_write_hook", "ef 92 ff 92"),
    0x07B0: (".active_sense_hook", "88 e5 91 e0 c7 df"),
    0x1514: (".midi_out_hook", "80 91 22 01"),
    0x15F0: (".midi_in_wait_hook", "ca 94 8f ef"),
    0x1632: (".midi_in_realtime_hook", "0e 94 6a 01 8e 01 0f 5f 1f 4f"),
    0x183A: (".midi_in_event_hook", "0e 94 6a 01"),
}


# Byte addresses of the stock continuation instructions. Resolve relative
# branches after placing the hook sections at their final addresses.
CONTINUATIONS = {
    "DECODE_OUT_EVENT": 0x156e,
    "SERVICE_MIDI_IN": 0x15a4,
    "MIDI_IN_LOOP": 0x15f0,
    "MIDI_IN_LOOP_CONTINUE": 0x15f4,
    "MIDI_IN_LOOP_FINISH": 0x1840,
}


def read_hex(image: bytes) -> dict[int, int]:
    flash = {}
    lines = image.splitlines()
    if not lines or lines[-1] != b":00000001FF":
        raise ValueError("Missing Intel HEX end record")
    for line in lines[:-1]:
        if not line.startswith(b":"):
            raise ValueError("Invalid Intel HEX record prefix")
        record = bytes.fromhex(line[1:].decode("ascii"))
        if len(record) < 5 or record[0] != len(record) - 5 or sum(record) & 0xFF:
            raise ValueError("Invalid Intel HEX record length or checksum")
        if record[3] != 0:
            raise ValueError("Expected only 16-bit Intel HEX data records")
        address = int.from_bytes(record[1:3], "big")
        for offset, value in enumerate(record[4:-1]):
            if address + offset in flash:
                raise ValueError("Overlapping Intel HEX data records")
            flash[address + offset] = value
    return flash


def write_hex(flash: bytes) -> bytes:
    lines = []
    for address in range(0, len(flash), 16):
        data = flash[address : address + 16]
        record = bytes([len(data)]) + address.to_bytes(2, "big") + b"\0" + data
        record += bytes([-sum(record) & 0xFF])
        lines.append(":" + record.hex().upper())
    lines.append(":00000001FF")
    return ("\n".join(lines) + "\n").encode("ascii")


def build_patch() -> dict[int, int]:
    source = Path(__file__).resolve().with_name("usb_flow.S")
    with tempfile.TemporaryDirectory(prefix="megacommand-usb-flow-") as temp:
        obj = Path(temp) / "usb_flow.o"
        elf = Path(temp) / "usb_flow.elf"
        ihex = Path(temp) / "usb_flow.hex"
        subprocess.run(
            ["avr-as", "-mmcu=atmega16u2", "-o", obj, source],
            check=True,
        )
        subprocess.run(
            [
                "avr-ld",
                "-m",
                "avr35",
                *(f"--defsym={symbol}=0x{address:x}"
                  for symbol, address in CONTINUATIONS.items()),
                *(f"--section-start={section}=0x{address:x}"
                  for address, (section, _) in HOOKS.items()),
                f"--section-start=.usb_flow=0x{PATCH_START:x}",
                "-o",
                elf,
                obj,
            ],
            check=True,
        )
        subprocess.run(
            [
                "avr-objcopy",
                "-O",
                "ihex",
                *(argument for section, _ in HOOKS.values()
                  for argument in ("-j", section)),
                "-j",
                ".usb_flow",
                elf,
                ihex,
            ],
            check=True,
        )
        # GNU objcopy can emit an entry point record. The AVR starts at reset
        # vector zero and the patch overlays only the data records.
        data_records = [
            line for line in ihex.read_bytes().splitlines() if line[7:9] == b"00"
        ]
        return read_hex(b"\n".join([*data_records, b":00000001FF"]))


def apply_patch(original: bytes, patch: dict[int, int]) -> bytes:
    flash = read_hex(original)
    if set(flash) != set(range(PATCH_START)):
        raise ValueError("Unexpected application flash extent")

    hook_addresses = set()
    for address, (_, original_bytes) in HOOKS.items():
        expected = bytes.fromhex(original_bytes)
        addresses = range(address, address + len(expected))
        if bytes(flash[at] for at in addresses) != expected:
            raise ValueError(f"Unexpected instructions at 0x{address:04x}")
        hook_addresses.update(addresses)

    if {at for at in patch if at < PATCH_START} != hook_addresses:
        raise ValueError("Patch changes flash outside the hook sites")
    patch_end = max(patch) + 1
    if not PATCH_START < patch_end <= APP_END:
        raise ValueError("Patch must end before the boot section")

    flash.update(patch)
    if set(flash) != set(range(patch_end)):
        raise ValueError("Patch introduces a hole in the flash image")
    return write_hex(bytes(flash[at] for at in range(patch_end)))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=Path, required=True, help="MCL USB DFU ZIP file")
    parser.add_argument("--output", type=Path, default=OUTPUT, help="output HEX file")
    args = parser.parse_args()

    try:
        with ZipFile(args.bundle) as bundle:
            original = bundle.read(DIY_IMAGE)
        if hashlib.sha256(original).hexdigest() != DIY_SHA256:
            raise ValueError("Expected the original MegaCommand DIY USB MIDI 1.0.4 HEX")

        result = apply_patch(original, build_patch())
        checksum = hashlib.sha256(result).hexdigest()
        if checksum != OUTPUT_SHA256:
            raise ValueError("USB firmware differs from the reference build. Use the pinned AVR toolchain.")

        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(result)
    except KeyError:
        parser.exit(1, f"Missing firmware image in bundle: {DIY_IMAGE}\n")
    except (BadZipFile, OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"USB firmware build failed: {error}\n")
    print(f"{checksum}  {args.output}")


if __name__ == "__main__":
    main()
