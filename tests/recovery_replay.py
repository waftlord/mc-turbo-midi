#!/usr/bin/env python3
"""Exercise PAGE, serial upload replies and the return to dual MIDI on compiled images."""

import argparse
import functools
import operator
from pathlib import Path
import shutil

from avr_model import load_hex, midi_bytes, packetize
from paired_avr_replay import Bench, CYCLES_PER_MS, PROJECT, byte_cycles, model_classes


USB_IMAGE = 'megacommand-diy-dual-atmega16u2-dfu.hex'
MAIN_IMAGE = 'megacommand-diy-dual-atmega2560-update.hex'
MODE = 0x11B  # Pinned 1.0.4 stock mode and its configured main-loop entry point.
USB_LOOP = 0x1324
TOKEN = bytes.fromhex('a7 3d 91 48 bc 62 15 e3 56 89 d2 6c')


def reply(seq, payload):
    body = bytes((0x1b, seq, len(payload) >> 8, len(payload) & 255, 0x0e)) + payload
    return body + bytes((functools.reduce(operator.xor, body),))


def period(chip, base):
    return byte_cycles(chip.mem[base + 4]) // (2 if chip.mem[base] & 2 else 1)


class RecoveryBench(Bench):
    def __init__(self, previous, candidate, objdump):
        usb, self.main_cpu = model_classes()
        self.serial_in = bytearray()
        self.serial_wire = bytearray()
        self.usb = usb(load_hex(previous / USB_IMAGE, objdump, 'avr:35'))
        self.usb.boot()
        self.candidate = load_hex(candidate / MAIN_IMAGE, objdump, 'avr:6')
        self.main = self.main_cpu(load_hex(previous / MAIN_IMAGE, objdump, 'avr:6'), page=True)
        self.usb.check_endpoint_writes = False  # Stock CDC uses different endpoint flag semantics.
        self.run(600, lambda: self.usb.mem[MODE] == 3 and self.usb.pc == USB_LOOP)
        assert self.usb.mem[MODE] == 3 and self.usb.pc == USB_LOOP, 'PAGE failed to select CDC'
        self.usb.usb_reset()
        self.usb.control(bytes.fromhex('80 06 00 02 00 00 3e 00'), response=self.usb.flash_bytes(0xd9, 62))
        self.usb.control(bytes.fromhex('00 05 05 00 00 00 00 00'))
        self.usb.control(bytes.fromhex('00 09 01 00 00 00 00 00'))
        assert all(self.usb.endpoints[i].enabled for i in (3, 4))
        self.baud(115200)

    def baud(self, baud):
        self.usb.control(bytes.fromhex('21 20 00 00 00 00 07 00'),
                         data=baud.to_bytes(4, 'little') + bytes((0, 0, 8)))

    def run(self, timeout_ms, done=lambda: False):
        usb, main = self.usb, self.main
        deadline = max(usb.cycles, main.cycles) + timeout_ms * CYCLES_PER_MS
        while min(usb.cycles, main.cycles) < deadline and not done():
            if usb.cycles <= main.cycles:
                if usb.mem[MODE] == 3 and usb.host_out:
                    ep = usb.endpoints[4]
                    if ep.enabled and not ep.out_owned:
                        ep.data.extend(usb.host_out.popleft())
                        ep.rxouti = ep.out_owned = True
                if usb.mem[MODE] == 3 and usb.tx_shift is not None and usb.cycles >= usb.tx_end:
                    self.serial_wire.append(usb.tx_shift)
                usb.advance(main.inputs[0])
                # Model a serial host reading the CDC IN endpoint.
                ep = usb.endpoints[3]
                if usb.mem[MODE] == 3 and ep.pending is not None:
                    self.serial_in.extend(ep.pending)
                    ep.pending, ep.txini = None, True
            else:
                main.step()
                if main.outputs[0]:
                    row = main.outputs[0].popleft()
                    # Only matching baud rates deliver valid handoff bytes.
                    if period(usb, 0xc8) == period(main, 0xc0):
                        usb.input.append(row)

    def replies(self, data):
        spacing = period(self.usb, 0xc8)
        start = self.usb.cycles
        self.usb.input.extend((start + i * spacing, byte) for i, byte in enumerate(data, 1))
        self.run(len(data) * spacing // CYCLES_PER_MS + 8)

    def restart(self, page=False):
        when = max(self.main.cycles, self.usb.cycles)
        self.main = self.main_cpu(self.candidate, page=page)
        self.main.cycles = when
        self.main.next_timer = when + CYCLES_PER_MS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', type=Path, default=PROJECT / 'build')
    parser.add_argument('--from-images', type=Path, help='installed pair at PAGE-held startup')
    parser.add_argument('--objdump', default=shutil.which('avr-objdump') or
                        Path.home() / '.platformio/packages/toolchain-atmelavr/bin/avr-objdump')
    args = parser.parse_args()
    previous = args.from_images or args.images
    bench = RecoveryBench(previous, args.images, args.objdump)
    # Carry an ordinary STK500v2 sign-on request across the stock CDC OUT
    # endpoint and USART TX. The bootloader reply is supplied by the fixture.
    sign_on = reply(1, bytes((1,)))
    bench.usb.host_out.append(sign_on)
    bench.run(20)
    assert bytes(bench.serial_wire).endswith(sign_on), 'CDC OUT changed the sign-on request'
    sign_on_reply = reply(1, b'\x01\x00\x08AVRISP_2')
    bench.replies(sign_on_reply)
    bench.run(20)
    assert bytes(bench.serial_in).endswith(sign_on_reply), 'CDC IN changed the sign-on response'
    program = reply(2, bytes.fromhex('13 00'))
    leave = reply(3, bytes.fromhex('11 00'))
    readback = reply(4, b'\x14\x00' + program + leave + TOKEN + b'\x00')

    # A read-back may contain the startup token. An interrupted upload may
    # have a prior good block followed by a failed program or a truncated ACK.
    bench.replies(readback + program + reply(5, b'\x13\xc0') + leave + TOKEN)
    bench.replies(program[:-1] + b'\x00' + leave + TOKEN)
    bench.restart()
    bench.run(850)
    assert bench.usb.mem[MODE] == 3, 'failed upload left serial recovery'

    # A fresh successful upload must still recover without power cycling.
    bench.replies(sign_on_reply + program + leave + readback)
    bench.restart(page=True)
    bench.run(650)
    assert bench.usb.mem[MODE] == 3, 'held PAGE should keep serial recovery'
    bench.replies(program + leave)
    bench.baud(57600)  # A late host line-coding change misses the first token.
    bench.restart()
    bench.run(500)
    assert bench.usb.mem[MODE] == 3
    bench.baud(9600)
    bench.run(1600, lambda: bench.usb.mem[MODE] == 2 and bench.usb.pc == USB_LOOP)
    assert bench.usb.mem[MODE] == 2 and bench.usb.pc == USB_LOOP, 'did not reattach in MIDI mode'
    bench.usb.check_endpoint_writes = True
    bench.usb.configure_midi(check_names=True)
    bench.run(700, lambda: bench.usb.mem[0x284] == 1)
    assert bench.usb.mem[0x284] == 1
    bench.packetize, bench.decode = packetize, midi_bytes
    bench.clear()
    bench.status(1)
    if (previous / USB_IMAGE).read_bytes() != (args.images / USB_IMAGE).read_bytes():
        # The hardware DFU bootloader is outside these application images.
        # Simulate successful replacement and the specified power cycle.
        fresh = Bench(args.images, args.objdump)
        fresh.status(1)
    print('PAGE, bidirectional CDC 115200, failed upload/retry, held PAGE, late baud change, '
          'dual MIDI enumeration and read-only 1x reports passed')


if __name__ == '__main__':
    main()
