#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <util/atomic.h>
#include <stddef.h>

#include "BridgeWire.h"
#include "FastUart.h"
#include "MidiPacketizer.h"
#include "MidiStream.h"
#include "Panel.h"
#include "TurboLink.h"
#include "TurboTestSerial.h"

extern "C" {
FastUart usbUart, midiUart1, midiUart2;
volatile uint8_t fastTestUart;
}
static_assert(offsetof(FastUart, rx) == 5 && offsetof(FastUart, tx) == UART_TX_BUFFER &&
              offsetof(FastUart, rxEpoch) == UART_RX_EPOCH && offsetof(FastUart, tailEpoch) == UART_TAIL_EPOCH &&
              offsetof(FastUart, testIdle) == UART_TEST_IDLE &&
              offsetof(FastUart, testTxDrain) == UART_TEST_TX_DRAIN &&
              offsetof(FastUart, firstRxLoss) == UART_FIRST_RX_LOSS &&
              offsetof(FastUart, lastRxLoss) == UART_LAST_RX_LOSS,
              "Keep the assembly queue layout in sync");

namespace {

constexpr uint8_t noLimit = 0xff;
constexpr uint16_t initialCredit = BRIDGE_DIN_CREDIT;
constexpr uint16_t sysexStartWatermark = 7168;
constexpr uint16_t controlIdleTimeout = 750;
constexpr uint16_t toHostSize = 8192;
constexpr uint16_t toDinSize = 8192;
constexpr uint16_t memoryStart = 0x2200; // XMEM, bank 0
static_assert(memoryStart + 2 * (toHostSize + toDinSize) <= DIN_1_RX &&
              DIN_1_RX + DIN_RX_SIZE <= DIN_2_RX &&
              DIN_2_RX + uint32_t(DIN_RX_SIZE) <= 0x10000UL,
              "External queues must fit bank 0");
constexpr uint8_t dividerForSpeed[] = {31, 31, 15, 31, 7, 31, 31, 3};
constexpr uint8_t testDivider = F_CPU / (2 * TurboTestSerial::sampleRate) - 1;
constexpr uint8_t testLsbFirst = _BV(2); // UDORD in master SPI mode
static_assert(F_CPU == (testDivider + 1) * 2 * TurboTestSerial::sampleRate,
              "The Turbo test sample clock must divide the CPU clock exactly");
constexpr uint8_t turboSpeedRequest = 0x10;
constexpr uint8_t turboSpeedAnswer = 0x11;
constexpr uint8_t turboSpeedNegotiation = 0x12;
constexpr uint8_t turboSpeedAck = 0x13;
uint8_t fastTestOwner = noLimit;
volatile uint32_t clockMs = 0;
Panel panel;
BridgeReceiver receiver;
uint8_t returnBatchCredits = 0;
bool linkReady = false;
bool usbReady = false;
bool resetPending = false;
bool usbUpdate = false;
uint8_t returnBanks = 0;
uint8_t nextPort = 0;
uint32_t lastDeviceGrant = 0;
uint32_t lastData = 0;
uint32_t lastHeartbeat = 0;

class Queue {
public:
    Queue(uint16_t address, uint16_t size)
        : bytes_(reinterpret_cast<uint8_t *>(address)), mask_(size - 1) {}
    uint16_t size() const { return (head_ - tail_) & mask_; }
    uint16_t free() const { return mask_ - size(); }
    bool empty() const { return head_ == tail_; }
    uint8_t front() const { return bytes_[tail_]; }
    void push(uint8_t value) {
        bytes_[head_] = value;
        head_ = (head_ + 1) & mask_;
    }
    void pop() { tail_ = (tail_ + 1) & mask_; }
    uint8_t copySysExData(FastUart &uart, uint8_t &head, uint8_t limit) {
        const uint16_t queued = size();
        const uint8_t available = queued < limit ? queued : limit;
        uint16_t cursor = tail_;
        uint8_t count = 0;
        while (count < available) {
            const uint8_t byte = bytes_[cursor];
            if (byte >= 0x80) break;
            uart.tx[head++] = byte;
            ++count;
            cursor = (cursor + 1) & mask_;
        }
        tail_ = cursor;
        return count;
    }
    void pushThree(const uint8_t *bytes) {
        uint8_t *out = bytes_ + head_;
        const uint16_t next = (head_ + 3) & mask_;
        if (head_ + 2 <= mask_) {
            out[0] = bytes[0];
            out[1] = bytes[1];
            out[2] = bytes[2];
            head_ = next;
        } else {
            push(bytes[0]);
            push(bytes[1]);
            push(bytes[2]);
        }
    }
    void pushEvent(const uint8_t *event) {
        uint8_t *out = bytes_ + head_;
        out[0] = event[0];
        out[1] = event[1];
        out[2] = event[2];
        out[3] = event[3];
        head_ = (head_ + 4) & mask_;
    }
    void popEvent(uint8_t *event) {
        const uint8_t *in = bytes_ + tail_;
        event[0] = in[0];
        event[1] = in[1];
        event[2] = in[2];
        event[3] = in[3];
        tail_ = (tail_ + 4) & mask_;
    }
    void clear() { tail_ = head_; }

private:
    uint8_t *bytes_;
    uint16_t mask_;
    uint16_t head_ = 0;
    uint16_t tail_ = 0;
};

uint32_t nowMs() {
    uint32_t now;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { now = clockMs; }
    return now;
}

void configureUart(FastUart &queue, volatile uint8_t *reg, uint8_t divisor, uint8_t mode = 0) {
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        reg[1] = 0;
        queue.rxTail = queue.rxHead;
        queue.tailEpoch = queue.rxEpoch;
        queue.error = 0;
        reg[5] = 0;
        reg[4] = divisor;
        reg[2] = _BV(UCSZ01) | _BV(UCSZ00);
        reg[0] = _BV(TXC0) | mode;
        reg[1] = _BV(RXEN0) | _BV(TXEN0) | _BV(RXCIE0);
    }
}

bool serviceMidiStartup(uint32_t now) {
    // A late CDC line-coding change or a partial serial frame can swallow
    // a startup token. Retry until MIDI answers or the startup window ends.
    constexpr uint16_t quietMs = BRIDGE_CDC_FRAME_GAP_MS + 20;
    constexpr uint16_t reattachMs = 350;
    constexpr uint8_t maximumAttempts = 8;
    enum class Stage : uint8_t { Midi, BeforeFast, BeforeDefault, RestoreMidi, Done };
    static Stage stage = Stage::Midi;
    static uint32_t nextAction = 250;
    static uint8_t attempts = 0;
    if (stage == Stage::Done) return false;
    if ((stage == Stage::Midi || stage == Stage::BeforeFast) && linkReady) {
        stage = Stage::Done;
        return false;
    }
    if (static_cast<int32_t>(now - nextAction) < 0 ||
        usbUart.txHead != usbUart.txTail || !(UCSR0A & _BV(TXC0))) {
        return stage != Stage::Midi;
    }

    if (stage == Stage::Midi) {
        nextAction = now + quietMs;
        stage = Stage::BeforeFast;
        return true;
    }

    constexpr uint8_t token[] = {BRIDGE_MIDI_STARTUP_BYTES};
    if (stage == Stage::BeforeFast) {
        configureUart(usbUart, &UCSR0A, 16, _BV(U2X0));
        // The token takes just over 1 ms at 115200. Give a receiver at
        // the other baud a full framing timeout before trying again.
        nextAction = now + quietMs + 2;
        stage = Stage::BeforeDefault;
    } else if (stage == Stage::BeforeDefault) {
        configureUart(usbUart, &UCSR0A, 103);
        stage = Stage::RestoreMidi;
    } else {
        configureUart(usbUart, &UCSR0A, 0);
        receiver.phase = 0;
        nextAction = now + reattachMs;
        stage = ++attempts == maximumAttempts ? Stage::Done : Stage::Midi;
        return false;
    }
    writeUart(usbUart, UCSR0B, token, sizeof(token));
    return true;
}

void appendUsbByte(uint8_t &head, uint8_t byte) {
    usbUart.tx[head++] = byte;
}

void appendUsbEscaped(uint8_t &head, uint8_t byte) {
    if (byte >= BRIDGE_ESCAPE) {
        appendUsbByte(head, BRIDGE_ESCAPE);
        byte -= BRIDGE_ESCAPE;
    }
    appendUsbByte(head, byte);
}

void sendFrame(uint8_t type, const uint8_t *data = nullptr, uint8_t length = 0) {
    uint8_t head = usbUart.txHead;
    appendUsbByte(head, BRIDGE_START);
    appendUsbByte(head, type);
    uint8_t crc = bridgeCrc(0, type);
    if (type == BRIDGE_DATA || type == BRIDGE_SYSEX) {
        appendUsbByte(head, length);
        crc = bridgeCrc(crc, length);
    }
    if (type == BRIDGE_SYSEX) {
        // Encode full USB events directly into the link ring. Only the
        // cable-tagged first payload byte can need a wire escape.
        for (uint8_t i = 0; i < length; i += 4) {
            const uint8_t first = data[i + 1] | ((data[i] & 16) << 3);
            appendUsbEscaped(head, first);
            crc = bridgeCrc(crc, first);
            appendUsbByte(head, data[i + 2]);
            crc = bridgeCrc(crc, data[i + 2]);
            appendUsbByte(head, data[i + 3]);
            crc = bridgeCrc(crc, data[i + 3]);
        }
    } else {
        for (uint8_t i = 0; i < length; ++i) {
            const uint8_t byte = data[i];
            appendUsbEscaped(head, byte);
            crc = bridgeCrc(crc, byte);
        }
    }
    appendUsbEscaped(head, crc);
    usbUart.txHead = head;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { UCSR0B |= _BV(UDRIE0); }
}

struct Port {
    Port(uint8_t number, FastUart &uart, volatile uint8_t *registers,
         bool (*sendControl)(const uint8_t *, uint8_t), bool (*idle)(),
         void (*setRate)(uint8_t), void (*notify)(uint8_t), bool (*canTest)())
        : toDin(memoryStart + toDinSize * number, toDinSize),
          toHost(memoryStart + 2 * toDinSize + toHostSize * number, toHostSize),
          turbo(sendControl, idle, setRate, notify, canTest), uart(uart), reg(registers),
          number(number) {}

    bool fastTest() const { return reg[2] & _BV(UMSEL01); }
    bool canUseFastTest() const { return fastTestOwner == noLimit || fastTestOwner == number; }

    void startFastTest() {
        // TXD keeps the same pin in both modes. XCK is local to the AVR
        // and has no connection to the MIDI cable.
        if (number == 0) {
            PORTD |= _BV(PD3);
            DDRD |= _BV(PD3) | _BV(PD5);
        } else {
            PORTH |= _BV(PH1);
            DDRH |= _BV(PH1) | _BV(PH2);
        }
        testDecoder = TurboTestSerial::Decoder{};
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            reg[1] = 0;
            uart.error = 0;
            uart.rxTail = uart.rxHead;
            uart.tailEpoch = uart.rxEpoch;
            uart.testIdle = 4;
            uart.testTxDrain = 0;
            reg[5] = 0;
            reg[4] = 0;
            reg[2] = _BV(UMSEL01) | _BV(UMSEL00) | testLsbFirst;
            reg[1] = _BV(TXEN0) | _BV(RXEN0);
            reg[4] = testDivider;
            reg[0] = _BV(TXC0);
            reg[6] = 0xff;
            while (!(reg[0] & _BV(UDRE0))) {}
            reg[6] = 0xff;
            reg[1] |= _BV(RXCIE0);
            fastTestUart = number + 1;
        }
    }

    void setSpeed(uint8_t code) {
        dinFilter.clear();
        if (code == TurboLink::fastTestSpeed) {
            startFastTest();
        } else {
            const bool wasFast = fastTest();
            if (wasFast) {
                // Stop refilling, then let the remaining idle samples drain
                // with other interrupts enabled before changing modes.
                ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                    fastTestUart = 0;
                    reg[1] = _BV(TXEN0) | _BV(RXEN0);
                }
                while (!(reg[0] & _BV(TXC0))) {}
            }
            configureUart(uart, reg, dividerForSpeed[code]);
            if (fastTestOwner == number) fastTestOwner = noLimit;
            if (wasFast) {
                if (number == 0) DDRD &= ~_BV(PD5);
                else DDRH &= ~_BV(PH2);
            }
        }
        pendingTx = false;
    }

    bool idle() const {
        if (fastTest()) return uart.txHead == uart.txTail && !uart.testTxDrain;
        return !pendingTx || (uart.txHead == uart.txTail && (reg[0] & _BV(TXC0)));
    }

    void write(const uint8_t *data, uint8_t length, uint32_t now) {
        pendingTx = true;
        if (fastTest()) {
            uint8_t head = uart.txHead;
            TurboTestSerial::encode(data, length, [this, &head](uint8_t samples) {
                uart.tx[head++] = samples;
            });
            uart.txHead = head;
        } else {
            writeUart(uart, reg[1], data, length);
        }
        if (data[0] != 0xFE) panel.midiOut(number, now);
    }

    bool sendControl(const uint8_t *data, uint8_t length) {
        if (uart.writable() < (fastTest() ? TurboTestSerial::packedSize(length) : length) ||
            (data[0] < 0xF8 && !outgoing.complete())) {
            return false;
        }
        if (length >= 8 && data[0] == 0xf0) {
            const uint8_t command = data[6];
            const bool reserveTest = (command == turboSpeedRequest && turbo.mayUseFastTest()) ||
                (command == turboSpeedAnswer && (data[7] & TurboLink::fastTestCapability));
            if (reserveTest) {
                if (!canUseFastTest()) return false;
                fastTestOwner = number;
            } else if (fastTestOwner == number && (command == turboSpeedAnswer ||
                       (command == turboSpeedAck && !turbo.selectedFastTest()) ||
                       (command == turboSpeedNegotiation && data[7] != TurboLink::fastTestSpeed))) {
                fastTestOwner = noLimit;
            }
        }
        if (data[0] < 0xF8) outgoing = MidiBoundary{};
        write(data, length, nowMs());
        return true;
    }

    void packet(const uint8_t *event) {
        if (toHost.free() < 4) {
            if (linkReady) fault = Panel::Fault::RxFull;
            inputLoss = true;
            return;
        }
        toHost.pushEvent(event);
    }

    __attribute__((noinline)) void receiveUsb(const uint8_t *event, uint8_t length, uint32_t now) {
        const uint8_t cin = event[0] & 15;
        const uint8_t first = event[1];
        if (first < 0xf8) {
            if ((first == 0xf7 && !hostSysEx) ||
                (first < 0x80 && (cin == 15 ? awaitHostStatus : !hostSysEx))) {
                credit += length;
                return;
            }
            if (first >= 0x80) {
                awaitHostStatus = false;
                if (first == 0xf0 && !hostSysEx) hostSysExStarted = now;
                hostSysEx = first == 0xf0;
            }
            if (event[length] == 0xf7) {
                hostSysEx = false;
                awaitHostStatus = true;
                ++sysexEnds;
            }
            lastHostAt = now;
        }

        // Credits reserve room for the batch and bytes held by the filter.
        if (toDin.free() < uint16_t(length) + ControlFilter::maxBuffered) {
            fault = Panel::Fault::TxFull;
            resetPending = true;
            return;
        }
        if (cin == 4 && event[1] < 0x80 && !usbFilter.pending()) {
            toDin.pushThree(event + 1);
            return;
        }
        for (uint8_t i = 1; i <= length; ++i) {
            usbFilter.accept(event[i], now, [this](uint8_t byte) {
                toDin.push(byte);
            }, [this](const uint8_t *frame, uint8_t size) {
                if (frame[6] == 1) {
                    report = true;
                    // A TM-1 probe from C6 starts negotiation on an idle
                    // link. The MegaCMD status-only query has no side effects.
                    if (frame[1] == 0 && !turbo.busy() && !button &&
                        turbo.speed() == TurboLink::normalSpeed && requestedLimit == noLimit &&
                        turbo.limit() > TurboLink::normalSpeed) {
                        requestedLimit = turbo.limit();
                        requestedLimitFromHost = true;
                        fault = Panel::Fault::None;
                    }
                } else if (frame[1] == 0x7d) {
                    usbUpdate = true;
                } else {
                    requestedLimit = frame[7];
                    requestedLimitFromHost = true;
                    fault = Panel::Fault::None;
                    panel.previewLimit(number, nowMs());
                }
                credit += size;
                if (sysexEnds) --sysexEnds;
            });
        }
    }

    void receiveSysEx(const uint8_t *data, uint32_t now) {
        const uint16_t free = toDin.free();
        if ((data[1] | data[2]) >= 0x80 || free < 3 + ControlFilter::maxBuffered) {
            fault = free < 3 + ControlFilter::maxBuffered ? Panel::Fault::TxFull : Panel::Fault::Usb;
            resetPending = true;
            return;
        }
        if (!hostSysEx) {
            credit += 3;
            return;
        }
        if (!usbFilter.pending()) {
            lastHostAt = now;
            const uint8_t bytes[] = {static_cast<uint8_t>(data[0] & 127), data[1], data[2]};
            toDin.pushThree(bytes);
        } else {
            const uint8_t event[] = {4, static_cast<uint8_t>(data[0] & 127), data[1], data[2]};
            receiveUsb(event, 3, now);
        }
    }

    void recoverInput() {
        if (!inputLoss || toHost.free() < 4) return;
        parser.abortSysEx(number << 4, [this](const uint8_t *event) { packet(event); });
        inputLoss = false;
        awaitStatus = true;
    }

    uint8_t readFastTest(uint8_t *input, uint8_t budget, uint32_t now) {
        uint8_t length = 0;
        for (uint8_t packed; budget && uart.read(packed); --budget) {
            uint8_t byte = 0;
            const auto result = testDecoder.accept(packed, byte);
            if (result == TurboTestSerial::Decoder::Result::BadStop) {
                dinFilter.clear();
                if (!turbo.reportTransportError(now, true)) fault = Panel::Fault::Din;
                return 0;
            }
            if (result == TurboTestSerial::Decoder::Result::Byte) input[length++] = byte;
        }
        return length;
    }

    __attribute__((noinline)) void readDin(uint32_t now) {
        const bool testing = fastTest();
        recoverInput();
        if (turbo.recovering()) {
            // Either peer can return to 1× first. Discard bytes received
            // during that interval, including partial control frames.
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                uart.rxTail = uart.rxHead;
                uart.tailEpoch = uart.rxEpoch;
                uart.error = 0;
            }
            return;
        }
        uint8_t error = 0;
        if (testing) {
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                error = uart.error;
                if (error) {
                    uart.rxTail = uart.rxHead;
                    uart.error = 0;
                }
            }
        }
        uint8_t input[48];
        const uint16_t free = toHost.free();
        // Until a USB host drains the queues, keep the MIDI bytes in the
        // raw ring. Reserve a few extra events for any held SysEx prefix.
        constexpr uint8_t reserve = 4 * ControlFilter::maxBuffered;
        const uint8_t budget = free >= 4 * sizeof(input) + reserve
            ? sizeof(input) : free > reserve ? (free - reserve) / 4 : 0;
        if (!error && !budget && linkReady && !turbo.busy()) {
            // USB backpressure can park the reader for longer than the
            // Turbo peer timeout. Only fresh ISR writes prove it is alive.
            const uint8_t head = uart.rxHead;
            if (head != blockedRxHead) {
                blockedRxHead = head;
                turbo.noteDinActivity(now);
            }
            return;
        }
        const uint8_t readBudget = budget ? budget : sizeof(input);
        const uint8_t length = testing ? (error ? 0 : readFastTest(input, readBudget, now))
                                       : uart.readDin(input, readBudget, error);
        if (!length) dinFilter.expire(now, [this](uint8_t value) { forwardDin(value); });
        if (error && (testing || inputSeen || error != _BV(FE0))) {
            inputLoss = true;
            dinFilter.clear();
            if (!turbo.reportTransportError(now, error == _BV(FE0))) {
                fault = error & (0x80 | _BV(DOR0)) ? Panel::Fault::RxFull : Panel::Fault::Din;
            }
            recoverInput();
            return;
        }
        if (!length) return;
        const auto forward = [this](uint8_t value) { forwardDin(value); };
        bool activity = false;
        bool payload = false;
        const uint8_t cable = number << 4;
        bool payloadPaused = turbo.pausePayload();
        const auto emitRun = [this, budget](const uint8_t *event) {
            if (budget) toHost.pushEvent(event);
            else packet(event);
        };
        for (uint8_t i = 0; i < length; ++i) {
            const uint8_t byte = input[i];
            if (byte < 0x80 && parser.sysex() && !dinFilter.pending() && !payloadPaused) {
                if (!inputLoss) {
                    // The read budget reserves space for all bytes in this run.
                    const uint8_t used = parser.acceptSysExRun(input + i, length - i, cable, emitRun);
                    i += used - 1;
                }
                activity = true;
                payload = true;
                continue;
            }
            turbo.observeDinByte(byte, now);
            if (byte == 0xfe && (turbo.busy() || turbo.speed() != TurboLink::normalSpeed)) continue;
            activity |= byte != 0xfe && (!turbo.busy() || byte != 0);
            payload |= byte < 0xf8;
            const bool isStatus = byte >= 0x80 && byte < 0xf7;
            inputSeen |= isStatus;
            dinFilter.accept(byte, now, forward, [this, now, &payloadPaused](
                                 const uint8_t *frame, uint8_t size) {
                // A consumed DIN control is still a SysEx boundary on the
                // wire. Cancel running status and close any interrupted dump.
                inputLoss = true;
                recoverInput();
                turbo.receive(frame, size, now);
                payloadPaused = turbo.pausePayload();
            });
        }
        if (activity) {
            turbo.noteDinActivity(now);
            panel.midiIn(number, now);
        }
        if (payload) lastDinAt = now;
        dinFilter.expire(now, forward);
    }

    void forwardDin(uint8_t value) {
        if (!linkReady || inputLoss || (turbo.pausePayload() && value < 0xf8)) return;
        if (awaitStatus && value < 0xf8) {
            if (value < 0x80 || value == 0xf7) return;
            awaitStatus = false;
        }
        parser.accept(value, number << 4, [this](const uint8_t *event) { packet(event); });
    }

    void cycleLimit(uint32_t now) {
        const uint8_t current = requestedLimit == noLimit ? turbo.limit() : requestedLimit;
        requestedLimit = current < 2 ? 2 : current < 4 ? 4 : current < 7 ? 7 : 1;
        requestedLimitFromHost = false;
        fault = Panel::Fault::None;
        panel.previewLimit(number, now);
    }

    __attribute__((noinline)) void serviceDin(uint32_t now) {
        if (closeDin && fastTest()) closeDin = false;
        if (closeDin) {
            if (!uart.writable()) return;
            const uint8_t end = 0xf7;
            write(&end, 1, now);
            closeDin = false;
        }
        usbFilter.expire(now, [this](uint8_t byte) { toDin.push(byte); });
        const bool waitingForControl = button || requestedLimit != noLimit;
        if (waitingForControl && !turbo.recovering()) {
            // A manual speed change cannot wait forever for a missing EOX
            // or data byte. Leave idle messages alone until a change is requested.
            if (parser.sysex() && now - lastDinAt >= controlIdleTimeout && !uart.dinReadable()) {
                inputLoss = true;
                dinFilter.clear();
                recoverInput();
            }
            if (!outgoing.complete() && toDin.empty() && idle() &&
                now - lastHostAt >= controlIdleTimeout) {
                closeDin = outgoing.sysex();
                outgoing = MidiBoundary{};
                hostSysEx = false;
                awaitHostStatus = true;
                if (closeDin) return;
            }
        }
        const bool mayStartFastTest = requestedLimit == noLimit
            ? turbo.limit() == TurboLink::maximumSpeed || turbo.limit() == TurboLink::normalSpeed
            : requestedLimit >= TurboLink::maximumSpeed;
        const bool waitingForTest = fastTestOwner != noLimit && fastTestOwner != number &&
            !turbo.busy() && turbo.speed() == TurboLink::normalSpeed && mayStartFastTest;
        if (waitingForControl && !waitingForTest && (turbo.recovering() ||
            (outgoing.complete() && !parser.sysex() && !dinFilter.pending() && idle()))) {
            if (requestedLimit != noLimit) {
                turbo.setLimit(requestedLimit, now);
                requestedLimit = noLimit;
                requestedLimitFromHost = false;
            } else {
                turbo.toggle(now);
                button = false;
            }
        }
        turbo.tick(now);
        if (fastTestOwner == number) {
            if (turbo.recovering() && fastTest() && idle()) setSpeed(TurboLink::normalSpeed);
            if (!turbo.busy() || (turbo.recovering() && !fastTest())) fastTestOwner = noLimit;
        }
        if (turbo.pausePayload() && parser.sysex()) inputLoss = true;
        if (turbo.pausePayload() || !linkReady || resetPending) return;
        uint8_t count = 0;
        uint8_t head = uart.txHead;
        const uint8_t capacity = uart.writable();
        // A run of SysEx data cannot change the MIDI boundary. Leave
        // realtime bytes and the terminator for the ordinary path.
        if (outgoing.sysex()) count = toDin.copySysExData(uart, head, capacity);
        while (count < capacity && !toDin.empty()) {
            if (waitingForControl && outgoing.complete()) break;
            const uint8_t byte = toDin.front();
            // Only prefill a dump that the host is still sending. A channel
            // status can end an interrupted SysEx without an explicit EOX.
            if (byte == 0xf0 && hostSysEx && !sysexEnds && now - lastHostAt < 150) {
                const uint16_t queued = toDin.size();
                const uint32_t age = now - hostSysExStarted;
                // A slowly streamed record cannot prefill fast enough to
                // keep the DIN UART busy. Bound startup for that sender,
                // while allowing a dense transfer to build its reservoir.
                if (queued < sysexStartWatermark && age < 1250 &&
                    (age < 150 || queued >= age)) break;
            }
            toDin.pop();
            if (byte == 0xf7 && sysexEnds) --sysexEnds;
            uart.tx[head++] = byte;
            ++count;
            if (!outgoing.sysex() || byte >= 0x80) outgoing.accept(byte);
        }
        if (count) {
            pendingTx = true;
            uart.txHead = head;
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) { reg[1] |= _BV(UDRIE0); }
            panel.midiOut(number, now);
            credit += count;
        }
    }

    void serviceReport() {
        // Wait for a MIDI boundary so reports cannot interrupt DIN SysEx.
        if (!linkReady || !report || requestedLimit != noLimit || button || turbo.busy()
            || parser.sysex() || dinFilter.pending() || toHost.free() < 12) return;
        const uint8_t response[] = {0xf0, 0x00, 0x20, 0x3c, 0x04, 0x00, 0x02,
                                    turbo.speed(), 0xf7};
        MidiPacketizer speedReport;
        for (uint8_t byte : response) {
            speedReport.accept(byte, number << 4,
                              [this](const uint8_t *event) { packet(event); });
        }
        report = false;
    }

    void resetTransport() {
        closeDin |= outgoing.sysex();
        toDin.clear();
        toHost.clear();
        usbFilter.clear();
        dinFilter.clear();
        outgoing = MidiBoundary{};
        parser.reset();
        credit = 0;
        awaitStatus = true;
        awaitHostStatus = true;
        hostSysEx = false;
        inputLoss = false;
        report = true;
        sysexEnds = 0;
        // A queued USB command belongs to the session that submitted it.
        if (requestedLimitFromHost) requestedLimit = noLimit;
        requestedLimitFromHost = false;
    }

    Queue toDin, toHost;
    TurboLink turbo;
    FastUart &uart;
    volatile uint8_t *reg;
    uint8_t number;
    TurboTestSerial::Decoder testDecoder;
    ControlFilter dinFilter{MidiSide::Din}, usbFilter{MidiSide::Usb};
    MidiBoundary outgoing;
    MidiPacketizer parser;
    uint32_t lastHostAt = 0;
    uint32_t hostSysExStarted = 0;
    uint32_t lastDinAt = 0;
    uint16_t credit = 0;
    uint16_t sysexEnds = 0;
    uint8_t requestedLimit = noLimit;
    bool requestedLimitFromHost = false;
    bool button = false;
    bool report = false;
    bool pendingTx = false;
    bool closeDin = false;
    Panel::Fault fault = Panel::Fault::None;
    bool inputLoss = false;
    bool inputSeen = false;
    uint8_t blockedRxHead = 0;
    bool awaitStatus = true;
    bool awaitHostStatus = true;
    bool hostSysEx = false;
};

template <uint8_t Index> bool sendDin(const uint8_t *, uint8_t);
template <uint8_t Index> bool dinIdle();
template <uint8_t Index> void setDinSpeed(uint8_t);
template <uint8_t Index> void notifySpeed(uint8_t);
template <uint8_t Index> bool canTestDin();

Port ports[] = {
    {0, midiUart1, &UCSR1A, sendDin<0>, dinIdle<0>, setDinSpeed<0>, notifySpeed<0>, canTestDin<0>},
    {1, midiUart2, &UCSR2A, sendDin<1>, dinIdle<1>, setDinSpeed<1>, notifySpeed<1>, canTestDin<1>},
};

template <uint8_t Index> bool sendDin(const uint8_t *frame, uint8_t length) {
    return ports[Index].sendControl(frame, length);
}
template <uint8_t Index> bool dinIdle() { return ports[Index].idle(); }
template <uint8_t Index> void setDinSpeed(uint8_t speed) { ports[Index].setSpeed(speed); }
template <uint8_t Index> void notifySpeed(uint8_t) { ports[Index].report = true; }
template <uint8_t Index> bool canTestDin() { return ports[Index].canUseFastTest(); }

__attribute__((noinline)) void serviceUsbIn(uint32_t now) {
    uint8_t error;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        error = usbUart.error;
        usbUart.error = 0;
        if (error) usbUart.rxTail = usbUart.rxHead;
    }
    if (error) {
        receiver.phase = 0;
        resetPending = true;
        if (linkReady) {
            for (Port &port : ports) {
                if (port.fault == Panel::Fault::None) port.fault = Panel::Fault::Usb;
            }
        }
    }
    uint8_t byte;
    for (uint8_t budget = 0; budget < 40 && usbUart.read(byte); ++budget) {
        const uint8_t type = bridgeReceive(&receiver, byte);
        if (type == BRIDGE_HELLO || (type == BRIDGE_BAD_FRAME && linkReady)) {
            resetPending = true;
            if (type == BRIDGE_BAD_FRAME) {
                for (Port &port : ports) {
                    if (port.fault == Panel::Fault::None) port.fault = Panel::Fault::Usb;
                }
            }
        } else if (type == BRIDGE_ACK && !resetPending && !usbReady) {
            linkReady = true;
            returnBanks = BRIDGE_HOST_WINDOW;
            for (Port &port : ports) port.credit = initialCredit;
        } else if (type == BRIDGE_CREDIT && linkReady && !resetPending) {
            const uint8_t grant = receiver.control[4];
            if (receiver.control[0] != BRIDGE_DEVICE_BANK ||
                grant > BRIDGE_DEVICE_WINDOW - returnBatchCredits) {
                resetPending = true;
            } else {
                returnBatchCredits += grant;
                lastDeviceGrant = now;
                usbReady |= grant != 0;
            }
        } else if (type == BRIDGE_SYSEX && linkReady && !resetPending) {
            if (returnBanks >= BRIDGE_HOST_WINDOW) {
                resetPending = true;
                break;
            }
            ++returnBanks;
            for (uint8_t i = 0; i < receiver.length && !resetPending; i += 3) {
                const uint8_t *triplet = receiver.data + i;
                ports[triplet[0] >> 7].receiveSysEx(triplet, now);
            }
        } else if (type == BRIDGE_DATA && linkReady && !resetPending) {
            if (returnBanks >= BRIDGE_HOST_WINDOW) {
                resetPending = true;
                break;
            }
            ++returnBanks;
            for (uint8_t i = 0; i < receiver.length && !resetPending; i += 4) {
                const uint8_t *event = receiver.data + i;
                const uint8_t count = bridgeUsbMidiLength(event);
                if (count) ports[event[0] >> 4].receiveUsb(event, count, now);
            }
        }
    }
}

__attribute__((noinline)) void serviceUsbOut(uint32_t now) {
    static uint32_t nextConnect = 0;
    if (resetPending) {
        if (usbUart.writable() < 5) return;
        for (Port &port : ports) {
            if (port.fault == Panel::Fault::None && linkReady &&
                (port.outgoing.sysex() || port.parser.sysex() ||
                 !port.toDin.empty() || !port.toHost.empty())) {
                port.fault = Panel::Fault::Usb;
            }
            port.resetTransport();
        }
        returnBatchCredits = 0;
        returnBanks = 0;
        linkReady = false;
        usbReady = false;
        resetPending = false;
        nextConnect = now;
    }
    if (!usbReady) {
        if (static_cast<int32_t>(now - nextConnect) >= 0 && usbUart.writable() >= 5) {
            sendFrame(BRIDGE_RESET);
            nextConnect = now + 50;
        }
        return;
    }
    const uint16_t queued = ports[0].toHost.size() + ports[1].toHost.size();
    // DIN input has no per-byte backpressure. Pace USB OUT while either
    // raw input queue is filling and the host is reading.
    const bool drainInputs = queued >= BRIDGE_DEVICE_BANK && now - lastDeviceGrant < 12 &&
        (midiUart1.dinReadable() >= 4096 || midiUart2.dinReadable() >= 4096);
    const bool grantDue = drainInputs ? now - lastData >= 8 : now != lastData;
    const uint8_t batchGrant = drainInputs ? uint8_t(returnBanks != 0 && grantDue) : returnBanks;
    if ((batchGrant || (grantDue && (ports[0].credit || ports[1].credit))) && usbUart.writable() >= 15) {
        const uint8_t credit[] = {
            static_cast<uint8_t>(ports[0].credit),
            static_cast<uint8_t>(ports[0].credit >> 8),
            static_cast<uint8_t>(ports[1].credit),
            static_cast<uint8_t>(ports[1].credit >> 8),
            batchGrant,
        };
        returnBanks -= batchGrant;
        ports[0].credit = ports[1].credit = 0;
        lastData = now;
        sendFrame(BRIDGE_CREDIT, credit, sizeof(credit));
    }
    if (!returnBatchCredits || !queued || usbUart.writable() < 200) return;
    // While a stream is running, coalesce partial banks for up to one millisecond.
    static uint32_t lastBank = 0;
    if (queued < BRIDGE_DEVICE_BANK && now == lastBank) return;
    uint8_t data[BRIDGE_BANK];
    uint8_t length = 0;
    uint8_t sysexInBank = 0;
    bool compact = true;
    while (length < BRIDGE_DEVICE_BANK) {
        uint8_t port = nextPort;
        if (ports[port].toHost.empty()) port ^= 1;
        if (ports[port].toHost.empty()) break;
        const uint8_t cin = ports[port].toHost.front() & 15;
        const uint8_t cable = 1 << port;
        // CoreMIDI's legacy byte delivery can insert F7 when realtime follows
        // same-cable SysEx data in one USB transfer. Start a new bank there.
        if (cin == 15 && (sysexInBank & cable)) break;
        if (cin == 4) sysexInBank |= cable;
        else if (cin >= 5 && cin <= 7) sysexInBank &= ~cable;
        uint8_t *event = data + length;
        ports[port].toHost.popEvent(event);
        compact &= cin == 4 && (event[1] | event[2] | event[3]) < 128;
        length += 4;
        nextPort = port ^ 1;
    }
    --returnBatchCredits;
    lastBank = now;
    sendFrame(compact ? BRIDGE_SYSEX : BRIDGE_DATA, data, length);
}

} // namespace

ISR(TIMER1_COMPA_vect) { ++clockMs; }

int main() {
    MCUSR &= ~_BV(WDRF);
    wdt_disable();
    panel.begin();
    DDRL |= _BV(PL6);
    PORTL &= ~_BV(PL6);
    XMCRA |= _BV(SRE);
    midiUart1.extendedRx = reinterpret_cast<uint8_t *>(DIN_1_RX);
    midiUart2.extendedRx = reinterpret_cast<uint8_t *>(DIN_2_RX);
    configureUart(usbUart, &UCSR0A, 0);
    for (Port &port : ports) port.setSpeed(TurboLink::normalSpeed);
    TCCR1A = 0;
    TCCR1B = _BV(WGM12) | _BV(CS11) | _BV(CS10);
    OCR1A = F_CPU / 64 / 1000 - 1;
    TIMSK1 = _BV(OCIE1A);
    sei();
    panel.dinReady();

    uint32_t nextRecovery = 0;
    uint32_t nextPanel = 0;
    bool recoveryStarted = false;
    bool recoveryAcknowledged = false;
    for (;;) {
        const uint32_t now = nowMs();
        if (panel.recoveryHeld() || usbUpdate) {
            if (!recoveryStarted) {
                receiver.phase = 0;
                recoveryStarted = true;
            }
            uint8_t byte;
            while (usbUart.read(byte)) {
                if (bridgeReceive(&receiver, byte) == BRIDGE_CDC) recoveryAcknowledged = true;
            }
            if (!recoveryAcknowledged && static_cast<int32_t>(now - nextRecovery) >= 0 &&
                usbUart.writable() >= 5) {
                sendFrame(BRIDGE_CDC);
                nextRecovery = now + 100;
            }
            panel.showUsbUpdate();
            panel.flushPriorityChunk();
            continue;
        }
        const bool probingCdc = serviceMidiStartup(now);
        // Listen for a late MIDI reply during the quiet interval before
        // changing the UART from its ordinary 1 Mbps rate.
        if (!probingCdc || UBRR0L == 0) serviceUsbIn(now);
        for (Port &port : ports) port.serviceDin(now);
        for (Port &port : ports) port.readDin(now);
        if (!probingCdc) serviceUsbOut(now);

        if (now != nextPanel) {
            nextPanel = now;
            const uint8_t pressed = panel.takeTurboPresses(now);
            Panel::Status status[2];
            for (Port &port : ports) {
                if (pressed & (1 << port.number)) {
                    port.button = !port.button;
                    port.fault = Panel::Fault::None;
                }
                if (pressed & (4 << port.number)) port.cycleLimit(now);
                port.serviceReport();
                uint8_t limit = port.turbo.limit();
                if (port.requestedLimit != noLimit) {
                    limit = port.requestedLimit >= 7 ? 7 : port.requestedLimit >= 4 ? 4 :
                            port.requestedLimit >= 2 ? 2 : 1;
                }
                status[port.number] = {port.turbo.speed(), limit,
                                       port.turbo.busy(), port.turbo.failed(), port.fault,
                                       port.requestedLimit != noLimit};
            }
            panel.service(now, status, usbReady);
        }
        panel.flushPriorityChunk();
        if (linkReady && now - lastHeartbeat >= 100 &&
            usbUart.writable() >= 5) {
            // The marker exposes truncated frames during otherwise idle traffic.
            lastHeartbeat = now;
            sendFrame(BRIDGE_HEARTBEAT);
        }
    }
}
