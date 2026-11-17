#!/usr/bin/env python3
"""Check both MIDI sessions at USB reconfiguration and EP0 after address changes."""
import argparse
from collections import deque

from paired_avr_replay import assert_bytes, replay_args, speed_report, start_bench


def setup(bench, request, value, expected=None):
    usb = bench.usb
    ep0 = usb.endpoints[0]
    assert ep0.enabled and not ep0.setup
    start = len(usb.control_in)
    ep0.data = deque((0x80 if expected is not None else 0, request, value, 0, 0, 0,
                      len(expected) if expected is not None else 0, 0))
    ep0.rxouti = False
    ep0.setup = ep0.txini = True
    usb.control_response_bytes = len(expected) if expected is not None else None
    # Service the pending flag according to SREG, UEIENX and actual polling.
    bench.run(5)
    assert usb.control_in[start:] == [expected or b''], f'EP0 request {request} value {value}'


def configuration(bench, value):
    setup(bench, 9, value)
    setup(bench, 8, 0, bytes((value,)))


def check(bench, path):
    bench.clear()
    prefixes = {port: bytes((0xf0, 0x7d, port, 1, 2, 3)) for port in (1, 2)}
    for port, prefix in prefixes.items():
        bench.input_din(port, prefix)
        bench.send_host((port, bytes((4,)) + prefix[:3] + bytes((4,)) + prefix[3:]))
    bench.run(180)
    for port, prefix in prefixes.items():
        assert_bytes(bench.read_host(port), prefix, f'P{port} open USB IN SysEx')
        assert_bytes(bench.output_din(port), prefix, f'P{port} open DIN OUT SysEx')
    bench.clear()
    losses = bench.usb.link_losses
    if path != 'reselect':
        configuration(bench, 0)
        if path == 'new-address':
            # Start again at address zero. A subsequent setup must be
            # serviced while the configured MIDI foreground is stopped.
            for address in (0, 5):
                setup(bench, 5, address)
                assert bench.usb.mem[0xe3] & 0x7f == address
    configuration(bench, 1)
    bench.run(350, lambda: bench.usb.mem[0x284] == 1)
    assert bench.usb.mem[0x284] == 1, 'USB transport failed to reconnect'
    bench.run(25)
    for port in (1, 2):
        note = bytes((0x90, 0x30 + port, 0x40))
        bench.send_host((port, bytes.fromhex('04 56 57 58 05 f7 00 00 09') + note))
        bench.input_din(port, bytes((0x57, 0x58, 0xf7)) + note)
    bench.run(40)
    for port in (1, 2):
        note = bytes((0x90, 0x30 + port, 0x40))
        assert_bytes(bench.output_din(port), b'\xf7' + note, f'{path} P{port} DIN OUT')
        assert_bytes(bench.read_host(port).replace(speed_report(1), b''), note, f'{path} P{port} USB IN')
    assert bench.usb.link_losses > losses, 'old transport session survived a configuration change'
    assert not any(bench.main.overruns) and not bench.usb.overruns
    print(f'{path}: fresh session, one DIN recovery EOX per port, orphan suffixes discarded, '
          'clean notes exact in both directions', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    replay_args(parser)
    bench, _ = start_bench(parser, parser.parse_args())
    bench.status(1)
    for path in ('deconfigure', 'reselect', 'new-address'):
        check(bench, path)


if __name__ == '__main__':
    main()
