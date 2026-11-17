#!/usr/bin/env python3
"""Profile a matched HEX pair under the byte-exact, dual-DIN stress replay."""

import argparse
from collections import Counter, deque
import hashlib
import json
from math import ceil
from pathlib import Path
import shutil
import subprocess
import tempfile

from avr_model import CIN_LENGTH, UsbAvr
from paired_avr_replay import Bench, PROJECT, byte_cycles, replay_args, replay_records


def elf_symbols(elf, nm):
    symbols = {}
    for line in subprocess.check_output([nm, '-SnC', '--defined-only', elf], text=True).splitlines():
        words = line.split(maxsplit=3)
        if len(words) == 3:
            address, _, name = words
            size = 0
        elif len(words) == 4:
            address, length, _, name = words
            size = int(length, 16)
        else:
            continue
        symbols[name] = int(address, 16), size
    return symbols


def word(mem, address):
    return mem[address] | mem[address + 1] << 8


class CpuProfile:
    def __init__(self, chip):
        self.usb = isinstance(chip, UsbAvr)
        self.vectors = {0x3c, 0x5c, 0x60} if self.usb else {0x44, 0x64, 0x68, 0x90, 0x94, 0xcc, 0xd0}
        self.returns = {pc for pc, (_, op, _, _) in chip.instructions.items() if op == 'reti'}
        self.set_sp = {pc for pc, (_, op, args, _) in chip.instructions.items()
                       if op == 'out' and int(args[0], 0) == 0x3d}
        self.active = []
        self.pc_cycles = [0] * (max(chip.flash) + 1)
        self.isr_cycles = 0
        self.elapsed = 0
        self.maximum = Counter()

    def instruction(self, chip, pc, cycles, recording):
        if pc in self.set_sp and chip.sp:
            chip.lowest_stack = min(chip.lowest_stack, chip.sp)
        if pc in self.vectors:
            # Four cycles are charged for vector entry by Avr.interrupt().
            self.active.append((pc, chip.cycles - cycles - 4))
            cycles += 4
        if recording:
            self.elapsed += cycles
            if self.active:
                self.isr_cycles += cycles
            else:
                self.pc_cycles[pc] += cycles
        if pc in self.returns:
            vector, start = self.active.pop()
            if recording:
                self.maximum[hex(vector)] = max(self.maximum[hex(vector)], chip.cycles - start)

    def result(self, chip, symbols):
        ram_end = 0x2ff if self.usb else 0x21ff
        # The stock 16U2 startup clears BSS to 0x299 inclusive and its
        # retained state at 0x29a..0x29d follows it. The patch overlays SRAM.
        static_end = 0x29e if self.usb else (symbols['__bss_end'][0] & 0xffff)
        assert static_end <= chip.lowest_stack + 1 <= ram_end
        result = {
            'execution_cycles': self.elapsed,
            'isr_cycles': self.isr_cycles,
            'isr_percent': round(100 * self.isr_cycles / self.elapsed, 3),
            'max_isr_cycles_by_vector': dict(sorted(self.maximum.items())),
            'max_stack_bytes_since_boot': ram_end - chip.lowest_stack,
            'sram_gap_bytes_at_lowest_observed_sp': chip.lowest_stack + 1 - static_end,
            'static_sram_reserved_bytes': static_end - (0x100 if self.usb else 0x200),
            'flash_image_bytes': len(chip.flash),
        }
        if not self.usb:
            costs = {name: sum(self.pc_cycles[address:address + size])
                     for name, (address, size) in symbols.items()
                     if size and address < len(self.pc_cycles)}
            result['top_foreground_symbols_percent_of_cpu'] = {
                name: round(100 * count / self.elapsed, 3) for name, count in
                sorted(costs.items(), key=lambda item: item[1], reverse=True)[:8]}
        return result


class Profile:
    def __init__(self, symbols):
        self.symbols = symbols
        self.poll_pc = symbols['(anonymous namespace)::serviceUsbOut(unsigned long)'][0]
        self.service_points = None
        self.uarts = [symbols[name][0] & 0xffff for name in ('usbUart', 'midiUart1', 'midiUart2')]
        ports, size = symbols['(anonymous namespace)::ports']
        self.ports = [ports & 0xffff, (ports & 0xffff) + size // 2]
        self.chips = {}
        self.peaks = Counter()
        self.samples = 0
        self.recording = False

    def observe(self, chip, pc, cycles):
        profile = self.chips.get(chip)
        if profile is None:
            profile = self.chips[chip] = CpuProfile(chip)
        profile.instruction(chip, pc, cycles, self.recording)
        if self.recording and not profile.usb:
            if self.service_points is None:
                self.service_points = {self.poll_pc} | {
                    address + size for address, (size, op, _, target) in chip.instructions.items()
                    if target == self.poll_pc and op in ('rcall', 'call')}
            if pc in self.service_points:
                self.sample_main(chip.mem)

    def sample_main(self, mem):
        # Sample between service routines, when split 16-bit queue writes are complete.
        self.samples += 1
        def peak(key, value):
            self.peaks[key] = max(self.peaks[key], value)

        for i, uart in enumerate(self.uarts):
            if i:
                head = mem[uart] | mem[uart + 517] << 8
                tail = mem[uart + 1] | mem[uart + 518] << 8
                peak(f'din{i}_raw_rx', (head - tail) % 0x2f00)
            else:
                peak('main_link_rx', (mem[uart] - mem[uart + 1]) % 256)
            peak(f'uart{i}_tx', (mem[uart + 2] - mem[uart + 3]) % 256)
        for i, port in enumerate(self.ports, 1):
            for offset, name in ((0, 'to_din'), (8, 'to_host')):
                ptr = port + offset
                mask = word(mem, ptr + 2)
                assert mask == 8191, 'Queue layout changed; check the matching ELF'
                peak(f'p{i}_{name}', (word(mem, ptr + 4) - word(mem, ptr + 6)) & mask)


class TimedOutBanks(deque):
    def __init__(self, usb, arrivals):
        super().__init__()
        self.usb, self.arrivals = usb, arrivals

    def popleft(self):
        bank = super().popleft()
        self.arrivals.append((self.usb.cycles, bank))
        return bank


class TimedInBanks(list):
    def __init__(self, usb, arrivals):
        super().__init__()
        self.usb, self.arrivals = usb, arrivals

    def append(self, bank):
        self.arrivals.append((self.usb.cycles, bank))
        super().append(bank)


def event_times(banks, cable):
    result = []
    for timestamp, bank in banks:
        for i in range(0, len(bank), 4):
            header = bank[i]
            if header >> 4 == cable:
                result.extend([timestamp] * CIN_LENGTH[header & 15])
    return result


def microseconds(values):
    values = sorted(values)
    assert values and values[0] >= 0
    return {key: round(values[ceil(len(values) * fraction) - 1] / 16, 3)
            for key, fraction in [('p50', 0.5), ('p95', 0.95), ('p99', 0.99), ('max', 1.0)]}


def profile(args):
    objdump = args.objdump or shutil.which('avr-objdump') or (
        Path.home() / '.platformio/packages/toolchain-atmelavr/bin/avr-objdump')
    nm = Path(objdump).with_name('avr-nm')
    # Compare actual flash bytes; Intel HEX line widths and line endings can vary.
    with tempfile.TemporaryDirectory() as temp:
        objcopy = Path(objdump).with_name('avr-objcopy')
        files = [args.elf, args.images / 'megacommand-diy-dual-atmega2560-update.hex']
        binaries = []
        for i, image in enumerate(files):
            binary = Path(temp) / f'flash-{i}.bin'
            source_format = 'ihex' if image.suffix == '.hex' else 'elf32-avr'
            subprocess.run([objcopy, '-I', source_format, '-O', 'binary', '-R', '.eeprom',
                            image, binary], check=True)
            binaries.append(binary.read_bytes())
        if binaries[0] != binaries[1]:
            raise ValueError('ATmega2560 ELF and HEX flash contents differ')
    symbols = elf_symbols(args.elf, nm)
    tracker = Profile(symbols)
    bench = Bench(args.images, objdump, tracker.observe)
    bench.negotiate(7)
    # The optional four-record fixture is useful for measuring shorter transfers.
    records = replay_records(args.capture) if args.case == '8x' else [
        b'\xf0\x7d' + bytes(range(128)) * 188 + b'\xf7']
    first_cycle = bench.main.cycles
    tracker.recording = True
    usb_in, usb_out = [], []
    assert not bench.usb.host_out
    bench.usb.in_banks = TimedInBanks(bench.usb, usb_in)
    bench.usb.host_out = TimedOutBanks(bench.usb, usb_out)
    bench.usb.max_tx_count = 0
    bench.replay(records, (1, 2), 128 if args.case == 'stress' else 0, realtime=True)
    metrics = {}
    for port in (1, 2):
        reads = event_times(usb_in, port - 1)
        host_out = event_times(usb_out, port - 1)
        din = [(at, byte) for at, byte in bench.main.outputs[port] if byte != 0xfe]
        assert len(din) == len(host_out)
        period = byte_cycles(bench.divisor(port))
        # Bench.input_din schedules a 100 µs lead followed by one serial character.
        arrived = [first_cycle + 1600 + (i + 1) * period for i in range(len(reads))]
        gaps = [b[0] - a[0] for a, b in zip(din, din[1:]) if a[1] != 0xf7]
        metrics[f'p{port}'] = {
            'din_to_usb_bytes': len(reads), 'usb_to_din_bytes': len(din),
            'din_to_usb_bytes_per_s': round(len(reads) * 16e6 / (reads[-1] - first_cycle)),
            'usb_to_din_bytes_per_s': round(len(din) * 16e6 / (din[-1][0] - first_cycle)),
            'din_rx_complete_to_usb_host_read_us': microseconds([a - b for a, b in zip(reads, arrived)]),
            'first_usb_out_bank_to_first_din_byte_us': (din[0][0] - host_out[0]) / 16,
            'usb_out_bank_admission_to_din_complete_us': microseconds([a[0] - b for a, b in zip(din, host_out)]),
            'within_record_din_gap_us': microseconds(gaps),
            'within_record_gap_peak_to_peak_us': (max(gaps) - min(gaps)) / 16,
        }
    metrics['atmega2560'] = tracker.chips[bench.main].result(bench.main, symbols)
    metrics['atmega16u2'] = tracker.chips[bench.usb].result(bench.usb, symbols)
    metrics['queue_high_water_bytes'] = {**tracker.peaks, 'usb_link_tx_exact': bench.usb.max_tx_count}
    metrics['main_queue_samples'] = tracker.samples
    metrics['case'] = args.case
    metrics['sha256'] = {image.name: hashlib.sha256(image.read_bytes()).hexdigest() for image in sorted(
        args.images.glob('megacommand-diy-dual-atmega*.hex'))}
    return metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    replay_args(parser)
    parser.add_argument('--elf', type=Path, default=PROJECT / '.pio/build/megacommand_diy/firmware.elf',
                        help='matching ATmega2560 ELF for symbol lookup')
    parser.add_argument('--case', choices=['stress', '8x'], default='stress')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if args.capture and args.case != '8x':
        parser.error('use --case 8x to profile a supplied capture')
    args.output.write_text(json.dumps(profile(args), indent=2) + '\n')


if __name__ == '__main__':
    main()
