#!/usr/bin/env python3
"""Measure an activity-only repaint, then check the compiled OLED fault display."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from avr_model import load_hex, MainAvr

ROOT = Path(__file__).resolve().parents[1]


def capture_oled(avr):
    """Observe the SSD1305 page/column commands and data on the modeled SPI pins."""
    screen = [bytearray(128) for _ in range(4)]
    page = column = 0
    write = avr.write

    def observe(address, value):
        nonlocal page, column
        if address == 0x4e and not avr.mem[0x10b] & 0x80:
            if avr.mem[0x10b] & 0x20:
                screen[page][column] = value
                column = (column + 1) % 128
            elif value < 16:
                column = (column & 0xf0) | value
            elif value < 32:
                column = (column & 15) | ((value & 15) << 4)
            elif 0xb0 <= value <= 0xb3:
                page = value - 0xb0
        write(address, value)

    avr.write = observe
    return screen


class PanelModel(MainAvr):
    def __init__(self, image):
        super().__init__(image, page=False)
        self.screen = capture_oled(self)

    def through(self, stage):
        while self.mem[0x3e] != stage and self.cycles < 2_000_000:
            self.step()
        assert self.mem[0x3e] == stage, 'panel probe timed out'


# Reference bitmaps from the panel's 5x7 font, independent of the image
# being tested; compare the bytes actually written to the OLED.
FONT = {
    ' ': '0000000000', '1': '00427f4000', '2': '4261514946', '4': '1814127f10', '8': '3649494936',
    'A': '7e1111117e', 'B': '7f49494936', 'D': '7f4141221c', 'E': '7f49494941', 'F': '7f09090901',
    'G': '3e4149497a', 'I': '00417f4100', 'K': '7f08142241', 'L': '7f40404040', 'M': '7f020c027f',
    'N': '7f0408107f', 'O': '3e4141413e', 'P': '7f09090906', 'R': '7f09192946', 'S': '4649494931',
    'T': '01017f0101', 'U': '3f4040403f', 'W': '3f4038403f', 'X': '6314081463', 'Y': '0708700807',
}


def pixels(text):
    return bytes([0]).join(bytes.fromhex(FONT[ch]) for ch in text)


def check_port(screen, port, rate, status, detail=''):
    base = 64 * port
    for page in (0, 1):
        # Doubled glyphs in the upper two OLED pages; two columns per source column.
        expanded = bytearray()
        for column in pixels(rate):
            nibble = (column >> (4 * page)) & 15
            dots = sum(3 << (bit * 2) for bit in range(4) if nibble & (1 << bit))
            expanded.extend((dots, dots))
        assert screen[page][base + 5:base + 5 + len(expanded)] == expanded, f'P{port+1} settled {rate}'
    # Exclude only the fixed inter-port divider at column 63.
    middle = bytearray(63)
    for x, text in ((4, f'P{port+1}'), (22, status)):
        data = pixels(text)
        middle[x:x+len(data)] = bytes(value << 1 for value in data)
    assert screen[2][base:base+63] == middle, f'P{port+1} main label {status!r}'
    bottom = bytearray(63)
    data = pixels(detail)
    x = (64 - len(data)) // 2
    bottom[x:x+len(data)] = data
    assert screen[3][base:base+63] == bottom, f'P{port+1} detail {detail!r}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'src')
    args = parser.parse_args()
    # Older sources can still be profiled with the same driver before the
    # display assertions. Their status type has no cap preview field.
    preview_flag = ['-DPANEL_HAS_PREVIEW'] if 'previewLimit' in (args.source / 'Panel.h').read_text() else []
    cc = shutil.which('avr-g++') or Path.home() / '.platformio/packages/toolchain-atmelavr/bin/avr-g++'
    bindir = Path(cc).parent
    with tempfile.TemporaryDirectory() as temp:
        elf = Path(temp) / 'panel.elf'
        image = elf.with_suffix('.hex')
        subprocess.run([cc, *preview_flag, '-Os', '-std=gnu++11', '-mmcu=atmega2560', '-DF_CPU=16000000UL',
                        '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-I', args.source,
                        ROOT / 'tests/panel_probe.cpp', args.source / 'Panel.cpp', '-o', elf], check=True)
        subprocess.run([bindir / 'avr-objcopy', '-O', 'ihex', '-R', '.eeprom', elf, image], check=True)
        avr = PanelModel(load_hex(image, bindir / 'avr-objdump', 'avr:6'))
    avr.through(1)
    start = avr.cycles
    avr.through(2)
    print(f'First complete status draw: {avr.cycles - start} modeled cycles.')
    avr.through(3)
    healthy = [row[:] for row in avr.screen]
    assert avr.mem[0x2e] & 0x30 == 0x20, 'P1 at 4x must light only its own LED'
    start = avr.cycles
    before = len(avr.oled_data) + len(avr.oled_commands)
    avr.through(4)
    first_service = avr.cycles - start
    avr.through(5)
    sent = len(avr.oled_data) + len(avr.oled_commands) - before
    print(f'One input activity edge: first service {first_service} modeled cycles, {sent} SPI bytes to settle.')
    check_port(healthy, 0, '4X', 'TURBO')
    check_port(healthy, 1, '1X', 'READY')
    assert sent <= 7, 'a single activity indicator required a full redraw'
    expected = [row[:] for row in healthy]
    expected[0][51:53] = bytes((0x7c, 0x7c))
    assert avr.screen == expected, 'unexpected pixels changed for DIN 1 input'
    for stage in range(6, 9):
        before = len(avr.oled_data) + len(avr.oled_commands)
        avr.through(stage)
        assert len(avr.oled_data) + len(avr.oled_commands) - before <= 7, 'activity tick exceeded one indicator'
    for page, column in ((1, 51), (0, 115), (1, 115)):
        expected[page][column:column + 2] = bytes((0x7c, 0x7c))
    assert avr.screen == expected, 'missing or crossed I/O indicators'

    # Both ports exercise the same steady, negotiating and failed states.
    # A cap preview can toggle while every other field remains unchanged.
    states = [
        (9, ('READY', ''), ('TURBO', '')),
        (10, ('MAX 8X', ''), ('MAX 2X', '')),
        (11, ('READY', ''), ('TURBO', '')),
        (12, ('MAX 8X', 'LINKING'), ('', 'NO TURBO')),
        (14, ('', 'NO TURBO'), ('MAX 2X', 'LINKING')),
        (15, ('', 'WAIT USB'), ('', 'WAIT USB')),
        (16, ('MAX 8X', 'DIN ERR'), ('MAX 2X', 'USB ERR')),
        (17, ('', 'RX FULL'), ('', 'TX FULL')),
        (18, ('READY', ''), ('TURBO', '')),
        (19, ('MAX 8X', ''), ('TURBO', '')),
        (20, ('MAX 8X', ''), ('MAX 2X', '')),
        (21, ('MAX 8X', ''), ('MAX 2X', '')),
        (22, ('MAX 8X', ''), ('MAX 2X', '')),
        (23, ('MAX 8X', ''), ('TURBO', '')),
        (24, ('MAX 8X', ''), ('TURBO', '')),
        (25, ('READY', ''), ('TURBO', '')),
        (26, ('READY', ''), ('MAX 2X', '')),
        (27, ('READY', ''), ('MAX 2X', '')),
        (28, ('READY', ''), ('TURBO', '')),
    ]
    # A single edit (19) or expiry (23) needs at most three transactions
    # of 16 framebuffer bytes plus three command bytes. Stages 10 and 11
    # exercise simultaneous preview changes on both independent columns.
    preview_spi_budget = {10: 114, 11: 114, 19: 57, 23: 57}
    for stage, left, right in states:
        before = len(avr.oled_data) + len(avr.oled_commands)
        avr.through(stage)
        if stage in preview_spi_budget:
            sent = len(avr.oled_data) + len(avr.oled_commands) - before
            assert sent <= preview_spi_budget[stage], f'cap preview stage {stage} wrote {sent} SPI bytes'
        check_port(avr.screen, 0, '1X', *left)
        check_port(avr.screen, 1, '4X', *right)
        if stage == 12:
            assert avr.mem[0x2e] & 0x20, 'negotiating LED must blink on'
            avr.through(13)
            assert not avr.mem[0x2e] & 0x20, 'negotiating LED must blink off'
    print('Both OLED columns: settled rate, four activity indicators, READY/TURBO, cap preview, '
          'LINKING, four fault labels, peer loss, USB wait and independent 1500 ms preview expiry passed.')


if __name__ == '__main__':
    main()
