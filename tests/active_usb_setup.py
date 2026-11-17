#!/usr/bin/env python3
"""Read the device descriptor periodically during the paired dual-DIN stress test."""

import argparse
from collections import deque

from avr_model import SPL
from paired_avr_replay import CYCLES_PER_MS, replay_args, start_bench


class UsbStackGuard:
    # Stock BSS and retained state use 0x100..0x29d.
    static_end = 0x29e

    def __init__(self, usb):
        self.usb = usb
        self.lowest = usb.sp
        self.push = usb.push
        self.write = usb.write
        usb.push = self.guard_push
        usb.write = self.guard_write

    def observe(self):
        self.lowest = min(self.lowest, self.usb.sp)
        assert self.gap >= 0, f'16U2 stack crossed reserved SRAM at SP {self.lowest:#x}'

    def guard_push(self, value):
        assert self.usb.sp >= self.static_end, f'16U2 stack write into reserved SRAM at {self.usb.sp:#x}'
        self.push(value)
        self.observe()

    def guard_write(self, address, value):
        self.write(address, value)
        if address == SPL:
            self.observe()

    @property
    def gap(self):
        return self.lowest + 1 - self.static_end


class DescriptorReads:
    def __init__(self, usb):
        self.usb = usb
        self.expected = usb.flash_bytes(0x117, 18)
        self.deadline = usb.cycles + 10 * CYCLES_PER_MS
        self.start = None
        self.responses = 0
        self.advance = usb.advance
        usb.advance = self.step

    def completed(self):
        if self.start is None:
            return
        reply = b''.join(self.usb.control_in[self.start:])
        assert reply == self.expected, f'descriptor response {self.responses + 1}: {reply.hex()}'
        self.responses += 1
        self.start = None

    def step(self, receiver):
        usb = self.usb
        if usb.cycles >= self.deadline:
            self.completed()
            ep0 = usb.endpoints[0]
            assert ep0.enabled and not ep0.setup
            self.start = len(usb.control_in)
            ep0.data = deque(bytes.fromhex('80 06 00 01 00 00 12 00'))
            ep0.rxouti = False
            ep0.setup = ep0.txini = True
            usb.control_response_bytes = len(self.expected)
            self.deadline += 31 * CYCLES_PER_MS
        # Hardware raises RXSTPI regardless of SREG/RXSTPE. The normal
        # endpoint-vector arbitration decides when firmware can service it.
        self.advance(receiver)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    replay_args(parser)
    args = parser.parse_args()
    bench, _ = start_bench(parser, args)
    bench.negotiate(7)
    stack = UsbStackGuard(bench.usb)
    reader = DescriptorReads(bench.usb)
    record = b'\xf0\x7d' + bytes(range(128)) * 188 + b'\xf7'
    bench.replay([record], (1, 2), pause_ms=128, realtime=True)
    reader.deadline = bench.usb.cycles + 10 * CYCLES_PER_MS
    bench.run(4)
    reader.completed()
    assert reader.responses > 10
    print(f'{reader.responses} correct descriptor reads during stress, '
          f'minimum 16U2 stack-to-static gap {stack.gap} bytes')


if __name__ == '__main__':
    main()
