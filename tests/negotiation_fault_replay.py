#!/usr/bin/env python3
"""Exercise Turbo negotiation with silent, absent, incompatible and disconnected peers."""
import argparse

from paired_avr_replay import (CYCLES_PER_MS, STATUS, assert_bytes, command, replay_args,
                               speed_report, start_bench)


def request(bench, port, limit):
    data = bytes.fromhex('f0 00 20 3c 04 00 03') + bytes((limit, 0xf7))
    bench.send_host((port, bench.packetize(data)))


def check_upgrade_receiver(bench):
    port = 1
    answer = command(0x11, bytes.fromhex('7f 00 00 00'))
    pattern = bytes.fromhex('55 55 55 55 00 00 00 00')
    # Cold-boot default cap, using the same probe as a host Turbo button.
    bench.send_host((port, bench.packetize(bytes.fromhex('f0 00 20 3c 04 00 01 f7'))))
    bench.run(10)
    assert_bytes(bench.output_din(port), command(0x10), 'startup discovery')
    bench.clear()
    bench.input_din(port, answer)
    bench.run(17)
    assert_bytes(bench.output_din(port), command(0x12, bytes((7, 7))), 'try maximum 8x')
    bench.clear()
    bench.input_din(port, command(0x13))
    bench.run(17)
    assert bench.divisor(port) == 3
    assert_bytes(bench.output_din(port), bytes(16) + command(0x14, pattern), '8x test')
    bench.clear()
    # As on the physical receiver, no first test result comes back at 8x.
    bench.run(395)
    expected = command(0x12, bytes((1, 1))) + command(0x10)
    assert_bytes(bench.output_din(port, remove_sense=True), expected, 'rediscover at 1x')
    assert bench.divisor(port) == 31 and speed_report(7) not in bench.read_host(port)
    for incoming, expected in (
        (answer, command(0x12, bytes((4, 4)))),
        (command(0x13), bytes(16) + command(0x14, pattern)),
        (command(0x15, pattern), command(0x16)),
        (command(0x17), b''),
    ):
        bench.clear()
        bench.input_din(port, incoming)
        bench.run(17)
        assert_bytes(bench.output_din(port, remove_sense=True), expected, 'startup handshake')
    assert bench.divisor(port) == 7 and not bench.read_host(port)
    record = bytes.fromhex('f0 7d 21 02 03 04 05 f7')
    bench.send_host((port, bench.packetize(record)))
    bench.clear()
    bench.run(330)
    assert_bytes(bench.output_din(port, remove_sense=True), command(0x10), 'queued data held')
    assert not bench.read_host(port)
    bench.clear()
    bench.input_din(port, answer)
    bench.run(475)
    assert_bytes(bench.output_din(port, remove_sense=True), record, 'queued data released')
    assert_bytes(bench.read_host(port), speed_report(4), 'sustained silent receiver')
    assert bench.divisor(port) == 7
    bench.clear()
    request(bench, port, 1)
    bench.run(390)
    assert_bytes(bench.output_din(port, remove_sense=True), command(0x12, bytes((1, 1))), 'return to 1x')
    assert_bytes(bench.read_host(port), speed_report(1), '1x restored')
    bench.status(1)
    bench.assert_no_loss()
    print('Machinedrum startup: silent 4x receiver confirmed before queued SysEx, returned to 1x', flush=True)


def check_clean(bench):
    # Distinct cable and direction tags make crossing, loss and duplication
    # visible. Running status expands into complete USB channel events.
    bench.clear()
    outputs = []
    for port in (1, 2):
        tag = 0x20 + port
        bench.input_din(port, bytes((0xf0, 0x7d, tag, 1, 2, 0xf7, 0xc0, tag, 0xf8, tag+1)))
        tag += 16
        outputs.append((port, bench.packetize(bytes((0xf0, 0x7d, tag, 3, 4, 0xf7)))
                        + bytes((12, 0xc1, tag, 0, 15, 0xf8, 0, 0, 15, tag+1, 0, 0))))
    bench.send_host(*outputs)
    bench.run(30)
    for port in (1, 2):
        tag = 0x20 + port
        assert_bytes(bench.read_host(port),
                     bytes((0xf0, 0x7d, tag, 1, 2, 0xf7, 0xc0, tag, 0xf8, 0xc0, tag+1)), 'USB IN')
        tag += 16
        assert_bytes(bench.output_din(port, remove_sense=True),
                     bytes((0xf0, 0x7d, tag, 3, 4, 0xf7, 0xc1, tag, 0xf8, tag+1)), 'DIN OUT')
    bench.assert_no_loss()
    bench.clear()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    replay_args(parser)
    args = parser.parse_args()
    bench, _ = start_bench(parser, args)
    bench.status(1)
    for port, peer in ((1, 'absent'), (2, 'incompatible'), (1, 'simultaneous')):
        request(bench, port, 4)
        bench.run(8)
        assert_bytes(bench.output_din(port), command(0x10), 'initial speed request')
        bench.clear()
        if peer == 'incompatible':
            # Only 8x is advertised, above the requested cap.
            bench.input_din(port, command(0x11, bytes((0x20, 0, 0x20, 0))))
        elif peer == 'simultaneous':
            bench.input_din(port, command(0x10))
        bench.run(55)
        expected = command(0x11, bytes((5, 0, 5, 0))) if peer == 'simultaneous' else b''
        assert_bytes(bench.output_din(port), expected, peer)
        assert_bytes(bench.read_host(port), speed_report(1), f'{peer} settled 1x')
        assert not bench.output_din(3 - port) and not bench.read_host(3 - port)
        check_clean(bench)
        print(f'{peer}: both ports delivered next clean SysEx, running status and clock', flush=True)

    # Even a reserved frame received outside its handshake state starts a
    # SysEx on the physical DIN cable. It cancels channel running status.
    for port in (1, 2):
        before = bytes((0x90, 60, 100))
        clean = bytes((0x80, 60, 0))
        bench.input_din(port, before + command(0x13) + bytes((61, 99)) + clean)
        bench.run(12)
        assert_bytes(bench.read_host(port), before + clean, 'running status across consumed DIN control')
        bench.clear()

    bench.negotiate(2, (1,))
    bench.negotiate(4, (2,))
    # Port 1 disconnects. Port 2 continues receiving notes between keepalives.
    for i in range(4):
        bench.input_din(2, bytes((0xfe, 0x90, 50 + i, 100)))
        bench.run(85)
    assert bench.divisor(1) == 31 and bench.divisor(2) == 7
    assert_bytes(bench.read_host(1), speed_report(1), 'disconnected peer rate')
    expected = b''.join(bytes((0x90, 50 + i, 100)) for i in range(4))
    assert_bytes(bench.read_host(2), expected, 'traffic on surviving link')
    check_clean(bench)
    for port in (1, 2):
        bench.send_host((port, bench.packetize(STATUS)))
    bench.run(20)
    assert_bytes(bench.read_host(1), speed_report(1), 'P1 query after peer loss')
    assert_bytes(bench.read_host(2), speed_report(4), 'P2 query after other peer lost')
    print('P1 2x disconnect: P1 recovered at 1x, P2 kept 4x and delivered every note', flush=True)

    # A previously settled link permits an extra retry after returning to 1x.
    # Start the upgrade scenario with both ports at their cold-boot defaults.
    bench, _ = start_bench(parser, args)
    bench.status(1)
    check_upgrade_receiver(bench)


if __name__ == '__main__':
    main()
