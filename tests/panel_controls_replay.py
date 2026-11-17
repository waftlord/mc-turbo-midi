#!/usr/bin/env python3
"""Exercise the production USB limit controls and the 2560 panel shift register."""
import argparse

from paired_avr_replay import assert_bytes, command, replay_args, speed_report, start_bench
from panel_replay import capture_oled, check_port


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    replay_args(parser)
    bench, _ = start_bench(parser, parser.parse_args())
    screen = capture_oled(bench.main)
    bench.run(245)  # Finish the startup splash and both complete OLED columns.
    bench.status(1)  # Asserts each read-only reply and zero DIN negotiation traffic.
    check_port(screen, 0, '1X', 'READY')
    check_port(screen, 1, '1X', 'READY')

    set_one = bytes.fromhex('f0 00 20 3c 04 00 03 01 f7')
    bench.send_host((2, bench.packetize(set_one)))
    bench.run(30)
    assert_bytes(bench.read_host(2), speed_report(1), 'host set P2 cap')
    assert not bench.read_host(1) and not bench.output_din(1) and not bench.output_din(2)
    check_port(screen, 0, '1X', 'READY')
    check_port(screen, 1, '1X', 'MAX 1X')
    bench.clear()

    bench.run(390)
    bench.main.buttons = 0xff & ~(1 << 4)  # SAVE: P1's default 8x cap cycles to 1x.
    bench.run(36)
    bench.main.buttons = 0xff
    bench.run(24)
    assert_bytes(bench.read_host(1), speed_report(1), 'SAVE changed P1 cap')
    assert not bench.read_host(2) and not bench.output_din(1) and not bench.output_din(2)
    check_port(screen, 0, '1X', 'MAX 1X')
    check_port(screen, 1, '1X', 'MAX 1X')
    bench.clear()

    bench.run(1100)  # The earlier USB edit has expired; the later SAVE edit is visible.
    check_port(screen, 0, '1X', 'MAX 1X')
    check_port(screen, 1, '1X', 'READY')
    bench.main.buttons = 0xff & ~(1 << 7)  # LOAD: P2 cycles from 1x to 2x.
    bench.run(36)
    bench.main.buttons = 0xff
    assert not bench.output_din(1) and not bench.read_host(1)
    assert_bytes(bench.output_din(2), command(0x10), 'LOAD requested Turbo only on P2')
    check_port(screen, 0, '1X', 'MAX 1X')
    check_port(screen, 1, '1X', 'MAX 2X', 'LINKING')
    bench.run(500)
    check_port(screen, 0, '1X', 'READY')
    check_port(screen, 1, '1X', 'MAX 2X', 'NO TURBO')
    assert_bytes(bench.read_host(2), speed_report(1), 'P2 reports 1x after missing peer')
    bench.run(1050)
    check_port(screen, 0, '1X', 'READY')
    check_port(screen, 1, '1X', '', 'NO TURBO')

    # Both actual firmware paths remain usable at the settled rate.
    bench.clear()
    for port in (1, 2):
        bench.input_din(port, bytes((0x90, 36 + port, 100)))
        bench.send_host((port, bytes((9, 0x91, 60 + port, 80))))
    bench.run(20)
    for port in (1, 2):
        assert_bytes(bench.read_host(port), bytes((0x90, 36 + port, 100)), 'DIN input after controls')
        assert_bytes(bench.output_din(port), bytes((0x91, 60 + port, 80)), 'DIN output after controls')
    bench.assert_no_loss()
    print('Read-only READY, P2 USB cap, independent SAVE/LOAD previews and expiry, '
          'P2-only negotiation, and bidirectional notes on both ports passed.')


if __name__ == '__main__':
    main()
