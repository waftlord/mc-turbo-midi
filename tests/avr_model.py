"""Small AVR instruction and endpoint model for MegaCommand transport replays.

Adapted from the project's standalone TM-1 transport model. USART timing and
wiring between processors are supplied by paired_avr_replay.py.
"""

from collections import deque
from dataclasses import dataclass, field
import re
import subprocess


SREG = 0x5F
SPL = 0x5D
SPH = 0x5E
RXOUTI = 0x04
TXINI = 0x01
FIFOCON = 0x80
RWAL = 0x20
OUT_EP = 1
CIN_LENGTH = (0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1)


def load_hex(path, objdump, architecture):
    line_pattern = re.compile(r"^\s*([\da-f]+):\s+((?:[\da-f]{2} )+[\da-f]{2})\s+(\S+)\s*([^;]*)(?:;\s*(.*))?$")
    disassembly = subprocess.check_output(
        [objdump, "-D", "-z", "-m", architecture, path], text=True,
    )
    instructions = {}
    flash = {}
    for line in disassembly.splitlines():
        match = line_pattern.match(line)
        if not match:
            continue
        address, encoded, opcode, operands, comment = match.groups()
        address = int(address, 16)
        encoded = bytes.fromhex(encoded)
        flash.update((address + offset, byte) for offset, byte in enumerate(encoded))
        args = re.sub(r"\s+", "", operands).split(",") if operands.strip() else []
        args = [int(arg[1:]) if re.fullmatch(r"r\d+", arg) else arg for arg in args]
        if comment and comment.startswith("0x"):
            target = int(comment.split()[0], 16)
        else:
            target = None
        instructions[address] = len(encoded), opcode, args, target
    assert instructions and flash, path
    return instructions, flash


class Avr:
    """Subset exercised by these images, with AVR flags and an SRAM stack."""

    def __init__(self, image, sram_size, return_bytes):
        self.instructions, self.flash = image
        self.r = bytearray(32)
        self.mem = bytearray(sram_size)
        self.return_bytes = return_bytes
        self.pc = 0
        self.cycles = 0
        self.lowest_stack = sram_size - 1

    def read(self, address):
        return self.r[address] if address < 32 else self.mem[address]

    def write(self, address, value):
        if address < 32:
            self.r[address] = value
        else:
            self.mem[address] = value

    def word(self, register):
        return self.r[register] | (self.r[register + 1] << 8)

    def set_word(self, register, value):
        self.r[register] = value & 0xFF
        self.r[register + 1] = (value >> 8) & 0xFF

    def address(self, operand):
        register = {"X": 26, "Y": 28, "Z": 30}[operand.strip("-")[0]]
        base = self.word(register)
        if operand.startswith("-"):
            self.set_word(register, base - 1)
            return base - 1
        if operand.endswith("+"):
            self.set_word(register, base + 1)
        return base + (int(operand[2:], 0) if len(operand) > 2 else 0)

    @property
    def sp(self):
        return self.mem[SPL] | (self.mem[SPH] << 8)

    @sp.setter
    def sp(self, value):
        self.mem[SPL] = value & 0xFF
        self.mem[SPH] = value >> 8
        self.lowest_stack = min(self.lowest_stack, value)

    def push(self, byte):
        address = self.sp
        assert 0x100 <= address < len(self.mem), (hex(self.pc), hex(address))
        self.mem[address] = byte
        self.sp = address - 1

    def pop(self):
        self.sp += 1
        return self.mem[self.sp]

    def call(self, target):
        return_pc = self.pc // 2
        for byte in range(self.return_bytes):
            self.push((return_pc >> (8 * byte)) & 0xFF)
        self.pc = target

    def interrupt(self, vector):
        assert self.mem[SREG] & 0x80, hex(self.pc)
        self.mem[SREG] &= 0x7F
        self.call(vector)
        self.cycles += 4

    def set_flags(self, result, overflow=False, carry=None, chained=False, half=None, width=8):
        previous = self.mem[SREG]
        negative = bool(result & (1 << (width - 1)))
        zero = not (result & ((1 << width) - 1)) and (not chained or previous & 2)
        self.mem[SREG] = (
            (previous & 0xC0)
            | ((previous & 0x20) if half is None else (0x20 if half else 0))
            | (0x10 if negative != overflow else 0)
            | (8 if overflow else 0) | (4 if negative else 0) | (2 if zero else 0)
            | ((previous & 1) if carry is None else int(carry))
        )

    def step(self):
        address = self.pc
        size, op, args, target = self.instructions[address]
        self.pc += size
        cycles = 1
        if op in {"brcc", "brcs", "breq", "brne", "brge", "brlt", "brmi", "brpl"}:
            mask, set_bit = {
                "brcc": (1, False), "brcs": (1, True),
                "breq": (2, True), "brne": (2, False),
                "brge": (16, False), "brlt": (16, True),
                "brmi": (4, True), "brpl": (4, False),
            }[op]
            if bool(self.mem[SREG] & mask) == set_bit:
                self.pc = target
                cycles = 2
        elif op in {"jmp", "rjmp", "ijmp", "eijmp"}:
            if target is None:
                target = 2 * (self.word(30) + (self.mem[0x5C] << 16 if op == "eijmp" else 0))
            self.pc = target
            cycles = 3 if op == "jmp" else 2
        elif op in {"call", "rcall", "icall", "eicall"}:
            if target is None:
                target = 2 * (self.word(30) + (self.mem[0x5C] << 16 if op == "eicall" else 0))
            self.call(target)
            cycles = (4 if op == "call" else 3) + (self.return_bytes - 2)
        elif op in {"ret", "reti"}:
            self.pc = 0
            for _ in range(self.return_bytes):
                self.pc = self.pc * 256 + self.pop()
            self.pc *= 2
            if op == "reti":
                self.mem[SREG] |= 0x80
            cycles = self.return_bytes + 2
        elif op == "cli":
            self.mem[SREG] &= 0x7F
        elif op == "sei":
            self.mem[SREG] |= 0x80
        elif op in {"nop", "wdr"}:
            pass
        elif op in {"sbrc", "sbrs", "sbic", "sbis", "cpse"}:
            a, b = args
            if op == "cpse":
                skip = self.r[a] == self.r[b]
            else:
                value = self.r[a] if op.startswith("sbr") else self.read(0x20 + int(a, 0))
                skip = bool(value & (1 << int(b, 0))) == (op in {"sbrs", "sbis"})
            if skip:
                skipped_size = self.instructions[self.pc][0]
                self.pc += skipped_size
                cycles += skipped_size // 2
        elif op in {"cp", "cpc", "cpi", "add", "adc", "sub", "subi", "sbci", "sbc"}:
            a, b = args
            left = self.r[a]
            right = self.r[b] if isinstance(b, int) else int(b, 0)
            carry = self.mem[SREG] & 1 if op in {"adc", "cpc", "sbci", "sbc"} else 0
            plus = op in {"add", "adc"}
            total = left + right + carry if plus else left - right - carry
            overflow = bool((~(left ^ right) if plus else (left ^ right)) & (left ^ total) & 0x80)
            half = ((left & 15) + (right & 15) + carry > 15 if plus
                    else (left & 15) - (right & 15) - carry < 0)
            self.set_flags(total, overflow, total > 255 if plus else total < 0,
                           op in {"cpc", "sbci", "sbc"}, half)
            if op not in {"cp", "cpc", "cpi"}:
                self.r[a] = total & 0xFF
        elif op in {"and", "andi", "or", "ori", "eor", "dec", "inc", "neg", "com"}:
            a = args[0]
            left = self.r[a]
            if op in {"dec", "inc", "neg", "com"}:
                result = {"dec": left - 1, "inc": left + 1, "neg": -left, "com": ~left}[op] & 0xFF
            else:
                b = args[1]
                right = self.r[b] if isinstance(b, int) else int(b, 0)
                result = left & right if op in {"and", "andi"} else left | right if op in {"or", "ori"} else left ^ right
            overflow = (op in {"dec", "neg"} and left == 0x80) or (op == "inc" and left == 0x7F)
            carry = (result != 0) if op == "neg" else True if op == "com" else None
            self.set_flags(result, overflow, carry, half=bool((result | left) & 8) if op == "neg" else None)
            self.r[a] = result
        elif op in {"adiw", "sbiw"}:
            a, b = args
            left = self.word(a)
            total = left + int(b, 0) * (1 if op == "adiw" else -1)
            overflow = (left < 0x8000 <= total) if op == "adiw" else (left >= 0x8000 > total)
            self.set_flags(total, overflow, total < 0 or total > 0xFFFF, width=16)
            self.set_word(a, total)
            cycles = 2
        elif op in {"lsr", "ror", "asr", "swap"}:
            a = args[0]
            left = self.r[a]
            if op == "swap":
                self.r[a] = ((left << 4) | (left >> 4)) & 0xFF
            else:
                high = (self.mem[SREG] & 1) << 7 if op == "ror" else left & 0x80 if op == "asr" else 0
                result = (left >> 1) | high
                self.r[a] = result
                self.set_flags(result, bool(high) != bool(left & 1), bool(left & 1))
        elif op in {"sbi", "cbi"}:
            a, b = args
            a = int(a, 0) + 0x20
            mask = 1 << int(b, 0)
            self.write(a, self.read(a) | mask if op == "sbi" else self.read(a) & ~mask)
            cycles = 2
        elif op in {"bst", "bld"}:
            register, bit = args
            mask = 1 << int(bit)
            if op == "bst":
                self.mem[SREG] = (self.mem[SREG] & 0xBF) | (0x40 if self.r[register] & mask else 0)
            else:
                self.r[register] = (self.r[register] & ~mask) | (mask if self.mem[SREG] & 0x40 else 0)
        elif op == "ldi":
            self.r[args[0]] = int(args[1], 0)
        elif op == "mov":
            self.r[args[0]] = self.r[args[1]]
        elif op == "movw":
            self.set_word(args[0], self.word(args[1]))
        elif op == "in":
            self.r[args[0]] = self.read(0x20 + int(args[1], 0))
        elif op == "out":
            self.write(0x20 + int(args[0], 0), self.r[args[1]])
        elif op in {"ld", "ldd", "lpm", "elpm"}:
            destination, operand = args or (0, "Z")
            source = self.address(operand)
            if op == "elpm":
                source += self.mem[0x5B] << 16
            self.r[destination] = self.flash[source] if op in {"lpm", "elpm"} else self.read(source)
            cycles = 3 if op in {"lpm", "elpm"} else 2
        elif op in {"st", "std"}:
            self.write(self.address(args[0]), self.r[args[1]])
            cycles = 2
        elif op == "lds":
            self.r[args[0]] = self.read(int(args[1], 0))
            cycles = 2
        elif op == "sts":
            self.write(int(args[0], 0), self.r[args[1]])
            cycles = 2
        elif op == "push":
            self.push(self.r[args[0]])
            cycles = 2
        elif op == "pop":
            self.r[args[0]] = self.pop()
            cycles = 2
        elif op in {"mul", "muls", "mulsu"}:
            left, right = (self.r[register] for register in args)
            if op != "mul" and left & 0x80:
                left -= 256
            if op == "muls" and right & 0x80:
                right -= 256
            result = left * right
            self.set_word(0, result)
            self.mem[SREG] = (self.mem[SREG] & 0xFC) | (1 if result & 0x8000 else 0) | (2 if not result else 0)
            cycles = 2
        else:
            raise ValueError(f"Unsupported instruction at {address:#x}: {op} {args}")
        self.cycles += cycles

    def until(self, condition, limit=1_000_000):
        deadline = self.cycles + limit
        while not condition():
            assert self.cycles < deadline, (hex(self.pc), self.cycles, limit)
            self.step()
        return self


@dataclass
class Endpoint:
    control: int = 0
    direction: int = 0
    config: int = 0
    interrupt: int = 0
    txini: bool = True
    rxouti: bool = False
    out_owned: bool = False
    setup: bool = False
    fifo_error: bool = False
    data: deque = field(default_factory=deque)
    pending: bytes | None = None

    @property
    def capacity(self):
        return 8 << ((self.config >> 4) & 7)

    @property
    def enabled(self):
        return bool(self.control & 1 and self.config & 2)

    @property
    def flags(self):
        if not self.enabled:
            return 0
        control_endpoint = self.capacity == 8
        flags = TXINI if self.txini and (self.direction & 1 or control_endpoint) else 0
        flags |= RXOUTI if self.rxouti else 0
        flags |= 8 if self.setup else 0
        if self.direction & 1:
            if self.pending is None:
                flags |= FIFOCON
                if len(self.data) < self.capacity and not self.control & 0x20 and not self.fifo_error:
                    flags |= RWAL
        elif self.out_owned:
            flags |= FIFOCON | (RWAL if self.data and not self.control & 0x20 and not self.fifo_error else 0)
        return flags


class UsbAvr(Avr):
    def __init__(self, image):
        super().__init__(image, 0x300, 2)
        self.endpoints = [Endpoint() for _ in range(8)]
        self.host_out = deque()
        self.in_wire = bytearray()
        self.pause_in_until = 0
        self.poll_every = 16000
        self.next_poll = 0
        self.next_timer = 16000
        self.max_tx_count = 0
        self.max_in_bank = 0
        self.usb_out_banks = 0
        self.control_in = []
        self.control_payload = None
        self.control_response_bytes = None
        self.check_endpoint_writes = False

    @property
    def endpoint(self):
        return self.endpoints[self.mem[0xE9]]

    def read(self, address):
        endpoint = self.endpoint
        if address == 0x49:  # PLL lock, modeled as immediate when enabled.
            return self.mem[address] | bool(self.mem[address] & 2)
        if address == 0xE4:
            return (self.cycles // 16000) & 0xFF
        if address == 0xE5:
            return (self.cycles // 16000 >> 8) & 7
        if address == 0xE8:
            return endpoint.flags
        if address == 0xEB:
            return endpoint.control
        if address == 0xEC:
            return endpoint.direction
        if address == 0xED:
            return endpoint.config
        if address == 0xEE:
            return 0x80 if endpoint.enabled else 0
        if address == 0xF0:
            return endpoint.interrupt
        if address == 0xF1:
            assert endpoint.enabled and not endpoint.control & 0x20 and not endpoint.fifo_error and endpoint.data, (hex(self.pc), self.mem[0xE9])
            return endpoint.data.popleft()
        if address == 0xF2:
            return len(endpoint.data)
        if address == 0xF4:
            return sum(int(ep.flags & ep.interrupt != 0) << index
                       for index, ep in enumerate(self.endpoints))
        return super().read(address)

    def write(self, address, value):
        endpoint = self.endpoint
        if address == 0xEB:
            endpoint.control = value
            return
        if address == 0xEC:
            endpoint.direction = value
            return
        if address == 0xED:
            endpoint.config = value
            return
        if address == 0xF0:
            endpoint.interrupt = value
            return
        if address == 0xE1:
            self.mem[address] &= value  # Interrupt flags are cleared by writing zero.
            return
        if address == 0xEA:
            for index, selected in enumerate(self.endpoints):
                if value & (1 << index):
                    selected.data.clear()
                    selected.pending = None
                    selected.txini, selected.rxouti = True, False
        if address == 0xE8:
            assert endpoint.enabled, (hex(self.pc), self.mem[0xE9])
            if endpoint.direction & 1 and value & RXOUTI and self.check_endpoint_writes:
                raise AssertionError(f"IN KILLBK asserted at {self.pc:#x}")
            had_setup = endpoint.setup
            if not value & 8:
                endpoint.setup = False
                if had_setup and self.control_payload is not None:
                    endpoint.data = self.control_payload
                    endpoint.rxouti = True
                    self.control_payload = None
            if not value & RXOUTI and not had_setup:
                endpoint.rxouti = False
            if not value & TXINI:
                endpoint.txini = False
            if endpoint.capacity == 8:
                if not endpoint.txini:
                    self.control_in.append(bytes(endpoint.data))
                    if self.control_response_bytes is not None:
                        self.control_response_bytes -= len(endpoint.data)
                        if self.control_response_bytes <= 0:
                            endpoint.rxouti = True
                            self.control_response_bytes = None
                    endpoint.data.clear()
                    endpoint.txini = True
            elif not value & FIFOCON:
                if endpoint.direction & 1:
                    assert not endpoint.txini and endpoint.pending is None, hex(self.pc)
                    endpoint.pending = bytes(endpoint.data)
                    self.max_in_bank = max(self.max_in_bank, len(endpoint.pending))
                else:
                    assert not endpoint.rxouti, hex(self.pc)
                    endpoint.out_owned = False
                    self.usb_out_banks += 1
                endpoint.data.clear()
            return
        if address == 0xF1:
            assert endpoint.enabled and not endpoint.control & 0x20 and (endpoint.direction & 1 or endpoint.capacity == 8), hex(self.pc)
            assert len(endpoint.data) < endpoint.capacity, (hex(self.pc), len(endpoint.data))
            endpoint.data.append(value)
            return
        if address == 0x1DC:
            assert value <= 128, (hex(self.pc), value)
            self.max_tx_count = max(self.max_tx_count, value)
        return super().write(address, value)

    def run_isr(self, vector):
        previous_pc, sp, previous_flags, registers = self.pc, self.sp, self.mem[SREG], bytes(self.r)
        self.interrupt(vector)
        self.until(lambda: self.pc == previous_pc and self.sp == sp)
        assert self.mem[SREG] == previous_flags and self.r == registers, (hex(vector), hex(previous_pc))

    def signal_usb_reset(self):
        for ep in self.endpoints:
            ep.control = ep.config = ep.interrupt = 0
            ep.direction = 0
            ep.txini, ep.rxouti, ep.setup = True, False, False
            ep.out_owned = False
            ep.fifo_error = False
            ep.data.clear()
            ep.pending = None
        self.mem[0xE1] |= 8

    def usb_reset(self):
        self.signal_usb_reset()
        self.run_isr(0x2C)
        assert self.mem[0x3E] == 2 and self.endpoints[0].enabled

    def control(self, request, data=None, response=None):
        ep0 = self.endpoints[0]
        ep0.setup = ep0.txini = True
        ep0.data = deque(request)
        # The host acknowledges a status IN immediately after it is armed.
        status_count = len(self.control_in)
        self.control_payload = deque(data) if data is not None else None
        self.control_response_bytes = len(response) if response is not None else None
        self.run_isr(0x30)
        result = self.control_in[status_count:]
        if response is None:
            assert result == [b""], (request.hex(), result)
        else:
            assert b"".join(result) == response, (request.hex(), result)

    def load_out_bank(self):
        if self.host_out:
            endpoint = self.endpoints[OUT_EP]
            if endpoint.enabled and not (endpoint.flags & FIFOCON):
                endpoint.data.extend(self.host_out.popleft())
                endpoint.rxouti = endpoint.out_owned = True


class MainAvr(Avr):
    def __init__(self, image, page):
        super().__init__(image, 0x2200, 3)
        self.buttons = 0xDF if page else 0xFF
        self.button_shift = 0
        self.oled_commands = bytearray()
        self.oled_data = bytearray()
        self.next_timer = 16000

    def read(self, address):
        if address == 0x109:
            return (self.buttons >> (7 - self.button_shift)) & 1 if self.button_shift < 8 else 1
        if address == 0x4D:
            return 0x80  # SPI transfer complete.
        return super().read(address)

    def write(self, address, value):
        if address == 0x10B:
            if not value & 2:
                self.button_shift = 0
            if value & ~self.mem[address] & 4:
                self.button_shift += 1
        if address == 0x4E and not self.mem[0x10B] & 0x80:
            buffer = self.oled_data if self.mem[0x10B] & 0x20 else self.oled_commands
            buffer.append(value)
        return super().write(address, value)


def packetize(message):
    events = []
    for offset in range(0, len(message), 3):
        data = message[offset:offset + 3]
        cin = 4 if offset + 3 < len(message) else 4 + len(data)
        events.append(bytes([cin]) + data.ljust(3, b"\0"))
    return b"".join(events)


def midi_bytes(events):
    assert not len(events) % 4
    return b"".join(events[i + 1:i + 1 + CIN_LENGTH[events[i] & 15]]
                    for i in range(0, len(events), 4))


def messages(data):
    frames = [frame + b"\xf7" for frame in data.split(b"\xf7")[:-1]]
    assert b"".join(frames) == data
    assert all(frame[0] == 0xF0 and all(byte < 0x80 for byte in frame[1:-1]) for frame in frames)
    return frames
