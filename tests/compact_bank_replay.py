#!/usr/bin/env python3
"""Exercise compiled DIN-to-USB bank assembly at ring and realtime boundaries."""

import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile

from paired_avr_replay import Bench, PROJECT, UARTS, assert_bytes
from profile_avr import elf_symbols, word
from usb_transport_replay import wire_frame


def bank_symbols(elf, objdump, bench):
    # Firmware addresses come from the linked program that produced the HEX.
    with tempfile.TemporaryDirectory() as directory:
        binary = Path(directory) / 'main.bin'
        subprocess.run([objdump.with_name('avr-objcopy'), '-O', 'binary', '-R', '.eeprom',
                        elf, binary], check=True)
        flash = bench.main.flash
        if binary.read_bytes() != bytes(flash.get(i, 0) for i in range(max(flash) + 1)):
            raise ValueError('ATmega2560 ELF and HEX flash contents differ')
    return elf_symbols(elf, objdump.with_name('avr-nm'))


def queue_events(mem, queue, tail, events):
    # Queue stores its backing pointer, mask, head and tail as uint16_t.
    base, mask = word(mem, queue), word(mem, queue + 2)
    assert mask == 8191 and len(events) == 32
    for i, byte in enumerate(events):
        mem[base + ((tail + i) & mask)] = byte
    head = (tail + len(events)) & mask
    mem[queue + 4:queue + 8] = head.to_bytes(2, 'little') + tail.to_bytes(2, 'little')


def check_banks(bench, symbols):
    def symbol(name):
        return symbols['(anonymous namespace)::' + name][0] & 0xffff

    main = bench.main
    mem = main.mem
    service_pc = symbol('serviceUsbOut(unsigned long)')
    bank_credits = symbol('returnBatchCredits')
    next_port = symbol('nextPort')
    first_port, ports_size = symbols['(anonymous namespace)::ports']
    # Each Port begins with the DIN queue followed by the eight-byte host queue.
    queues = [(first_port & 0xffff) + 8 + i * (ports_size // 2) for i in range(2)]
    uart = symbols['usbUart'][0] & 0xffff
    tx_head, tx_tail = uart + 2, uart + 3
    tx_buffer = uart + 5 + 256  # Following the UART flags and receive ring.

    def idle_at_service():
        return (main.pc == service_pc and mem[bank_credits] == 2 and mem[tx_head] == mem[tx_tail]
                and main.tx_data[0] is None and main.tx_shift[0] is None
                and all(word(mem, q + 4) == word(mem, q + 6) for q in queues))

    def wait_idle():
        bench.run(30, idle_at_service)
        assert idle_at_service(), 'Return queues, batch credits or link UART did not settle'

    # Seed complete, already-packetized events at a quiescent foreground entry.
    # The compiled firmware performs all encoding, wire I/O and USB bank commits.
    for starting_port in (0, 1):
        for tail in (8192 - 32, 8192 - 4):
            for clock_at in range(17):  # 16 puts every event on the compact path.
                wait_idle()
                bench.clear()
                mem[next_port] = starting_port

                # A previous commit is still transmitting when this one is built.
                # Set the write position near the end so candidate frames wrap.
                end = (237 + clock_at) & 255
                prior = wire_frame(7, b'') * 14  # Valid internal heartbeats.
                start = (end - len(prior)) & 255
                for i, byte in enumerate(prior):
                    mem[tx_buffer + ((start + i) & 255)] = byte
                mem[tx_tail], mem[tx_head] = start, end
                mem[UARTS[0][0] + 1] |= 0x20  # Enable the normal data-empty ISR.

                by_port = [bytearray(), bytearray()]
                expected = bytearray()
                for i in range(16):
                    cable = starting_port ^ (i & 1)
                    if i == clock_at:
                        event = bytes((16 * cable + 15, 0xf8, 0, 0))
                    else:
                        # On cable 2, the tagged 7e/7f needs internal wire escaping.
                        event = bytes((16 * cable + 4, 126 + ((i // 2) & 1), 31 + i, 61 - i))
                    by_port[cable].extend(event)
                    expected.extend(event)
                for queue, events in zip(queues, by_port):
                    queue_events(mem, queue, tail, events)

                bench.run(15, lambda: len(bench.usb.in_wire) >= len(expected))
                wait_idle()
                where = f'first cable {starting_port}, queue tail {tail}, clock position {clock_at}'
                assert_bytes(bytes(bench.usb.in_wire), expected, where)
                expected_banks = ([expected[:4 * clock_at], expected[4 * clock_at:]]
                                  if 2 <= clock_at < 16 else [expected])
                assert bench.usb.in_banks == expected_banks, where
                assert mem[next_port] == starting_port, where
                bench.check_usb_banks()
                bench.assert_no_loss()
    print('68 bank schedules passed with exact USB events, bank boundaries, cable rotation and credits.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', type=Path, default=PROJECT / 'build', help='directory with both HEX files')
    parser.add_argument('--elf', type=Path, help='matching 2560 firmware.elf from the PlatformIO build')
    parser.add_argument('--objdump', type=Path, help='path to avr-objdump')
    args = parser.parse_args()
    objdump = args.objdump or Path(shutil.which('avr-objdump') or
                                   Path.home() / '.platformio/packages/toolchain-atmelavr/bin/avr-objdump')
    elf = args.elf or args.images.parent / '.pio/build/megacommand_diy/firmware.elf'
    bench = Bench(args.images, objdump)
    check_banks(bench, bank_symbols(elf, objdump, bench))


if __name__ == '__main__':
    main()
