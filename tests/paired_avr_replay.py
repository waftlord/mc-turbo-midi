#!/usr/bin/env python3
"""Replay a Machinedrum capture through both compiled AVR images.

Uses the bundled AVR instruction model and avr-objdump. UART FIFOs, USB
polling and the peer are modeled. DIN OUT is collected from the UART shift
register independently of USB IN. Physical DIN needs a bench capture.
"""

import argparse
from collections import deque
from pathlib import Path
import shutil

from avr_model import Avr, MainAvr, SREG, UsbAvr, load_hex, messages, midi_bytes, packetize


PROJECT = Path(__file__).resolve().parents[1]
UARTS = ((0xC0, 0x64, 0x68), (0xC8, 0x90, 0x94), (0xD0, 0xCC, 0xD0))
CYCLES_PER_MS = 16_000
RATE_DIVISOR = {1: 31, 2: 15, 4: 7, 7: 3}
STATUS = bytes.fromhex('f0 7d 4d 43 4c 01 01 f7')


def command(code, payload=b''):
    return bytes.fromhex('f0 00 20 3c 00 00') + bytes((code,)) + payload + b'\xf7'


def speed_report(code):
    return bytes.fromhex('f0 00 20 3c 04 00 02') + bytes((code, 0xF7))


def frame(code):
    crc = code
    for _ in range(8):
        crc = ((crc << 1) ^ (7 if crc & 128 else 0)) & 255
    return bytes((255, code, crc))


def byte_cycles(divisor):
    return 160 * (divisor + 1)


def assert_bytes(actual, expected, boundary):
    if actual != expected:
        first = next((i for i, (a, b) in enumerate(zip(actual, expected)) if a != b),
                     min(len(actual), len(expected)))
        raise AssertionError(
            f'{boundary}, first divergence at {first}, lengths {len(actual)}/{len(expected)}, '
            f'got {actual[first:first + 12].hex()}, expected {expected[first:first + 12].hex()}'
        )


def model_classes():

    def execute(chip):
        size, op, _, target = chip.instructions[chip.pc]
        if op in ('set', 'clt'):
            chip.mem[SREG] = (chip.mem[SREG] & 0xBF) | (0x40 if op == 'set' else 0)
            chip.pc += size
            chip.cycles += 1
        elif op in ('brtc', 'brts'):
            taken = bool(chip.mem[SREG] & 0x40) == (op == 'brts')
            chip.pc = target if taken else chip.pc + size
            chip.cycles += 2 if taken else 1
        else:
            Avr.step(chip)

    class Usb(UsbAvr):
        def __init__(self, image):
            super().__init__(image)
            self.input = deque()
            self.tx_end = 0
            self.tx_data = self.tx_shift = None
            self.rx_fifo = deque()
            self.overruns = self.link_losses = 0
            self.in_banks = []

        def read(self, address):
            if address == 0xC8:
                return (0x20 if self.tx_data is None else 0) | (0x40 if self.tx_shift is None else 0)
            if address == 0xCE and self.rx_fifo:
                return self.rx_fifo.popleft()
            return super().read(address)

        def write(self, address, value):
            if address == 0xCE:
                assert self.tx_data is None
                self.tx_data = value
                return
            if address == 0x284 and self.mem[address] and not value:
                self.link_losses += 1
            super().write(address, value)

        def boot(self):
            self.until(lambda: self.pc == 0x1324)
            assert self.mem[0xCC] == 0
            self.check_endpoint_writes = True
            self.usb_reset()
            descriptor = next(i for i in range(0x1914, 0x1A10, 2) if self.flash_bytes(i, 4) == b'\x09\x02\x85\x00')
            self.control(bytes.fromhex('80 06 00 01 00 00 12 00'), response=self.flash_bytes(0x117, 18))
            self.control(bytes.fromhex('80 06 00 02 00 00 85 00'), response=self.flash_bytes(descriptor, 133))
            self.control(bytes.fromhex('00 05 05 00 00 00 00 00'))
            self.control(bytes.fromhex('00 09 01 00 00 00 00 00'))
            assert all(self.endpoints[i].enabled for i in (1, 2))

        def flash_bytes(self, address, length):
            return bytes(self.flash.get(address + i, 0) for i in range(length))

        def advance(self, receiver):
            if self.tx_shift is not None and self.cycles >= self.tx_end:
                receiver.append((self.tx_end, self.tx_shift, 0))
                self.tx_shift = None
            if self.tx_shift is None and self.tx_data is not None:
                self.tx_shift, self.tx_data = self.tx_data, None
                self.tx_end = self.cycles + byte_cycles(self.mem[0xCC])
            while self.input and self.input[0][0] <= self.cycles:
                _, value = self.input.popleft()
                if len(self.rx_fifo) < 2:
                    self.rx_fifo.append(value)
                else:
                    self.overruns += 1
            self.load_out_bank()
            if self.cycles >= self.next_poll:
                self.next_poll = self.cycles + self.poll_every
                endpoint = self.endpoints[2]
                if endpoint.pending is not None and self.cycles >= self.pause_in_until:
                    self.in_banks.append(bytes(endpoint.pending))
                    self.in_wire.extend(endpoint.pending)
                    endpoint.pending = None
                    endpoint.txini = True
            if self.mem[SREG] & 0x80:
                if self.mem[0xC9] & 0x80 and self.rx_fifo:
                    self.interrupt(0x5C)
                elif self.mem[0xC9] & 0x20 and self.tx_data is None:
                    self.interrupt(0x60)
                elif self.cycles >= self.next_timer:
                    self.next_timer = self.cycles + CYCLES_PER_MS
                    self.interrupt(0x3C)
            execute(self)

    class Main(MainAvr):
        def __init__(self, image):
            super().__init__(image, page=False)
            self.mem.extend(bytes(0x10000 - len(self.mem)))
            self.inputs = [deque() for _ in UARTS]
            self.outputs = [deque() for _ in UARTS]
            self.rx_fifo = [deque() for _ in UARTS]
            self.overruns = [0] * len(UARTS)
            self.tx_data = [None] * len(UARTS)
            self.tx_shift = [None] * len(UARTS)
            self.tx_end = [0] * len(UARTS)
            self.txc = [False] * len(UARTS)

        def read(self, address):
            for port, (base, _, _) in enumerate(UARTS):
                fifo = self.rx_fifo[port]
                if address == base:
                    return ((0x20 if self.tx_data[port] is None else 0) |
                            (0x40 if self.txc[port] else 0) |
                            (0x80 | fifo[0][1] if fifo else 0))
                if address == base + 6 and fifo:
                    return fifo.popleft()[0]
            return super().read(address)

        def write(self, address, value):
            for port, (base, _, _) in enumerate(UARTS):
                if address == base and value & 0x40:
                    self.txc[port] = False
                if address == base + 6:
                    assert self.tx_data[port] is None
                    self.tx_data[port] = value
                    return
            super().write(address, value)

        def step(self):
            for port, (base, _, _) in enumerate(UARTS):
                if self.tx_shift[port] is not None and self.cycles >= self.tx_end[port]:
                    self.outputs[port].append((self.tx_end[port], self.tx_shift[port]))
                    self.tx_shift[port] = None
                    self.txc[port] = self.tx_data[port] is None
                if self.tx_shift[port] is None and self.tx_data[port] is not None:
                    self.tx_end[port] = self.cycles + byte_cycles(self.mem[base + 4])
                    self.tx_shift[port], self.tx_data[port] = self.tx_data[port], None
                fifo = self.rx_fifo[port]
                while self.inputs[port] and self.inputs[port][0][0] <= self.cycles:
                    _, value, status = self.inputs[port].popleft()
                    if self.mem[base + 1] & 0x10:
                        if len(fifo) < 2:
                            fifo.append((value, status))
                        else:
                            self.overruns[port] += 1
                            fifo[-1] = (fifo[-1][0], fifo[-1][1] | 0x08)
            if self.mem[SREG] & 0x80:
                if self.mem[0x6F] & 2 and self.cycles >= self.next_timer:
                    self.next_timer = self.cycles + CYCLES_PER_MS
                    self.interrupt(0x44)
                else:
                    for port, (base, rx, tx) in enumerate(UARTS):
                        control = self.mem[base + 1]
                        if self.rx_fifo[port] and control & 0x80:
                            self.interrupt(rx)
                            break
                        if self.tx_data[port] is None and control & 0x20:
                            self.interrupt(tx)
                            break
            execute(self)

    return Usb, Main


class Bench:
    def __init__(self, images, objdump):
        usb_model, main_model = model_classes()
        self.usb = usb_model(load_hex(images / 'megacommand-diy-dual-atmega16u2-dfu.hex', objdump, 'avr:35'))
        self.main = main_model(load_hex(images / 'megacommand-diy-dual-atmega2560-update.hex', objdump, 'avr:6'))
        self.packetize, self.decode = packetize, midi_bytes
        self.usb.boot()
        self.usb.poll_every = CYCLES_PER_MS
        self.run(95)
        assert self.usb.mem[0x284] == 1
        assert self.divisor(1) == self.divisor(2) == 31
        self.clear()

    def run(self, timeout_ms, done=lambda: False):
        usb, main = self.usb, self.main
        deadline = max(usb.cycles, main.cycles) + timeout_ms * CYCLES_PER_MS
        while min(usb.cycles, main.cycles) < deadline:
            if usb.cycles <= main.cycles:
                usb.advance(main.inputs[0])
            else:
                main.step()
                if main.outputs[0]:
                    usb.input.append(main.outputs[0].popleft())
            if done():
                return

    def clear(self):
        self.usb.in_wire.clear()
        self.usb.in_banks.clear()
        self.main.outputs[1].clear()
        self.main.outputs[2].clear()

    def divisor(self, port):
        return self.main.mem[UARTS[port][0] + 4]

    def input_din(self, port, data, fault=0):
        period = byte_cycles(self.divisor(port))
        start = self.main.cycles + 1600
        self.main.inputs[port].extend((start + (i + 1) * period, byte, fault)
                                      for i, byte in enumerate(data))

    def output_din(self, port, remove_sense=False):
        return bytes(value for _, value in self.main.outputs[port] if not remove_sense or value != 0xFE)

    def read_host(self, port):
        events = self.usb.in_wire
        return self.decode(b''.join(events[i:i + 4] for i in range(0, len(events), 4)
                                   if events[i] >> 4 == port - 1))

    def send_host(self, *events_by_port):
        parts = [bytes(value | ((port - 1) << 4) if i % 4 == 0 else value
                       for i, value in enumerate(events)) for port, events in events_by_port]
        wire = b''.join(part[i:i + 4] for i in range(0, max(map(len, parts)), 4) for part in parts)
        self.usb.host_out.extend(wire[i:i + 64] for i in range(0, len(wire), 64))

    def assert_no_loss(self):
        assert not self.usb.overruns and not any(self.main.overruns), (self.usb.overruns, self.main.overruns)
        assert self.usb.mem[0x284] and not self.usb.link_losses

    def status(self, rate):
        self.clear()
        events = self.packetize(STATUS)
        self.send_host((1, events), (2, events))
        self.run(30)
        for port in (1, 2):
            assert_bytes(self.read_host(port), speed_report(rate), f'port {port} settled rate')
            assert not self.output_din(port, remove_sense=True), 'status query changed DIN traffic'
        self.clear()

    def negotiate(self, rate):
        request = bytes.fromhex('f0 00 20 3c 04 00 03') + bytes((rate, 0xF7))
        events = self.packetize(request)
        pattern = b'\x55' * 4 + bytes(4)
        for port in (1, 2):
            self.send_host((port, events))
            self.run(10)
            assert_bytes(self.output_din(port), command(0x10), f'port {port} speed request')
            self.input_din(port, command(0x11, b'\x25\x00\x25\x00'))
            self.clear()
            self.run(13)
            assert_bytes(self.output_din(port), command(0x12, bytes((rate, rate))), f'port {port} negotiation')
            self.input_din(port, command(0x13))
            self.clear()
            self.run(15)
            assert self.divisor(port) == RATE_DIVISOR[rate]
            assert_bytes(self.output_din(port), bytes(16) + command(0x14, pattern), f'port {port} test')
            self.input_din(port, command(0x15, pattern))
            self.clear()
            self.run(9)
            assert_bytes(self.output_din(port), command(0x16), f'port {port} second test')
            self.input_din(port, command(0x17))
            self.clear()
            self.run(14)
            assert_bytes(self.output_din(port), b'\xfe', f'port {port} final test')
            self.input_din(port, b'\xfe')
            self.run(10)
            assert_bytes(self.read_host(port), speed_report(rate), f'port {port} settled Turbo rate')
            self.clear()

    def return_to_normal(self):
        request = bytes.fromhex('f0 00 20 3c 04 00 03 01 f7')
        events = self.packetize(request)
        self.send_host((1, events), (2, events))
        self.run(390)
        for port in (1, 2):
            assert self.divisor(port) == 31
            assert_bytes(self.read_host(port), speed_report(1), f'port {port} return to 1x')
        self.status(1)

    def replay(self, records, ports, pause_ms=0, realtime=False):
        self.clear()
        usb_events = b''.join(self.packetize(record) for record in records)
        expected = b''.join(records)
        outbound = {}
        inbound = {}
        self.usb.pause_in_until = self.usb.cycles + pause_ms * CYCLES_PER_MS
        for port in ports:
            if realtime:
                # Put realtime after a complete packet, past the held prefix.
                # Different statuses on all lines also check cable isolation.
                receive_status = b'\xf8\xf8\xfa' if port == 1 else b'\xfb\xf8\xfc'
                send_status = bytes((0xFB if port == 1 else 0xFC,))
                outbound[port] = (usb_events[:44] + bytes((15,)) + send_status + bytes(2) + usb_events[44:],
                                  expected[:33] + send_status + expected[33:])
                inbound[port] = (expected[:33] + receive_status[:1] + expected[33:111] +
                                 receive_status[1:2] + expected[111:234] +
                                 receive_status[2:] + expected[234:])
            else:
                outbound[port] = (usb_events, expected)
                inbound[port] = expected
            self.input_din(port, inbound[port])
            if self.divisor(port) != RATE_DIVISOR[1]:
                queue = self.main.inputs[port]
                last_payload = queue[-1][0]
                queue.extend((last_payload + delay * CYCLES_PER_MS, 0xFE, 0)
                             for delay in range(150, 2200, 150))
        self.send_host(*((port, outbound[port][0]) for port in ports))
        # End at the wire drain, allowing time for the final USB IN poll.
        timeout_ms = pause_ms + len(expected) * byte_cycles(self.divisor(ports[0])) // CYCLES_PER_MS + 2000
        expected_events = sum(len(events) + (8 if realtime else 0) for events, _ in outbound.values())
        self.run(timeout_ms, lambda: len(self.usb.in_wire) >= expected_events and
                 all(len(self.main.outputs[port]) >= len(outbound[port][1]) for port in ports))
        self.run(12)
        for port in (1, 2):
            target = outbound.get(port, (b'', b''))[1]
            assert_bytes(self.read_host(port), inbound.get(port, b''), f'port {port} USB IN')
            turbo = self.divisor(port) != RATE_DIVISOR[1]
            assert_bytes(self.output_din(port, remove_sense=turbo), target, f'port {port} DIN OUT')
            if not target:
                continue
            wire = list(self.main.outputs[port])
            wire_gaps = [end - start for (start, _), (end, _) in zip(wire, wire[1:])]
            minimum = byte_cycles(self.divisor(port))
            assert min(wire_gaps) >= minimum, (port, minimum, min(wire_gaps))
            payload = [(at, byte) for at, byte in wire if not turbo or byte != 0xFE]
            in_record_gaps = [end - start for (start, previous), (end, _) in zip(payload, payload[1:])
                              if previous != 0xF7]
            print(f'port {port}: {len(inbound[port])} USB IN bytes, {len(target)} DIN OUT bytes, minimum '
                  f'{min(wire_gaps) / 16:g} µs, max gap within SysEx {max(in_record_gaps) / 16:g} µs',
                  flush=True)
        self.assert_no_loss()


    def check_usb_banks(self):
        # An interleaved realtime event may precede SysEx data in a bank,
        # but the legacy host callback synthesizes an EOX in the reverse order.
        for i, bank in enumerate(self.usb.in_banks):
            has_sysex = set()
            for offset in range(0, len(bank), 4):
                cable, cin = divmod(bank[offset], 16)
                if cin == 4:
                    has_sysex.add(cable)
                elif cin in (5, 6, 7) and 0xF7 in bank[offset + 1:offset + 4]:
                    has_sysex.discard(cable)
                elif cin == 15 and bank[offset + 1] >= 0xF8 and cable in has_sysex:
                    raise AssertionError(f'bank {i}, cable {cable + 1}: CIN 4 precedes realtime in {bank.hex()}')

    def interleaved_realtime(self, records):
        # Delay host reads to make multiple DIN events queue for one IN bank.
        self.usb.pause_in_until = self.usb.cycles + 45 * CYCLES_PER_MS
        first = records[0]
        stream = first[:33] + b'\xf8' + first[33:66] + b'\xf8\xfa' + first[66:]
        self.input_din(1, stream)
        self.run(12)
        self.input_din(2, b'\xfc\xf8')
        self.run(48)
        assert_bytes(self.read_host(2), b'\xfc\xf8', 'realtime on the cable outside SysEx')
        self.run(350)
        assert_bytes(self.read_host(1), stream, 'SysEx and realtime preserve order through USB IN')
        self.check_usb_banks()
        self.assert_no_loss()
        print('USB IN banks preserve realtime order and isolate same-cable SysEx data from following realtime',
              flush=True)


def replay_args(parser):
    parser.add_argument('capture', nargs='?', type=Path,
                        help='optional 4038-byte Machinedrum capture; defaults to synthetic records')
    parser.add_argument('--images', type=Path, default=PROJECT / 'build', help='directory with both HEX files')
    parser.add_argument('--objdump', type=Path, help='path to avr-objdump; defaults to PATH or PlatformIO cache')


def start_bench(parser, args):
    objdump = args.objdump or shutil.which('avr-objdump')
    if objdump is None:
        cached = Path.home() / '.platformio/packages/toolchain-atmelavr/bin/avr-objdump'
        if not cached.is_file():
            parser.error('avr-objdump is required. Install AVR binutils or set --objdump.')
        objdump = cached
    return Bench(args.images, objdump), replay_records(args.capture)


def replay_records(capture):
    sizes = (1176, 1176, 873, 813)
    if capture is not None:
        records = messages(capture.read_bytes())
        assert tuple(map(len, records)) == sizes, 'unexpected capture record sizes'
        return records

    def encoded_word(value):
        return bytes((value >> 7, value & 127))

    records = []
    for index, size in enumerate(sizes):
        # Test data uses the Machinedrum header, kit/pattern identifiers,
        # checksum and stored length. Payload bytes are generated locally.
        kind, version, location = (0x52, 0x41, 7 + index) if index < 2 else (0x67, 0x40, 50 + index)
        header = bytes.fromhex('f0 00 20 3c 02 00') + bytes((kind, version, 1))
        data = bytes((location,)) + bytes(((i * 109 + (i >> 3) * 17 + index * 31) & 127)
                                            for i in range(size - len(header) - 6))
        checksum = encoded_word(sum(data) & 0x3FFF)
        records.append(header + data + checksum + encoded_word(size - 10) + b'\xf7')
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    replay_args(parser)
    parser.add_argument('--case', choices=('cold', 'return', '2x', '4x', '8x', 'stress',
                                          'interruption', 'realtime'), required=True)
    args = parser.parse_args()
    bench, records = start_bench(parser, args)

    if args.case == 'cold':
        bench.status(1)
        bench.replay(records, (1,))
        bench.replay(records, (2,))
    elif args.case == 'return':
        bench.negotiate(7)
        bench.return_to_normal()
        bench.replay(records, (1, 2))
    elif args.case in ('2x', '4x', '8x'):
        bench.negotiate({'2x': 2, '4x': 4, '8x': 7}[args.case])
        bench.replay(records, (1, 2), realtime=True)
    elif args.case == 'stress':
        bench.negotiate(7)
        long_dump = b'\xf0\x7d' + bytes(range(128)) * 188 + b'\xf7'
        bench.replay([long_dump], (1, 2), pause_ms=128, realtime=True)
    elif args.case == 'realtime':
        bench.status(1)
        bench.interleaved_realtime(records)
    else:
        for port in (1, 2):
            bench.clear()
            bench.input_din(port, b'\xf0\x7d\x01\x02')
            bench.send_host((port, bytes.fromhex('04 f0 7d 01 04 02 03 04')))
            bench.run(170)
            assert_bytes(bench.read_host(port), b'\xf0\x7d\x01', 'DIN partial message before fault')
            assert_bytes(bench.output_din(port), b'\xf0\x7d\x01\x02\x03\x04', 'host partial message before fault')
            bench.input_din(port, b'\x03', fault=0x10)
            bench.run(10)
            bench.input_din(port, b'\x04\x05\xf7' + records[0])
            bench.run(400)
            assert_bytes(bench.read_host(port), b'\xf0\x7d\x01\xf7' + speed_report(1) + records[0],
                         'DIN framing recovery and settled rate')

            # Corrupt a received internal heartbeat's CRC while DIN OUT is
            # mid-SysEx. The reset closes it and then admits the next message.
            corrupt = frame(7)
            previous_losses = bench.usb.link_losses
            start = bench.main.cycles + 1600
            bench.main.inputs[0].extend((start + i * 160, byte, 0)
                                        for i, byte in enumerate(corrupt[:-1] + bytes((corrupt[-1] ^ 1,))))
            bench.run(130)
            assert bench.usb.mem[0x284] and bench.usb.link_losses > previous_losses
            bench.send_host((port, bench.packetize(records[0])))
            bench.run(480)
            assert_bytes(bench.output_din(port), b'\xf0\x7d\x01\x02\x03\x04\xf7' + records[0],
                         'DIN OUT recovery after internal CRC fault')
        print('Both ports recovered from injected UART FE and internal CRC faults', flush=True)


if __name__ == '__main__':
    main()
