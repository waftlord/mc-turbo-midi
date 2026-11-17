#!/usr/bin/env python3
"""Exercise USB-MIDI queueing and session recovery on the compiled pair."""

import argparse

from avr_model import SREG
from paired_avr_replay import CYCLES_PER_MS, assert_bytes, replay_args, speed_report, start_bench


def interrupted_prefill(bench):
    # The host loses the rest of a dump after its first event, then sends
    # ordinary music. A new channel status ends the incomplete DIN message.
    partial = bytes.fromhex('04 f0 7d 01')
    latencies = []
    for port in (1, 2):
        bench.clear()
        bench.send_host((port, partial))
        bench.run(8)
        expected = bytearray(partial[1:])
        submitted = bench.main.cycles
        for note in (60, 61, 62):
            event = bytes((9, 0x90, note, 100))
            expected.extend(event[1:])
            bench.send_host((port, event))
            bench.run(12)
        bench.run(180, lambda: len(bench.main.outputs[port]) == len(expected))
        assert_bytes(bench.output_din(port), expected, f'port {port} notes after interrupted SysEx')
        latency = (bench.main.outputs[port][5][0] - submitted) / CYCLES_PER_MS
        print(f'Port {port} first complete note after interrupted prefill: {latency:.3f} ms', flush=True)
        latencies.append(latency)
        assert not bench.output_din(3 - port)
        bench.assert_no_loss()
    assert max(latencies) < 12, f'Clean notes parked behind an interrupted SysEx: {latencies} ms'


def streamed_prefill(bench):
    # Both hosts leave their record open and submit new continuation events
    # every 48 ms. Neither stream is large enough to hit the 7168-byte gate.
    begin = bytes.fromhex('04 f0 7d 01')
    continuation = bytes.fromhex('04 02 03 04')
    expected = [bytearray(begin[1:]), bytearray(begin[1:])]
    submitted = bench.main.cycles
    bench.send_host((1, begin), (2, begin))
    for _ in range(28):
        bench.run(48)
        bench.send_host((1, continuation), (2, continuation * 22))
        for data, repeats in zip(expected, (1, 22)):
            data.extend(continuation[1:] * repeats)
    end = bytes.fromhex('05 f7 00 00')
    bench.send_host((1, end), (2, end))
    bench.run(700)
    latencies = []
    for port in (1, 2):
        assert_bytes(bench.output_din(port), expected[port - 1] + b'\xf7', f'port {port} slow host')
        latency = (bench.main.outputs[port][0][0] - submitted) / CYCLES_PER_MS
        print(f'Port {port} first DIN byte from a continuously refreshed prefill: {latency:.3f} ms', flush=True)
        latencies.append(latency)
    assert latencies[0] < 165 and latencies[1] < 1270, f'Continuous host packets held prefill: {latencies} ms'
    bench.assert_no_loss()


def ordinary_messages(bench):
    bench.clear()
    # Invalid cable, mismatched status and invalid data precede clean events.
    # Raw CIN F can carry running status. Padding on short events is ignored.
    bench.usb.host_out.extend([
        bytes.fromhex('29 90 01 02 09 80 01 02 09 90 01 ff'),
        bytes.fromhex('09 90 3c'),  # Discard the incomplete USB event.
        bytes.fromhex('0f 90 fe ff 0f 3c fe ff 0f 40 fe ff 0f 3d fe ff 0f 41 fe ff'),
        bytes.fromhex('1c c1 02 ff 12 f3 03 ff 13 f2 01 02 15 f6 ff ff 1f ff fe ff'),
    ])
    bench.input_din(1, bytes.fromhex('90 20 40 21 41 f8 f3 02 03 c1 05 06'))
    bench.input_din(2, bytes.fromhex('f7 01 02 80 20 00 fc'))
    bench.run(20)
    assert_bytes(bench.output_din(1), bytes.fromhex('90 3c 40 3d 41'), 'raw USB running status')
    assert_bytes(bench.output_din(2), bytes.fromhex('c1 02 f3 03 f2 01 02 f6 ff'), 'short USB events')
    assert_bytes(bench.read_host(1), bytes.fromhex('90 20 40 90 21 41 f8 f3 02 c1 05 c1 06'),
                 'DIN running status and system common cancellation')
    assert_bytes(bench.read_host(2), bytes.fromhex('80 20 00 fc'), 'DIN orphan data')
    bench.assert_no_loss()
    print('Running status, system common, USB raw bytes, padding and malformed banks passed')


def prefix_clock(bench):
    # Real manufacturer data can share the first three bytes of a bridge
    # control. Position the clock on a USB event boundary in both directions.
    for port in (1, 2):
        bench.clear()
        events = bytes.fromhex('04 f0 00 20 0f f8 00 00 07 3c 02 f7')
        stream = bytes.fromhex('f0 00 20 f8 3c 02 f7')
        bench.send_host((port, events))
        bench.input_din(port, stream)
        bench.run(15)
        assert_bytes(bench.output_din(port), stream, 'DIN OUT clock after held prefix')
        assert_bytes(bench.read_host(port), stream, 'USB IN clock after held prefix')
        assert not bench.output_din(3 - port) and not bench.read_host(3 - port)
        bench.check_usb_banks()
        bench.assert_no_loss()
    print('Clock after a complete held SysEx group preserved on all four routes')


def endpoint_stalls(bench):
    for endpoint in (1, 2):
        bench.clear()
        ep = bench.usb.endpoints[endpoint]
        ep.control |= 0x20  # Halt the data endpoint while keeping EP0 configured.
        note = bytes.fromhex('90 3c 40')
        bench.input_din(1, note)
        bench.send_host((2, bytes.fromhex('09 91 3e 44')))
        bench.run(20)
        # The opposite direction remains live on its own endpoint.
        assert_bytes(bench.output_din(2), b'' if endpoint == 1 else bytes.fromhex('91 3e 44'),
                     'DIN OUT while USB endpoint halted')
        assert_bytes(bench.read_host(1), note if endpoint == 1 else b'', 'USB IN while endpoint halted')
        ep.control &= ~0x20
        bench.run(20)
        assert_bytes(bench.output_din(2), bytes.fromhex('91 3e 44'), 'DIN OUT after clear halt')
        assert_bytes(bench.read_host(1), note, 'USB IN after clear halt')
        assert not bench.read_host(2) and not bench.output_din(1)
        ordinary_messages(bench)
    print('Both USB data directions resume after a modeled temporary endpoint halt')


def wire_frame(kind, payload):
    # Independent bitwise CRC for injected internal control frames.
    crc = 0
    frame = bytearray([0xff])
    for byte in bytes((kind,)) + payload:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ (7 if crc & 128 else 0)) & 255
        frame.extend((0xfe, byte - 0xfe) if byte >= 0xfe else (byte,))
    frame.extend((0xfe, crc - 0xfe) if crc >= 0xfe else (crc,))
    return frame


def session_faults(bench):
    for fault in ('usb-credit', 'main-credit', 'usb-crc', 'usb-reset'):
        bench.clear()
        prior_losses = bench.usb.link_losses
        if fault == 'usb-reset':
            # The bus event clears the FIFOs immediately. Its ISR runs
            # only after firmware restores both interrupt enables.
            usb = bench.usb
            usb.signal_usb_reset()
            enabled = lambda: usb.mem[SREG] & 0x80 and usb.mem[0xe2] & 8
            bench.run(10, enabled)
            assert enabled(), 'pending USB reset never became serviceable'
            usb.run_isr(0x2c)
            assert usb.mem[0x3e] == 2 and usb.endpoints[0].enabled
            bench.usb.control(bytes.fromhex('00 05 05 00 00 00 00 00'))
            bench.usb.control(bytes.fromhex('00 09 01 00 00 00 00 00'))
        elif fault == 'main-credit':
            # The USB chip cannot give the main a third device-to-host bank.
            frame = wire_frame(2, bytes((64, 0, 0, 0, 3)))
            start = bench.main.cycles + 1600
            bench.main.inputs[0].extend((start + 160 * i, byte, 0) for i, byte in enumerate(frame))
        else:
            frame = wire_frame(2, bytes.fromhex('ff ff 00 00 00'))
            if fault == 'usb-crc':
                frame[-1] ^= 1
            start = bench.usb.cycles + 1600
            bench.usb.input.extend((start + 160 * i, byte) for i, byte in enumerate(frame))
        bench.run(160)
        assert bench.usb.mem[0x284] and bench.usb.link_losses > prior_losses, fault
        assert not bench.usb.overruns and not any(bench.main.overruns), fault
        bench.status(1)
        for port in (1, 2):
            clean = bytes((0xf0, 0x7d, port, 42, 55, 0xf7))
            bench.input_din(port, clean)
            # A stale continuation from before the reset must be discarded.
            bench.send_host((port, bytes.fromhex('04 04 05 06 05 f7 00 00') + bench.packetize(clean)))
            bench.run(12)
            assert_bytes(bench.output_din(port), clean, f'{fault} next DIN OUT on port {port}')
            assert_bytes(bench.read_host(port), clean, f'{fault} next USB IN on port {port}')
            bench.clear()
        print(f'{fault}: fresh handshake, both ports recovered cleanly', flush=True)


def stalled_dump(bench, overflow):
    bench.negotiate(7)
    bench.clear()
    count = 20000 if overflow else 15000
    pause_ms = count // 25  # 25,000 DIN bytes/s at 8×.
    bench.usb.pause_in_until = bench.usb.cycles + pause_ms * CYCLES_PER_MS
    streams = {}
    for port in (1, 2):
        streams[port] = b'\xf0\x7d' + bytes((i * 11 + port) & 127 for i in range(count - 3)) + b'\xf7'
        bench.input_din(port, streams[port])
        # The connected peer stays live after the last payload byte.
        last = bench.main.inputs[port][-1][0]
        bench.main.inputs[port].extend((last + ms * CYCLES_PER_MS, 0xfe, 0)
                                      for ms in range(120, 1201, 120))
    bench.run(pause_ms + 1300)
    for port in (1, 2):
        actual = bench.read_host(port)
        if overflow:
            actual = actual.replace(speed_report(1), b'').replace(b'\xfe', b'')
            # The 12,032-byte raw ring plus data already packetized for
            # USB fit before exhaustion. An early Turbo fallback retains
            # only the ~6 kB packetized portion and loses valid raw bytes.
            assert 12032 < len(actual) < count and actual[-1] == 0xf7
            assert_bytes(actual[:-1], streams[port][:len(actual) - 1], f'port {port} exact prefix at exhaustion')
            print(f'Port {port}: overflow retained {len(actual) - 1} prefix bytes and emitted one EOX')
        else:
            assert_bytes(actual, streams[port], f'port {port} finite host stall with live DIN')
            assert bench.divisor(port) == 3, 'Turbo peer continued sending throughout the pause'
    bench.clear()
    for port in (1, 2):
        clean = bytes((0xf0, 0x7d, port, 5, 6, 0xf7))
        bench.input_din(port, clean)
        bench.send_host((port, bench.packetize(clean)))
        bench.run(15)
        assert_bytes(bench.read_host(port), clean, f'port {port} next clean DIN input')
        assert_bytes(bench.output_din(port, remove_sense=True), clean, f'port {port} next clean DIN output')
        bench.clear()
    bench.assert_no_loss()
    print(f'Both ports recovered after a {pause_ms} ms modeled host IN pause')


def blocked_disconnect(bench):
    bench.negotiate(7)
    bench.clear()
    # Both raw rings have queued bytes after the USB packet queues fill.
    # Simulate unplugging Port 1. Port 2 keeps sending sense at the wire.
    bench.usb.pause_in_until = bench.usb.cycles + 750 * CYCLES_PER_MS
    for port in (1, 2):
        bench.input_din(port, b'\xf0\x7d' + bytes((port,)) * 7497 + b'\xf7')
    last = bench.main.inputs[2][-1][0]
    bench.main.inputs[2].extend((last + ms * CYCLES_PER_MS, 0xfe, 0) for ms in range(120, 961, 120))
    bench.run(720)
    assert (bench.divisor(1), bench.divisor(2)) == (31, 3), 'only the disconnected peer should fall back'
    bench.run(650)
    assert_bytes(bench.read_host(2), b'\xf0\x7d' + bytes((2,)) * 7497 + b'\xf7', 'live peer during disconnect')
    bench.clear()
    for port in (1, 2):
        clean = bytes((0xf0, 0x7d, port, 0xf7))
        bench.input_din(port, clean)
        bench.send_host((port, bench.packetize(clean)))
        bench.run(15)
        assert_bytes(bench.read_host(port), clean, f'port {port} next input after disconnect')
        assert_bytes(bench.output_din(port, remove_sense=True), clean, f'port {port} next output after disconnect')
        bench.clear()
    bench.assert_no_loss()
    print('With USB IN paused, only the disconnected port fell back and both passed the next clean message')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    replay_args(parser)
    cases = {'prefill': interrupted_prefill, 'streamed-prefill': streamed_prefill,
             'midi': ordinary_messages, 'prefix': prefix_clock,
             'stalls': endpoint_stalls, 'long-stall': lambda bench: stalled_dump(bench, False),
             'overflow': lambda bench: stalled_dump(bench, True), 'disconnect': blocked_disconnect,
             'reset': session_faults}
    parser.add_argument('--case', choices=cases, help='run one case instead of the whole suite')
    args = parser.parse_args()
    for case in ([args.case] if args.case else cases):
        bench, _ = start_bench(parser, args)
        cases[case](bench)


if __name__ == '__main__':
    main()
