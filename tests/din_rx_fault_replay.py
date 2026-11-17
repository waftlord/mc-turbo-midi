#!/usr/bin/env python3
"""Check that an AVR DIN UART fault only abandons the interrupted message."""

import argparse

from avr_model import SREG
from paired_avr_replay import CYCLES_PER_MS, UARTS, assert_bytes, byte_cycles, replay_args, speed_report, start_bench


def receive_without_foreground(bench, port, samples):
    """Run the compiled USART ISR for each sample before resuming the main loop."""
    avr = bench.main
    _, rx_vector, _ = UARTS[port]
    while not avr.mem[SREG] & 0x80:
        avr.step()
    foreground_pc = avr.pc
    fifo = avr.rx_fifo[port]
    assert not fifo
    for sample in samples:
        fifo.append(sample)
        avr.interrupt(rx_vector)
        for _ in range(256):
            avr.step()
            if avr.pc == foreground_pc and avr.mem[SREG] & 0x80:
                break
        else:
            raise AssertionError('USART RX interrupt did not return to foreground')
        assert not fifo, 'USART RX interrupt did not consume the sample'


def check_gap(bench, port, source, clean, cutoff, uart_error):
    bench.clear()
    # Commit complete USB-MIDI groups before the interrupted part arrives.
    before = cutoff - 15
    period = byte_cycles(bench.divisor(port))
    bench.input_din(port, source[:before])
    bench.run(before * period // CYCLES_PER_MS + 20)
    assert_bytes(bench.read_host(port), source[:before], f'port {port} initial prefix')

    # Both valid bytes before the error and a new F0 after the error
    # are already in the RX ring when the foreground first observes it.
    buffered_clean_bytes = 9
    queued = [(byte, 0) for byte in source[before:cutoff]]
    queued.append((source[cutoff], uart_error))
    queued.extend((byte, 0) for byte in source[cutoff + 1:cutoff + 6] + b'\xf7' + clean[:buffered_clean_bytes])
    receive_without_foreground(bench, port, queued)
    bench.input_din(port, clean[buffered_clean_bytes:])
    bench.run(len(clean) * period // CYCLES_PER_MS + 30)

    actual = bench.read_host(port).replace(speed_report(1), b'')
    expected = source[:cutoff] + b'\xf7' + clean
    assert_bytes(actual, expected, f'port {port} UART status 0x{uart_error:02x}')
    assert not bench.read_host(3 - port), 'interrupted receive affected the other cable'
    assert bench.divisor(port) == 31
    bench.assert_no_loss()
    print(f'port {port}: UART status 0x{uart_error:02x}, {cutoff} prefix bytes, '
          f'recovery F7 and {len(clean)} exact bytes in the next message', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    replay_args(parser)
    parser.add_argument('--port', type=int, choices=(1, 2), help='run one port case instead of both')
    args = parser.parse_args()
    bench, records = start_bench(parser, args)
    bench.status(1)
    if args.port in (None, 1):
        check_gap(bench, 1, records[0], records[2], 33, 0x10)
    if args.port in (None, 2):
        check_gap(bench, 2, records[1], records[3], 306, 0x08)


if __name__ == '__main__':
    main()
