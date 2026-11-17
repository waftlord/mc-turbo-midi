#!/usr/bin/env python3
"""Exercise peer collisions, incompatible or absent peers, and active-link loss."""
import argparse

from paired_avr_replay import (CYCLES_PER_MS, STATUS, assert_bytes, command, replay_args,
                               speed_report, start_bench)


def request(bench, port, limit):
    data = bytes.fromhex('f0 00 20 3c 04 00 03') + bytes((limit, 0xf7))
    bench.send_host((port, bench.packetize(data)))


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
    bench, _ = start_bench(parser, parser.parse_args())
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


if __name__ == '__main__':
    main()
