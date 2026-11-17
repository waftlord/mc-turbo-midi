#!/usr/bin/env python3
"""Compare a passive CoreMIDI capture against a complete four-record dump."""

import argparse
import hashlib
from pathlib import Path

SPEED_REPORT_PREFIX = bytes.fromhex('f0 00 20 3c 04 00 02')


def records(data):
    parts = data.split(b'\xf7')
    assert not parts[-1]
    result = [part + b'\xf7' for part in parts[:-1]]
    assert [len(part) for part in result] == [1176, 1176, 873, 813]
    assert all(part[0] == 0xf0 and all(value < 0x80 for value in part[1:-1]) for part in result)
    return result


def clock_eox_offsets(wire, packet_file):
    """Find an EOX appended to a three-byte packet and followed by clock at its timestamp."""
    previous = None
    end = 0
    result = set()
    for line in packet_file.read_text().splitlines():
        timestamp, offset, size = map(int, line.split())
        assert offset == end and size > 0, f'invalid packet offset {offset}'
        end += size
        if (size == 1 and wire[offset:offset + 1] == b'\xf8' and
                previous == (timestamp, 4) and wire[offset - 1] == 0xf7):
            result.add(offset - 1)
        previous = (timestamp, size)
    assert end == len(wire), 'legacy packet log length differs from captured bytes'
    return result


def compare_legacy(wire, packets, expected):
    clock_eox = clock_eox_offsets(wire, packets)
    extra_ends = []
    cursor = 0
    clock_counts = []
    while (start := wire.find(b'\xf0', cursor)) >= 0:
        cursor = start
        if wire.startswith(SPEED_REPORT_PREFIX, cursor) and wire[cursor + 8:cursor + 9] == b'\xf7':
            cursor += 9
            continue
        target = expected[len(extra_ends) % len(expected)]
        offset = 0
        inserted = []
        while offset < len(target):
            byte = wire[cursor]
            if byte == 0xf7 and offset < len(target) - 1 and cursor in clock_eox:
                inserted.append(offset)
            elif byte < 0xf8:
                assert byte == target[offset], f'legacy mismatch at byte {offset}: 0x{byte:02x}'
                offset += 1
            cursor += 1
        extra_ends.append(inserted)
        clock_counts.append(wire[start:cursor].count(0xf8))
    assert extra_ends and len(extra_ends) % len(expected) == 0, 'expected complete four-record dump(s)'
    return extra_ends, clock_counts


def compare_ump(ump, expected, expected_takes=None):
    actual = []
    current = None
    clocks = 0
    for line in ump.read_text().splitlines():
        _, *encoded = line.split()
        words = iter(int(word, 16) for word in encoded)
        for first in words:
            message_type = first >> 28
            if message_type in (0, 1, 2):
                if current is not None and message_type == 1 and ((first >> 16) & 255) == 0xf8:
                    clocks += 1
                continue
            assert message_type == 3, f'unexpected UMP message type {message_type}'
            second = next(words)
            status = (first >> 20) & 15
            count = (first >> 16) & 15
            payload = ((first & 65535).to_bytes(2, 'big') + second.to_bytes(4, 'big'))[:count]
            assert 0 <= status <= 3 and count <= 6 and all(byte < 0x80 for byte in payload)
            if status in (0, 1):
                assert current is None, 'UMP SysEx start before the preceding end'
                current = bytearray(b'\xf0')
            assert current is not None, 'UMP SysEx continuation without a start'
            current.extend(payload)
            if status in (0, 3):
                record = bytes(current) + b'\xf7'
                if len(record) != 9 or not record.startswith(SPEED_REPORT_PREFIX):
                    actual.append(record)
                current = None
    takes, remainder = divmod(len(actual), len(expected))
    assert expected_takes is None or takes == expected_takes, 'unexpected number of UMP takes'
    assert current is None and takes and not remainder and actual == expected * takes, (
        'UMP SysEx records differ from the control')
    return clocks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('legacy', type=Path)
    parser.add_argument('packets', type=Path)
    parser.add_argument('ump', type=Path, nargs='?')
    parser.add_argument('--allow-clock-eox', action='store_true',
                        help='classify and report the old legacy delivery failure')
    args = parser.parse_args()
    reference = records(args.reference.read_bytes())
    ends, clock_counts = compare_legacy(args.legacy.read_bytes(), args.packets, reference)
    sizes = '/'.join(str(len(record)) for record in reference)
    print(f'Legacy {sizes} bytes each take after excluding clock and any clock-associated EOX')
    for record, (cutoffs, clocks) in enumerate(zip(ends, clock_counts), 1):
        print(f'Legacy record {record}: {clocks} clocks, early EOX at byte offsets {cutoffs}')
    if args.ump:
        clocks = compare_ump(args.ump, reference)
        print(f'UMP {sizes} bytes each take with {clocks} interleaved clocks total')
    for path in (args.reference, args.legacy, args.packets, args.ump):
        if path:
            print(f'{hashlib.sha256(path.read_bytes()).hexdigest()}  {path}')
    if not args.allow_clock_eox:
        assert not any(ends), 'legacy byte delivery inserted early EOX'


if __name__ == '__main__':
    main()
