#include <avr/interrupt.h>
#include <avr/wdt.h>
#include <util/atomic.h>
#include <stdint.h>

#include "MidiStream.h"
#include "Panel.h"
#include "TurboLink.h"
#include "Uart.h"

namespace {

constexpr uint8_t normalSpeed = TurboLink::normalSpeed;
constexpr uint8_t usbSerialMode[] = {0xF0, 0x7D, 0x4D, 0x43, 0x4C, 0x01, 0x03, 0xF7};
volatile uint32_t clockMs = 0;

Uart usb(&UCSR0A);
Uart din(&UCSR1A);
Panel panel;
ControlFilter dinFilter;
ByteQueue<1024> toDin;
ByteQueue<1024> toUsb;
MidiBoundary dinStream;
MidiBoundary usbStream;
MidiBoundary dinInputStream;
MidiBoundary usbInputStream;

bool buttonPending = false;
bool resetDinStatus = false;
bool resetUsbStatus = false;
bool usbRouteError = false;
bool dinRouteError = false;
bool droppedMidi = false;
bool usbSwitchFailed = false;
bool hostTrafficSeen = false;
bool awaitUsbStatus = true;
bool awaitDinStatus = true;
bool recoveryMode = false;
uint8_t usbSpeed = normalSpeed;
uint8_t requestedUsbSpeed = normalSpeed;
uint8_t switchingTo = normalSpeed;
bool switchingUsb = false;
uint8_t guardZeros = 0;
uint32_t usbSwitchStarted = 0;
uint32_t usbLastGuard = 0;
uint32_t usbNextAttempt = 0;

uint32_t nowMs() {
    uint32_t now;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        now = clockMs;
    }
    return now;
}

bool writeDinControl(const uint8_t *bytes, uint8_t length) {
    if (bytes[0] < 0xF8 && !dinStream.complete()) {
        return false;
    }
    if (din.write(bytes, length)) {
        if (bytes[0] < 0xF8) {
            resetDinStatus = true;
            panel.midiOut(nowMs());
        }
        return true;
    }
    return false;
}

bool dinIdle() { return din.txIdle(); }
void setDinSpeed(uint8_t code) { din.setSpeed(code); }
void publishSpeed(uint8_t code) {
    requestedUsbSpeed = code;
    usbNextAttempt = 0;
    usbSwitchFailed = false;
}

TurboLink turbo(writeDinControl, dinIdle, setDinSpeed, publishSpeed);

void readUsb(uint32_t now) {
    uint8_t byte;
    const auto forward = [](uint8_t value) {
        if (awaitUsbStatus && value < 0xF8) {
            if (value < 0x80 || value == 0xF7) return;
            awaitUsbStatus = false;
        }
        usbInputStream.accept(value);
        if (!toDin.push(value)) {
            usbRouteError = true;
            droppedMidi = true;
        }
    };
    for (uint8_t budget = 0; budget < 32 && (switchingUsb || toDin.free() >= 16) &&
         usb.read(byte); ++budget) {
        if (switchingUsb) {
            if (byte == 0) {
                if (guardZeros < 16) ++guardZeros;
                usbLastGuard = now;
            } else if (guardZeros < 12) {
                guardZeros = 0;
            }
        } else {
            hostTrafficSeen = true;
            if (byte != 0xFE || usbSpeed == normalSpeed) {
                forward(byte);
            }
        }
    }
}

void readDin(uint32_t now) {
    uint8_t byte;
    const auto forward = [](uint8_t value) {
        if (!turbo.pausePayload() || (value >= 0xF8 && value != 0xFE)) {
            if (awaitDinStatus && value < 0xF8) {
                if (value < 0x80 || value == 0xF7) return;
                awaitDinStatus = false;
            }
            dinInputStream.accept(value);
            if (!toUsb.push(value)) {
                dinRouteError = true;
                if (hostTrafficSeen) droppedMidi = true;
            }
        }
    };
    const auto control = [now](const uint8_t *frame, uint8_t length) {
        // 0x20 is reserved for the existing USB chip's private baud switch.
        if (frame[6] != 0x20) {
            const bool wasPaused = turbo.pausePayload();
            turbo.receive(frame, length, now);
            if (!wasPaused && turbo.pausePayload() && !dinInputStream.complete()) {
                dinRouteError = true;
            }
        }
    };
    for (uint8_t budget = 0; budget < 32 && din.read(byte); ++budget) {
        turbo.observeDinByte(byte, now);
        if (byte != 0xFE && (!turbo.busy() || byte != 0)) {
            panel.midiIn(now);
        }
        if (byte != 0xFE || (!turbo.busy() && turbo.speed() == normalSpeed)) {
            dinFilter.accept(byte, now, forward, control);
        }
    }
    dinFilter.expire(now, forward);
}

bool startUsbSwitch(uint32_t now) {
    if (!usb.txIdle() || !din.txIdle() || !usbStream.complete() || !dinStream.complete() ||
        !usbInputStream.complete()) {
        return false;
    }
    const uint8_t control[] = {0xF0, 0x00, 0x20, 0x3C, 0x00, 0x00, 0x20,
                               requestedUsbSpeed, 0xF7};
    if (!usb.write(control, sizeof(control))) {
        return false;
    }
    // The 16U2 sends a burst of old-rate NULs before changing its own UART.
    switchingTo = requestedUsbSpeed;
    switchingUsb = true;
    guardZeros = 0;
    usbSwitchStarted = now;
    usbLastGuard = now;
    return true;
}

void serviceUsbControl(uint32_t now) {
    if (switchingUsb) {
        if (guardZeros >= 12 && now - usbLastGuard >= 3 && usb.txIdle()) {
            usb.setSpeed(switchingTo);
            usbSpeed = switchingTo;
            hostTrafficSeen = true;
            switchingUsb = false;
            usbSwitchFailed = false;
            resetUsbStatus = true;
            // Close the prefix that the 16U2 can emit before it sees the
            // final byte of its private command.
            const uint8_t end = 0xF7;
            usb.write(&end, 1);
            usbInputStream = MidiBoundary{};
            awaitUsbStatus = true;
        } else if (now - usbSwitchStarted >= 25 && usb.txIdle()) {
            switchingUsb = false;
            usbNextAttempt = now + (hostTrafficSeen ? 250 : 1000);
            usbSwitchFailed = true;
            usb.takeError();
        }
        return;
    }

    if (requestedUsbSpeed != usbSpeed && !usbRouteError && !dinRouteError) {
        if (!usbNextAttempt || static_cast<int32_t>(now - usbNextAttempt) >= 0) {
            if (startUsbSwitch(now)) return;
        }
    }
}

bool transferByte(ByteQueue<1024> &queue, Uart &output, MidiBoundary &stream,
                  bool &resetStatus) {
    const uint8_t byte = queue.front();
    uint8_t packet[] = {byte, byte};
    uint8_t count = 1;
    if (resetStatus && byte < 0x80 && !stream.sysex() && stream.runningStatus()) {
        packet[0] = stream.runningStatus();
        count = 2;
    }
    if (!output.write(packet, count)) {
        return false;
    }
    queue.pop();
    stream.accept(byte);
    if (byte < 0xF8) {
        resetStatus = false;
    }
    return true;
}

void serviceDin(uint32_t now) {
    if (usbRouteError || switchingUsb) return;
    if (buttonPending && dinStream.complete() && din.txIdle()) {
        const bool wasPaused = turbo.pausePayload();
        buttonPending = false;
        turbo.toggle(now);
        if (!wasPaused && turbo.pausePayload() && !dinInputStream.complete()) {
            dinRouteError = true;
        }
    }

    for (uint8_t budget = 0; budget < 32 && !toDin.empty() &&
         (!turbo.pausePayload() || !dinStream.complete()); ++budget) {
        if (dinStream.complete() && buttonPending) {
            break;
        }
        if (!transferByte(toDin, din, dinStream, resetDinStatus)) {
            break;
        }
        panel.midiOut(now);
    }
}

void serviceUsbData(uint32_t now) {
    if (switchingUsb || dinRouteError) {
        return;
    }
    for (uint8_t budget = 0; budget < 32 && !toUsb.empty(); ++budget) {
        // Finish the current USB MIDI message before switching the UART rate.
        const bool switchDue = requestedUsbSpeed != usbSpeed && usbInputStream.complete() &&
                               (!usbNextAttempt || static_cast<int32_t>(now - usbNextAttempt) >= 0);
        if (usbStream.complete() && switchDue) {
            break;
        }
        if (!transferByte(toUsb, usb, usbStream, resetUsbStatus)) {
            break;
        }
    }
}

void handleErrors(uint32_t now) {
    const bool turboActive = turbo.busy() || turbo.speed() != normalSpeed;
    if (!switchingUsb && usb.takeError(turboActive || usbSpeed != normalSpeed)) {
        usbRouteError = true;
        droppedMidi = true;
    }
    if (din.takeError(turboActive)) {
        dinRouteError = true;
        if (!turbo.recovering()) droppedMidi = true;
        turbo.reportTransportError(now);
    }
    const uint8_t end = 0xF7;
    if (usbRouteError) {
        if (dinStream.sysex() && !din.write(&end, 1)) {
            return;
        }
        toDin.clear();
        dinStream = MidiBoundary{};
        usbInputStream = MidiBoundary{};
        awaitUsbStatus = true;
        usbRouteError = false;
    }
    if (dinRouteError && !switchingUsb) {
        if (usbStream.sysex() && !usb.write(&end, 1)) {
            return;
        }
        dinFilter.clear();
        toUsb.clear();
        usbStream = MidiBoundary{};
        dinInputStream = MidiBoundary{};
        awaitDinStatus = true;
        dinRouteError = false;
    }
}

void serviceRecovery(uint32_t now) {
    static uint32_t nextCommand = 0;
    // UART input on the 16U2 becomes live after USB enumeration.
    if (now >= nextCommand && usb.txIdle()) {
        usb.write(usbSerialMode, sizeof(usbSerialMode));
        nextCommand = now + 250;
    }
    panel.service(now, normalSpeed, false);
}

} // namespace

ISR(TIMER1_COMPA_vect) { ++clockMs; }
ISR(USART0_RX_vect) { usb.receiveInterrupt(); }
ISR(USART0_UDRE_vect) { usb.transmitInterrupt(); }
ISR(USART1_RX_vect) { din.receiveInterrupt(); }
ISR(USART1_UDRE_vect) { din.transmitInterrupt(); }

int main() {
    MCUSR &= ~_BV(WDRF);
    wdt_disable();
    panel.begin();
    recoveryMode = panel.recoveryHeld();
    usb.setSpeed(normalSpeed);
    din.setSpeed(normalSpeed);
    TCCR1A = 0;
    TCCR1B = _BV(WGM12) | _BV(CS11) | _BV(CS10);
    OCR1A = F_CPU / 64 / 1000 - 1;
    TIMSK1 = _BV(OCIE1A);
    sei();

    for (;;) {
        const uint32_t now = nowMs();
        if (recoveryMode) {
            serviceRecovery(now);
            continue;
        }
        if (panel.takeTurboPress(now)) {
            buttonPending = !buttonPending;
            droppedMidi = false;
            usbSwitchFailed = false;
        }
        const bool wasPaused = turbo.pausePayload();
        turbo.tick(now);
        if (!wasPaused && turbo.pausePayload() && !dinInputStream.complete()) {
            dinRouteError = true;
        }
        readUsb(now);
        readDin(now);
        handleErrors(now);
        serviceDin(now);
        serviceUsbControl(now);
        serviceUsbData(now);
        panel.service(now, turbo.speed(), turbo.busy(),
                      turbo.failed() || droppedMidi || (hostTrafficSeen && usbSwitchFailed));
    }
}
