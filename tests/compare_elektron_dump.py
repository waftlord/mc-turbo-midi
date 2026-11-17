#!/usr/bin/env python3
"""Check clock-interleaved Machinedrum or Monomachine legacy CoreMIDI dump takes."""

import argparse
from collections import Counter
import hashlib
from pathlib import Path

from compare_coremidi_capture import compare_ump


def read_dump(path):
    frames = []
    current = None
    clocks = 0
    for i, byte in enumerate(path.read_bytes()):
        if byte >= 0xf8:
            clocks += int(current is not None and byte == 0xf8)
        elif byte == 0xf0 and current is None:
            current = bytearray([byte])
        elif byte == 0xf7 and current is not None:
            current.append(byte)
            frame = bytes(current)
            if (len(frame) < 14 or frame[1:4] != bytes.fromhex('00 20 3c') or
                    frame[4] not in (2, 3)):
                raise ValueError(f'{path}: unexpected SysEx ending at {i}')
            # The Monomachine 0x6f records in the bench selection include
            # the two bytes following the command in their 14-bit sum.
            start = 7 if (frame[4], frame[6]) == (3, 0x6f) else 9
            checksum = (frame[-5] << 7) | frame[-4]
            length = (frame[-3] << 7) | frame[-2]
            if (sum(frame[start:-5]) & 0x3fff) != checksum or len(frame) - 10 != length:
                raise ValueError(f'{path}: checksum or stored length mismatch at {i}')
            frames.append(frame)
            current = None
        elif current is not None and byte < 0x80:
            current.append(byte)
        else:
            raise ValueError(f'{path}: unexpected byte {byte:#04x} at {i}; arm before sending')
    if current is not None or not frames:
        raise ValueError(f'{path}: incomplete or absent dump')
    return frames, clocks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('capture', nargs='?', type=Path, help='repeat of the same saved selection')
    parser.add_argument('--ump', type=Path, help='simultaneous MIDI 1.0 UMP log for the last take')
    args = parser.parse_args()
    expected, _ = read_dump(args.reference)
    for path in (args.reference, args.capture):
        if path is None:
            continue
        frames, clocks = read_dump(path)
        if frames != expected:
            index = next((i for i, (a, b) in enumerate(zip(frames, expected)) if a != b),
                         min(len(frames), len(expected)))
            raise ValueError(f'first differing or missing record {index + 1}, counts {len(frames)}/{len(expected)}')
        stream = b''.join(frames)
        types = Counter(f'{frame[6]:02x}' for frame in frames)
        print(f'{path.name}: {len(frames)} intact records {dict(types)}, {len(stream)} bytes, '
              f'{clocks} clocks within records, SHA-256 {hashlib.sha256(stream).hexdigest()}')
    if args.ump:
        ump_clocks = compare_ump(args.ump, expected, expected_takes=1)
        if ump_clocks != clocks:
            raise ValueError(f'UMP recorded {ump_clocks} interleaved clocks, legacy recorded {clocks}')
        print(f'UMP records match, {ump_clocks} interleaved clocks')


if __name__ == '__main__':
    main()
