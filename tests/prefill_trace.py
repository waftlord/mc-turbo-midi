#!/usr/bin/env python3
"""Trace how USB OUT banks fill both DIN queues in the modeled dual-8× stress replay."""

import argparse
from collections import deque
from contextlib import redirect_stdout
import hashlib
import json
from pathlib import Path
import shutil
import sys

from avr_model import CIN_LENGTH
from paired_avr_replay import Bench, CYCLES_PER_MS, PROJECT
from profile_avr import TimedOutBanks, event_times
from usb_transport_replay import wire_frame


class WireTap(deque):
    """Retain completed internal UART characters as the model transfers them."""
    def __init__(self, queue):
        super().__init__(queue)
        self.characters = []

    def append(self, item):
        self.characters.append(item[:2])
        super().append(item)


def frames(characters):
    """Decode complete frames from START through CRC, including escape sequences."""
    encoded = bytearray()
    body = bytearray()
    escaped = False
    for tick, byte in characters:
        if byte == 0xff:
            assert not encoded, 'new START interrupted an internal frame'
            encoded.append(byte)
            body.clear()
            escaped = False
            continue
        if not encoded:  # Observation can start midway through an idle control frame.
            continue
        encoded.append(byte)
        if escaped:
            assert byte <= 1
            byte += 0xfe
            escaped = False
        elif byte == 0xfe:
            escaped = True
            continue
        body.append(byte)
        kind = body[0]
        assert 1 <= kind <= 8
        if kind in (1, 8):
            if len(body) < 2:
                continue
            assert 0 < body[1] <= 64 and body[1] % 4 == 0
            size = 3 + (body[1] if kind == 1 else 3 * (body[1] // 4))
        else:
            size = 7 if kind == 2 else 2
        if len(body) == size:
            payload = bytes(body[1:-1])
            assert encoded == wire_frame(kind, payload), 'invalid internal frame CRC'
            yield tick, kind, payload
            encoded.clear()
    # A final credit or heartbeat can still be shifting when Bench.replay returns.


def usb_events(kind, payload):
    if kind == 1:
        return payload[1:]
    events = bytearray()
    for i in range(1, len(payload), 3):
        tag, second, third = payload[i:i + 3]
        events.extend((4 | (tag >> 7) << 4, tag & 127, second, third))
    return bytes(events)


def measure(images, objdump, stall_ms):
    bench = Bench(images, objdump)
    bench.negotiate(7)
    banks = []
    assert not bench.usb.host_out
    bench.usb.host_out = TimedOutBanks(bench.usb, banks)
    host_wire = bench.main.inputs[0] = WireTap(bench.main.inputs[0])
    credit_wire = bench.main.outputs[0] = WireTap(bench.main.outputs[0])
    start = bench.main.cycles
    # Use the end of the prescribed pause for every comparison. The first
    # actual IN read can follow on the next 1 ms host poll.
    stall_end = bench.usb.cycles + stall_ms * CYCLES_PER_MS
    record = b'\xf0\x7d' + bytes(range(128)) * 188 + b'\xf7'
    with redirect_stdout(sys.stderr):
        bench.replay([record], (1, 2), stall_ms, realtime=True)

    def ms(cycles):
        return round(cycles / CYCLES_PER_MS, 4)

    admitted = [event_times(banks, i)[0] for i in range(2)]
    first_characters = [next((t, b) for t, b in bench.main.outputs[p] if b != 0xfe) for p in (1, 2)]
    assert all(byte == 0xf0 for _, byte in first_characters)
    first_din = [t for t, _ in first_characters]
    milestones = [{}, {}]
    payload_bytes = [0, 0]
    bytes_before_resume = [0, 0]
    total_frames = frames_before_resume = 0
    splits = []
    splits_before_resume = 0
    bank_frames = []
    bank_index = offset = 0
    completed_before_resume = 0
    for tick, kind, payload in frames(host_wire.characters):
        if kind not in (1, 8):
            continue
        events = usb_events(kind, payload)
        bank = banks[bank_index][1]
        assert events == bank[offset:offset + len(events)], 'internal frame differs from host bank'
        offset += len(events)
        total_frames += 1
        frames_before_resume += tick < stall_end
        bank_frames.append((kind, len(events) // 4))
        for i in range(0, len(events), 4):
            cable, cin = divmod(events[i], 16)
            payload_bytes[cable] += CIN_LENGTH[cin]
            if tick < stall_end:
                bytes_before_resume[cable] = payload_bytes[cable]
            for mark in (4096, 7168, 7264):
                if payload_bytes[cable] >= mark and mark not in milestones[cable]:
                    milestones[cable][mark] = ms(tick - admitted[cable])
        if offset == len(bank):
            completed_before_resume += tick < stall_end
            # Both frames must be compact continuations of the same actual USB bank.
            if bank_frames == [(8, 15), (8, 1)]:
                splits.append({'bank_1_based': bank_index + 1, 'complete_at_ms': ms(tick - start)})
                splits_before_resume += tick < stall_end
            bank_frames.clear()
            bank_index += 1
            offset = 0
    assert bank_index == len(banks) and offset == 0
    grants = [(t, p[4]) for t, kind, p in frames(credit_wire.characters) if kind == 2 and p[4]]
    first_grant_ticks = [t for t, _ in grants if t <= min(first_din)]
    max_grant_gap = max(b - a for a, b in zip(first_grant_ticks, first_grant_ticks[1:]))
    return {
        'images': {f.name: hashlib.sha256(f.read_bytes()).hexdigest() for f in sorted(
            images.glob('megacommand-diy-dual-atmega*.hex'))},
        'host_in_stall_ms': stall_ms,
        'usb_out_banks': len(banks),
        'usb_out_banks_completed_on_internal_link_before_host_in_resumes': completed_before_resume,
        'usb_out_frames_total': total_frames,
        'usb_out_frames_before_host_in_resumes': frames_before_resume,
        'usb_out_extra_frames_vs_one_per_bank': total_frames - len(banks),
        'compact_15_plus_1_splits': len(splits),
        'compact_15_plus_1_splits_before_host_in_resumes': splits_before_resume,
        'first_and_last_compact_split': [splits[0], splits[-1]] if splits else [],
        'frame_credits_returned_before_host_in_resumes': sum(n for t, n in grants if t < stall_end),
        'max_positive_grant_interval_before_either_din_starts_ms': ms(max_grant_gap),
        'ports': [{
            'first_usb_out_bank_from_replay_start_ms': ms(admitted[i] - start),
            'first_bank_to_first_completed_din_character_ms': ms(first_din[i] - admitted[i]),
            'first_bank_to_4096_internal_midi_bytes_ms': milestones[i][4096],
            'first_bank_to_7168_internal_midi_bytes_ms': milestones[i][7168],
            'first_bank_to_7264_internal_midi_bytes_ms': milestones[i][7264],
            'internal_midi_bytes_before_host_in_resumes': bytes_before_resume[i],
            'internal_midi_bytes_total': payload_bytes[i],
        } for i in range(2)],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--images', action='append', type=Path,
                        help='matched HEX directory; repeat to compare the same workload')
    parser.add_argument('--objdump', type=Path)
    parser.add_argument('--stall-ms', type=int, default=128)
    parser.add_argument('--output', type=Path, help='write compact JSON results to this file')
    args = parser.parse_args()
    if args.stall_ms < 0:
        parser.error('--stall-ms must be nonnegative')
    objdump = args.objdump or shutil.which('avr-objdump') or (
        Path.home() / '.platformio/packages/toolchain-atmelavr/bin/avr-objdump')
    results = [measure(images, objdump, args.stall_ms) for images in args.images or [PROJECT / 'build']]
    report = json.dumps(results, indent=2) + '\n'
    if args.output:
        args.output.write_text(report)
    else:
        sys.stdout.write(report)


if __name__ == '__main__':
    main()
