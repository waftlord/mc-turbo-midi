#!/usr/bin/env python3
"""Build the dual-port USB DFU HEX from the pinned MegaCommand DIY release."""

import argparse
import hashlib
from pathlib import Path
import subprocess
import tempfile
from zipfile import BadZipFile, ZipFile


DIY_IMAGE = "megacmd_usb_dfu_1.0.4/megacommand-diy-usb-midi-1.0.4.hex"
DIY_SHA256 = "5262a2ffeff37b0a6698675dbabb4916096a96fc806c59a37aaf942861c5fa8e"
OUTPUT = Path("build/megacommand-diy-dual-atmega16u2-dfu.hex")
APP_END = 0x3000  # Earliest possible boot section on the ATmega16U2.
PATCH_START = 0x1914

# Expected bytes at the stock entry points replaced by the overlay.
HOOKS = {
    0x002C: (".usb_reset_hook", "0c 94 1c 08"),
    0x005C: (".rx_vector_hook", "0c 94 f8 08"),
    0x0060: (".tx_vector_hook", "0c 94 be 08"),
    0x06F0: (".uart_midi_hook", "62 e1 7a e7 80 e0 90 e0 8b dd"),
    0x0D14: (".midi_string_hook", "99 27 81 30"),
    0x0D5A: (".midi_desc_hook", "85 e6 90 e0"),
    0x0DF2: (".usb_config_hook", "10 93 6d 02"),
    0x0F9E: (".timer_keepalive_hook", "f1 db"),
    0x1212: (".cdc_rx_hook", "60 91 ce 00"),
    0x133A: (".cdc_poll_hook", "9f b7 f8 94 80 91 dc 01 9f bf"),
    0x150C: (".midi_loop_hook", "8e b3 84 30 09 f0 48 c0"),
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
    source = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="megacommand-usb-dual-") as temp:
        temp = Path(temp)
        assembly = temp / "usb_dual_asm.o"
        obj = temp / "usb_dual.o"
        elf = temp / "usb_dual.elf"
        ihex = temp / "usb_dual.hex"
        subprocess.run(
            ["avr-as", "-mmcu=atmega16u2", "-o", assembly, source / "usb_dual.S"],
            check=True,
        )
        subprocess.run(
            ["avr-gcc", "-mmcu=atmega16u2", "-DF_CPU=16000000UL", "-std=gnu11", "-Os", "-Wall",
             "-Wextra", "-Werror", "-ffunction-sections", "-fdata-sections",
             "-o", obj, "-c", source / "usb_dual.c"],
            check=True,
        )
        script = temp / "overlay.ld"
        script.write_text(
            "SECTIONS {\n"
            + "".join(f"  {section} 0x{address:x} : {{ *({section}) }}\n"
                      for address, (section, _) in HOOKS.items())
            + f"  .usb_dual 0x{PATCH_START:x} : {{ *(.text*) *(.progmem*) }}\n"
            + '  .ram : { *(.data*) *(.bss*) *(.rodata*) *(COMMON) }\n'
            + '  ASSERT(SIZEOF(.ram) == 0, "USB overlay defines implicit SRAM")\n'
            + "}\n"
        )
        subprocess.run(
            ["avr-gcc", "-mmcu=atmega16u2", "-nostartfiles", "-Wl,-T," + str(script),
             "-o", elf, assembly, obj],
            check=True,
        )
        subprocess.run(
            ["avr-objcopy", "-O", "ihex",
             *(argument for section, _ in HOOKS.values()
               for argument in ("-j", section)),
             "-j", ".usb_dual", elf, ihex],
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
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(result)
    except KeyError:
        parser.exit(1, f"Missing firmware image in bundle: {DIY_IMAGE}\n")
    except (BadZipFile, OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"USB firmware build failed: {error}\n")
    print(f"{checksum}  {args.output}")


if __name__ == "__main__":
    main()
