#!/usr/bin/env python3
"""Measure USB OUT admission to DIN completion, DIN gaps and whole-burst latency."""
import argparse
from dataclasses import dataclass
import json
from pathlib import Path
import shutil

from paired_avr_replay import Bench, CYCLES_PER_MS, assert_bytes, byte_cycles
from profile_avr import TimedOutBanks, event_times, microseconds


@dataclass(frozen=True)
class Case:
    rates: tuple  # Turbo protocol codes 1, 2, 4 and 7 select 1x, 2x, 4x and 8x.
    sizes: tuple  # Exact musical bytes on each USB OUT cable.
    input_mask: int  # Emit distinct simultaneous DIN input on each selected port.
    stall_ms: int
    in_poll_phase_us: int
    lead_ms: int = 0  # Give Port 1's first three USB OUT banks a head start.


CASES = {
    'small1':     Case((1, 1), (63, 0),      0,   0,   0),
    'dual2':      Case((2, 2), (3075, 3075), 3, 128, 170),
    'mixed1_8':   Case((1, 7), (1176, 8193), 3, 128, 780),
    'mixed4_8':   Case((4, 7), (2049, 8193), 3,  30, 260),
    'solo8':      Case((7, 1), (1176, 0),    1,   0, 380),
    'singlelong': Case((1, 7), (0, 8193),    2,   0, 920),
    'bothshort':  Case((7, 7), (1176, 1176), 3,   0, 710),
    'mixed':      Case((2, 7), (2049, 8193), 3,  30, 310),
    'reverse':    Case((7, 2), (8193, 2049), 3,  30, 870),
    'outonly':    Case((7, 7), (8193, 8193), 0,   0, 490),
    'contended':  Case((7, 7), (8193, 8193), 3, 128, 120),
    'repeat':     Case((7, 7), (8193, 8193), 3, 128, 910),
    'longstress': Case((7, 7), (24579, 24579), 3, 128, 120),
    'staggered':  Case((7, 7), (8193, 8193), 3, 128, 540, 4),
}
QUICK = ('solo8', 'bothshort', 'mixed', 'contended', 'staggered')


def record(size, tag):
    if not size:
        return b''
    payload = bytes((tag + i * 31 + (i >> 3)) & 127 for i in range(size - 4))
    return bytes((0xf0, 0x7d, tag)) + payload + b'\xf7'


def queue_host(bench, outputs, lead_ms):
    packets = [bench.packetize(data) if data else b'' for data in outputs]
    if lead_ms:
        # 192 USB-MIDI bytes = 144 musical bytes on P1, before the mixed banks.
        bench.send_host((1, packets[0][:192]))
        bench.run(lead_ms)
        packets[0] = packets[0][192:]
    bench.send_host(*[(p, events) for p, events in enumerate(packets, 1) if events])


def measure(images, case, objdump):
    bench = Bench(images, objdump)
    for port, rate in enumerate(case.rates, 1):
        if rate > 1:
            bench.negotiate(rate, (port,))
    bench.clear()
    bench.usb.next_poll = bench.usb.cycles + 16 * case.in_poll_phase_us
    bench.usb.pause_in_until = bench.usb.cycles + case.stall_ms * CYCLES_PER_MS
    outputs = [record(size, 10 + 2 * i) for i, size in enumerate(case.sizes)]
    inputs = [record(size, 11 + 2 * i) if case.input_mask & (1 << i) else b''
              for i, size in enumerate(case.sizes)]
    for port, (data, rate) in enumerate(zip(inputs, case.rates), 1):
        if data:
            bench.input_din(port, data)
        if rate > 1:
            queue = bench.main.inputs[port]
            last = queue[-1][0] if queue else bench.main.cycles
            queue.extend((last + ms * CYCLES_PER_MS, 0xfe, 0) for ms in range(120, 6000, 120))

    admitted = []
    bench.usb.host_out = TimedOutBanks(bench.usb, admitted)
    queue_host(bench, outputs, case.lead_ms)
    expected_in = sum(len(bench.packetize(data)) for data in inputs)
    for _ in range(160):
        bench.run(25)
        if len(bench.usb.in_wire) >= expected_in and all(
            not data or bench.output_din(p, remove_sense=True).endswith(b'\xf7')
            for p, data in enumerate(outputs, 1)
        ):
            break
    bench.run(5)
    bench.assert_no_loss()
    bench.check_usb_banks()
    result = {}
    for i, data in enumerate(outputs):
        port = i + 1
        remove_sense = case.rates[i] > 1
        assert_bytes(bench.output_din(port, remove_sense), data, f'P{port} DIN OUT')
        assert_bytes(bench.read_host(port), inputs[i], f'P{port} USB IN')
        if not data:
            continue
        wire = [t for t, v in bench.main.outputs[port] if not remove_sense or v != 0xfe]
        arrivals = event_times(admitted, i)
        assert len(arrivals) == len(wire)
        gaps = [b - a for a, b in zip(wire, wire[1:])]
        period = byte_cycles(bench.divisor(port))
        assert min(gaps) >= period, (port, min(gaps), period)
        # An uninterrupted wire with first byte completing at S would complete
        # byte i at S + i*T. Its USB bank has to arrive at least T earlier.
        # This lower bound ignores all firmware and transport processing.
        earliest = max(t + (1 - idx) * period for idx, t in enumerate(arrivals))
        result[port] = {
            'first_ms': round((wire[0] - arrivals[0]) / CYCLES_PER_MS, 3),
            'no_idle_bound_ms': round((earliest - arrivals[0]) / CYCLES_PER_MS, 3),
            'gap_us': round(max(gaps) / 16, 3),
            'p99_gap_us': microseconds(gaps)['p99'],
            'over_1ms': sum(gap > CYCLES_PER_MS for gap in gaps),
            'finish_ms': round((wire[-1] - arrivals[0]) / CYCLES_PER_MS, 3),
        }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', type=Path, default=Path(__file__).resolve().parents[1] / 'build')
    parser.add_argument('--objdump', default=shutil.which('avr-objdump') or
                        str(Path.home() / '.platformio/packages/toolchain-atmelavr/bin/avr-objdump'))
    parser.add_argument('--full', action='store_true', help='run every case')
    parser.add_argument('--baseline', type=Path, help='JSONL captured from this sweep; adds per-port deltas')
    parser.add_argument('--max-first-ms', type=float,
                        help='fail when an active port starts later than this bound')
    parser.add_argument('--max-gap-us', type=float,
                        help='fail when any adjacent payload bytes exceed this gap')
    parser.add_argument('cases', nargs='*', metavar='CASE', help=', '.join(CASES))
    args = parser.parse_args()
    if args.full and args.cases:
        parser.error('choose --full or individual cases')
    reference = {}
    if args.baseline:
        for line in args.baseline.read_text().splitlines():
            reference.update(json.loads(line))
    cases = CASES if args.full else args.cases or QUICK
    for name in cases:
        if name not in CASES:
            parser.error(f'unknown case {name}')
        if args.baseline and name not in reference:
            parser.error(f'{args.baseline} has no result for {name}')
        result = measure(args.images, CASES[name], args.objdump)
        report = {name: result}
        if args.baseline:
            report['delta_from_baseline'] = {
                p: {key: round(value - reference[name][str(p)][key], 3)
                    for key, value in timing.items()} for p, timing in result.items()}
        print(json.dumps(report), flush=True)
        for port, timing in result.items():
            for metric, limit in (('first_ms', args.max_first_ms), ('gap_us', args.max_gap_us)):
                if limit is not None and timing[metric] > limit:
                    parser.exit(1, f'{name} P{port} {metric} = {timing[metric]} exceeds {limit}\n')


if __name__ == '__main__':
    main()
