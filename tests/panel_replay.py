#!/usr/bin/env python3
"""Measure an activity-only repaint, then check the compiled OLED fault display."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from avr_model import load_hex, MainAvr

ROOT = Path(__file__).resolve().parents[1]


class PanelModel(MainAvr):
    def __init__(self, image):
        super().__init__(image, page=False)
        self.screen = [bytearray(128) for _ in range(4)]
        self.page = self.column = 0

    def write(self, address, value):
        if address == 0x4e and not self.mem[0x10b] & 0x80:
            if self.mem[0x10b] & 0x20:
                self.screen[self.page][self.column] = value
                self.column = (self.column + 1) % 128
            elif value < 16:
                self.column = (self.column & 0xf0) | value
            elif value < 32:
                self.column = (self.column & 15) | ((value & 15) << 4)
            elif 0xb0 <= value <= 0xb3:
                self.page = value - 0xb0
        super().write(address, value)

    def through(self, stage):
        while self.mem[0x3e] != stage and self.cycles < 2_000_000:
            self.step()
        assert self.mem[0x3e] == stage, 'panel probe timed out'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'src')
    args = parser.parse_args()
    cc = shutil.which('avr-g++') or Path.home() / '.platformio/packages/toolchain-atmelavr/bin/avr-g++'
    bindir = Path(cc).parent
    with tempfile.TemporaryDirectory() as temp:
        elf = Path(temp) / 'panel.elf'
        image = elf.with_suffix('.hex')
        subprocess.run([cc, '-Os', '-std=gnu++11', '-mmcu=atmega2560', '-DF_CPU=16000000UL',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-I', args.source,
                        ROOT / 'tests/panel_probe.cpp', args.source / 'Panel.cpp', '-o', elf], check=True)
        subprocess.run([bindir / 'avr-objcopy', '-O', 'ihex', '-R', '.eeprom', elf, image], check=True)
        avr = PanelModel(load_hex(image, bindir / 'avr-objdump', 'avr:6'))
    avr.through(5)
    start = avr.cycles
    avr.through(6)
    print(f'First complete status draw: {avr.cycles - start} modeled cycles.')
    avr.through(1)
    assert avr.mem[0x2e] & 0x30 == 0x20, 'P1 at 4x must light only its own LED'
    start = avr.cycles
    before = len(avr.oled_data) + len(avr.oled_commands)
    baseline = [row[:] for row in avr.screen]
    avr.through(2)
    longest = avr.cycles - start
    avr.through(3)
    sent = len(avr.oled_data) + len(avr.oled_commands) - before
    print(f'One input activity edge: first service {longest} modeled cycles, {sent} SPI bytes to settle.')
    # Only the two interior columns of Port 1's IN indicator change.
    baseline[0][51:53] = b'\x7c\x7c'
    assert avr.screen == baseline
    avr.through(4)
    # Compare leading glyphs for DIN ERR / USB ERR in each 64px port,
    # and the persistent MAX label on the port identifier row.
    for x, letter in [(0, b'\x7f\x41\x41\x22\x1c'), (64, b'\x3f\x40\x40\x40\x3f')]:
        assert letter in avr.screen[3][x:x+64], 'active fault hidden while waiting for USB'
        assert bytes(c << 1 for c in (0x7f, 2, 12, 2, 0x7f)) in avr.screen[2][x:x+64], 'missing MAX'
    # The requested caps are 8x and 1x. P1's larger settled numeral stays 4.
    assert avr.screen[2][46:51] == bytes(c << 1 for c in (0x36, 0x49, 0x49, 0x49, 0x36))
    assert avr.screen[2][110:115] == bytes(c << 1 for c in (0, 0x42, 0x7f, 0x40, 0))
    assert avr.screen[0][5:15] == bytes.fromhex('c0 c0 30 30 0c 0c ff ff 00 00')
    avr.through(7)
    assert avr.mem[0x2e] & 0x30 == 0x20
    avr.through(8)
    assert avr.mem[0x2e] & 0x30 == 0, 'negotiating LED must blink'
    assert sent <= 7, 'a single activity indicator required a full redraw'


if __name__ == '__main__':
    main()
