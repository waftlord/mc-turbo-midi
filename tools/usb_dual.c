#include <avr/io.h>
#include <avr/pgmspace.h>
#include <util/atomic.h>
#include <util/delay.h>

#include "../src/BridgeWire.h"

/* MIDI overlays the unused half of the stock RX ring and MIDI parser
   workspace. CDC restores the stock RX ring at its configured baud. */
#define REG8(address) (*(volatile uint8_t *)(address))
#define REG16(address) (*(volatile uint16_t *)(address))
#define usbState GPIOR0
#define stateConfigured 4
#define txStart ((volatile uint8_t *)0x158)
#define txWrite REG16(0x1d8)
#define txUsed REG8(0x1dc)
#define rxHead REG8(0x27f)
#define rxTail REG8(0x280)
#define rxError REG8(0x281)
#define rxBytes ((volatile uint8_t *)0x1dd)
#define stockMode ((void (*)(uint8_t))(0x5b2 / 2))
#define bridgeEpoch REG8(0x282)
#define seenEpoch REG8(0x283)
#define active REG8(0x284)
#define mainBanks REG8(0x292)
#define flags REG8(0x269)
#define flowPort0 REG16(0x289)
#define flowPort1 REG16(0x28b)
#define hostEvent ((uint8_t *)0x27a)
#define hostEventHeld REG8(0x27e)
#define receiver ((BridgeReceiver *)0x21d)
_Static_assert(0x21d + sizeof(BridgeReceiver) <= 0x269,
               "MIDI parser state overlaps the receive ring indexes");
// The byte parser continues to consume control traffic while an IN bank
// is busy. This flag also blocks a second data frame until the FIFO clears.
#define pendingBank ((uint8_t *)0x21d)
#define pendingLength REG8(0x291)
#define pendingCompact REG8(0x287)
#define endpointOut 1
#define endpointIn 2
#define ackPending 1
#define inCommitted 8
#define deviceGrant REG8(0x288)
#define waitingIn 4
#define discardPending 16

// These MIDI queue indexes are idle while the stock CDC ring is in use.
#define cdcStartupMatch rxHead
#define cdcStartupReady rxError
#define cdcFramePhase REG8(0x27a)
#define cdcFrameBytes REG16(0x27b)
#define cdcFrameCommand REG8(0x27d)
#define cdcFrameStatus hostEventHeld
#define cdcFrameChecksum rxTail
#define cdcLastFrameTime REG16(0x286)
#define cdcUpload REG8(0x285)
#define cdcProgrammed 1
#define cdcLeftBootloader 2
#define stkSignOn 0x01
#define stkEnterProgramming 0x10
#define stkLeaveProgramming 0x11
#define stkChipErase 0x12
#define stkProgramFlash 0x13

static uint8_t beginTx(uint8_t maximum) {
    return txUsed <= 128 - maximum;
}

static void append(uint16_t *head, uint8_t byte) {
    *(volatile uint8_t *)*head = byte;
    ++*head;
    if (*head == (uint16_t)txStart + 128) *head = (uint16_t)txStart;
}

static void stuff(uint16_t *head, uint8_t byte) {
    if (byte >= BRIDGE_ESCAPE) {
        append(head, BRIDGE_ESCAPE);
        byte -= BRIDGE_ESCAPE;
    }
    append(head, byte);
}

static void send(uint8_t type, const uint8_t *data, uint8_t length) {
    const uint16_t start = txWrite;
    uint16_t head = start;
    append(&head, BRIDGE_START);
    stuff(&head, type);
    uint8_t crc = bridgeCrc(0, type);
    if (type == BRIDGE_DATA) {
        stuff(&head, length);
        crc = bridgeCrc(crc, length);
    }
    for (uint8_t i = 0; i < length; ++i) {
        const uint8_t byte = data[i];
        stuff(&head, byte);
        crc = bridgeCrc(crc, byte);
    }
    stuff(&head, crc);
    const uint8_t queued = (head - start) & 127;
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        txWrite = head;
        txUsed += queued;
        UCSR1B |= _BV(UDRIE1);
    }
}

static uint8_t readByte(uint8_t *byte) {
    const uint8_t tail = rxTail;
    if (tail == rxHead) return 0;
    *byte = rxBytes[tail];
    rxTail = (tail + 1) & 63;
    return 1;
}

static uint8_t selectEndpoint(uint8_t ep) {
    if (usbState != stateConfigured || UDINT & (_BV(SUSPI) | _BV(EORSTI))) return 0;
    UENUM = ep;
    return (UECONX & (_BV(EPEN) | _BV(STALLRQ))) == _BV(EPEN);
}

static uint8_t inBankReady(void) {
    if (!selectEndpoint(endpointIn) || !(UEINTX & _BV(TXINI))) return 0;
    // Retire the previous bank even when the next frame arrived inside
    // serviceOut, before the top-level poll gets to check TXINI.
    if (flags & inCommitted) {
        flags &= ~inCommitted;
        ++deviceGrant;
    }
    return 1;
}

static void sendUsbBank(void) {
    if (!inBankReady()) return;
    // Keep each critical section below one UART byte time. USB control
    // interrupts preserve UENUM, while bus reset invalidates the bank.
    const uint8_t stride = pendingCompact ? 3 : 4;
    const uint8_t writtenEvents = UEBCLX / 4;
    for (uint8_t offset = writtenEvents * stride; offset < pendingLength; offset += stride) {
        const uint8_t *event = pendingBank + offset;
        uint8_t header = event[0];
        uint8_t first = event[1], second = event[2];
        uint8_t third;
        if (pendingCompact) {
            third = second;
            second = first;
            first = header & 127;
            header = ((header & 128) >> 3) | 4;
        } else {
            third = event[3];
        }
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            if (usbState != stateConfigured || UENUM != endpointIn ||
                (UECONX & (_BV(EPEN) | _BV(STALLRQ))) != _BV(EPEN) ||
                (UDINT & (_BV(SUSPI) | _BV(EORSTI))) || !(UEINTX & _BV(RWAL))) return;
            UEDATX = header;
            UEDATX = first;
            UEDATX = second;
            UEDATX = third;
        }
    }
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (usbState != stateConfigured || UENUM != endpointIn ||
            (UECONX & (_BV(EPEN) | _BV(STALLRQ))) != _BV(EPEN) ||
            (UDINT & (_BV(SUSPI) | _BV(EORSTI)))) return;
        UEINTX = 0xfa;
        UEINTX = 0x7b;
    }
    flags &= ~waitingIn;
    flags |= inCommitted;
}

/* CRC of DATA and length followed by length zero bytes. XOR with the
   payload CRC to prepend the actual length in constant time. */
static const uint8_t prefixCrc[] PROGMEM = {
    0x15, 0xa6, 0xa9, 0x07, 0xca, 0x2f, 0x58, 0xf0, 0xdd,
    0x01, 0x9e, 0xed, 0x34, 0x95, 0x0f, 0x2c, 0x03
};

static const uint8_t compactPrefixCrc[] PROGMEM = {
    0xa8, 0x41, 0x72, 0x16, 0x42, 0x58, 0x86, 0x39, 0x88,
    0xbc, 0xc2, 0x0c, 0x64, 0x59, 0x00, 0x7a, 0x71
};

static uint8_t serviceRx(uint8_t budget, uint8_t allowTransfers);

static void discardMidiBanks(void) {
    // UERST discards the FIFOs while preserving the USB data toggles.
    UERST = _BV(endpointIn) | _BV(endpointOut);
    UERST = 0;
}

/* The USB setup ISR restores UENUM. USB reset can disable a bank in
   hardware; recheck it before ownership changes. */
static void serviceOut(void) {
    if (!mainBanks || !selectEndpoint(endpointOut) || !(UEINTX & _BV(RXOUTI))) return;
    uint8_t length = 0;
    uint8_t held = hostEventHeld;
    // Send one USB bank as the TX ring drains. Admission below reserves
    // five worst-case wire bytes per event, plus room for the CRC.
    const uint8_t available = 128 - txUsed;
    if (available < 36) return;
    const uint8_t limit = (available - 6) / 5;
    uint8_t events = 0;
    const uint16_t txFirst = txWrite;
    uint16_t cursor = txFirst;
    append(&cursor, BRIDGE_START);
    const uint16_t typeByte = cursor;
    append(&cursor, 0);
    const uint16_t lengthByte = cursor;
    append(&cursor, 0);
    // Each event consists of an unescaped header, a possible escaped MIDI
    // status, and two 7-bit bytes. Padding is canonicalized.
    uint8_t crc = 0;
    uint8_t compact = 0;
    uint8_t work = 0;
    while (length < BRIDGE_HOST_BANK && events < limit) {
        // The 64-byte UART ring is shared with a full-speed return stream.
        // Drain it between short bursts of USB OUT FIFO work.
        if (++work == 5) {
            work = 1;
            if (!serviceRx(20, 1)) return;
        }
        if (!held) {
            ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
                if (!selectEndpoint(endpointOut) || UEBCLX < 4 || !(UEINTX & _BV(RWAL))) break;
                hostEvent[0] = UEDATX;
                hostEvent[1] = UEDATX;
                hostEvent[2] = UEDATX;
                hostEvent[3] = UEDATX;
                held = 1;
            }
            if (!held) break;
        }
        const uint8_t header = hostEvent[0];
        const uint8_t cable = header >> 4;
        const uint8_t count = bridgeUsbMidiLength(hostEvent);
        if (count) {
            const uint8_t continuation = (header & 15) == 4 &&
                (hostEvent[1] | hostEvent[2] | hostEvent[3]) < 0x80;
            if (events && compact != continuation) break;
            compact = continuation;
            volatile uint16_t *credit = cable ? &flowPort1 : &flowPort0;
            if (*credit < count) break;
            *credit -= count;
            ++events;
            if (compact) {
                const uint8_t first = hostEvent[1] | (cable << 7);
                stuff(&cursor, first);
                append(&cursor, hostEvent[2]);
                append(&cursor, hostEvent[3]);
                crc = bridgeCrc(crc, first);
                crc = bridgeCrc(crc, hostEvent[2]);
                crc = bridgeCrc(crc, hostEvent[3]);
                length += 4;
                held = 0;
                continue;
            }
            hostEvent[2] = count >= 2 ? hostEvent[2] : 0;
            hostEvent[3] = count >= 3 ? hostEvent[3] : 0;
            append(&cursor, header);
            stuff(&cursor, hostEvent[1]);
            append(&cursor, hostEvent[2]);
            append(&cursor, hostEvent[3]);
            crc = bridgeCrc(crc, header);
            crc = bridgeCrc(crc, hostEvent[1]);
            crc = bridgeCrc(crc, hostEvent[2]);
            crc = bridgeCrc(crc, hostEvent[3]);
            length += 4;
        }
        held = 0;
    }
    hostEventHeld = held;
    if (length) {
        // CRC linearity lets us prepend the now-known length without a
        // second pass through the frame contents.
        const uint8_t *prefixTable = compact ? compactPrefixCrc : prefixCrc;
        const uint8_t prefix = pgm_read_byte(prefixTable + events);
        *(volatile uint8_t *)typeByte = compact ? BRIDGE_SYSEX : BRIDGE_DATA;
        *(volatile uint8_t *)lengthByte = length;
        stuff(&cursor, crc ^ prefix);
        const uint8_t queued = (cursor - txFirst) & 127;
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            txWrite = cursor;
            txUsed += queued;
            UCSR1B |= _BV(UDRIE1);
        }
        --mainBanks;
    }
    ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
        if (!held && selectEndpoint(endpointOut) && UEBCLX < 4 &&
            (!UEBCLX || (UEINTX & _BV(RWAL)))) {
            // Consume any incomplete event before releasing the OUT bank.
            while (UEBCLX) (void)UEDATX;
            UEINTX = 0xfb;
            UEINTX = 0x7f;
        }
    }
}

void dualUsbInit(void) __attribute__((section(".text.dual_usb_init")));
void dualUsbInit(void) {
    UCSR1B = 0;
    UBRR1 = 0;
    UCSR1A = 0;
    UCSR1B = _BV(RXCIE1) | _BV(RXEN1) | _BV(TXEN1);
    UCSR1C = _BV(UCSZ11) | _BV(UCSZ10);
    receiver->phase = 0;
    active = flags = seenEpoch = bridgeEpoch = 0;
    rxHead = rxTail = rxError = deviceGrant = 0;
    hostEventHeld = 0;
    flowPort0 = flowPort1 = mainBanks = 0;
}

static void loseLink(void) {
    if (usbState == stateConfigured && !(UDINT & (_BV(SUSPI) | _BV(EORSTI)))) {
        discardMidiBanks();
        flags = 0;
    } else {
        flags = discardPending;
    }
    rxError = 0;
    rxTail = rxHead;
    seenEpoch = bridgeEpoch;
    active = deviceGrant = 0;
    flowPort0 = flowPort1 = mainBanks = 0;
    hostEventHeld = 0;
    receiver->phase = 0;
}

static void observeBootloaderReply(uint8_t byte) {
    // STK500v2 replies use a start byte, sequence, big-endian payload
    // length, token, payload, and XOR checksum.
    cdcFrameChecksum ^= byte;
    switch (cdcFramePhase) {
    case 0:
        if (byte != 0x1b) return;
        cdcFrameChecksum = byte;
        break;
    case 1:
        break;
    case 2:
        cdcFrameBytes = (uint16_t)byte << 8;
        break;
    case 3:
        cdcFrameBytes |= byte;
        break;
    case 4:
        if (byte != 0x0e || !cdcFrameBytes || cdcFrameBytes > 512) {
            cdcFramePhase = byte == 0x1b;
            cdcFrameChecksum = byte;
            return;
        }
        cdcFrameCommand = 0;
        cdcFrameStatus = 0xff;
        break;
    case 5:
        if ((byte == stkSignOn && cdcFrameBytes >= 2) ||
            (cdcFrameBytes == 2 && byte >= stkEnterProgramming && byte <= stkProgramFlash)) {
            cdcFrameCommand = byte;
        }
        if (--cdcFrameBytes) {
            cdcFramePhase = 6;
            return;
        }
        cdcFramePhase = 7;
        return;
    case 6:
        if (cdcFrameCommand) cdcFrameStatus = byte;
        if (!--cdcFrameBytes) cdcFramePhase = 7;
        return;
    case 7:
        if (!cdcFrameChecksum) {
            // A read or status reply can still be in flight after LEAVE.
            // Only a new programming operation supersedes a good exit.
            if (cdcFrameCommand == stkSignOn || cdcFrameCommand == stkEnterProgramming ||
                cdcFrameCommand == stkChipErase) {
                cdcUpload = 0;
            } else if (cdcFrameCommand == stkProgramFlash) {
                cdcUpload = cdcFrameStatus == 0 ? cdcProgrammed : 0;
            } else if (cdcFrameCommand == stkLeaveProgramming) {
                if (cdcFrameStatus == 0 && (cdcUpload & cdcProgrammed)) {
                    cdcUpload |= cdcLeftBootloader;
                } else {
                    cdcUpload &= ~cdcLeftBootloader;
                }
            }
        }
        cdcFramePhase = 0;
        return;
    }
    ++cdcFramePhase;
}

uint8_t dualCdcTrackStartup(uint8_t status, uint8_t byte) {
    static const uint8_t token[] PROGMEM = {BRIDGE_MIDI_STARTUP_BYTES};
    const uint16_t now = UDFNUM;
    if (((now - cdcLastFrameTime) & 0x07ff) > BRIDGE_CDC_FRAME_GAP_MS) {
        cdcFramePhase = cdcStartupMatch = 0;
    }
    cdcLastFrameTime = now;
    if (status & (_BV(FE1) | _BV(DOR1) | _BV(UPE1))) {
        cdcStartupMatch = cdcFramePhase = 0;
    } else if (cdcFramePhase || byte == 0x1b) {
        // A flash read-back can contain the startup token itself.
        cdcStartupMatch = 0;
        observeBootloaderReply(byte);
    } else if (!(cdcUpload & cdcLeftBootloader)) {
        return byte;
    } else if (byte != pgm_read_byte(token + cdcStartupMatch)) {
        cdcStartupMatch = byte == pgm_read_byte(token) ? 1 : 0;
    } else if (++cdcStartupMatch == sizeof(token)) {
        cdcStartupMatch = 0;
        cdcStartupReady = 1;
    }
    return byte;
}

static void detachForModeChange(void) {
    UDCON |= _BV(DETACH);
    // The stock mode switch rebuilds the controller and descriptors
    // immediately. Make the disconnect visible to the host first.
    _delay_ms(250);
}

void dualCdcResumeMidi(void) {
    if (cdcStartupReady) {
        UCSR1B = 0;
        detachForModeChange();
        stockMode(2);
    }
}

static uint8_t serviceRx(uint8_t budget, uint8_t allowTransfers) {
    if (rxError || seenEpoch != bridgeEpoch) return 0;
    uint8_t byte;
    while (budget-- && readByte(&byte)) {
        if ((flags & waitingIn) && receiver->phase == 1 &&
            (byte == BRIDGE_DATA || byte == BRIDGE_SYSEX)) {
            // The parser and pending USB frame share storage.
            loseLink();
            return 0;
        }
        const uint8_t type = bridgeReceive(receiver, byte);
        if (type == BRIDGE_BAD_FRAME) {
            if (active) {
                loseLink();
                return 0;
            }
            continue;
        }
        if (type == BRIDGE_CDC) {
            // Let the main chip stop retrying before the USART changes to
            // the slower CDC baud. The queue drains during USB detach.
            if (beginTx(5)) send(BRIDGE_CDC, 0, 0);
            detachForModeChange();
            UCSR1B = 0;
            cdcStartupMatch = cdcStartupReady = cdcFramePhase = cdcUpload = 0;
            stockMode(3);
            return 0;
        }
        if (!allowTransfers) continue;
        if (type == BRIDGE_RESET) {
            if (active) {
                if (UDINT & (_BV(SUSPI) | _BV(EORSTI))) {
                    loseLink();
                    return 0;
                }
                discardMidiBanks();
            }
            flowPort0 = flowPort1 = mainBanks = 0;
            flags = ackPending;
            active = 0;
            deviceGrant = 0;
            hostEventHeld = 0;
            return 0;
        } else if (type == BRIDGE_CREDIT && active) {
            const uint16_t port0 = receiver->control[0] | ((uint16_t)receiver->control[1] << 8);
            const uint16_t port1 = receiver->control[2] | ((uint16_t)receiver->control[3] << 8);
            if (port0 > BRIDGE_DIN_CREDIT - flowPort0 || port1 > BRIDGE_DIN_CREDIT - flowPort1 ||
                receiver->control[4] > BRIDGE_HOST_WINDOW - mainBanks) {
                loseLink();
                return 0;
            }
            flowPort0 += port0;
            flowPort1 += port1;
            mainBanks += receiver->control[4];
        } else if (type == BRIDGE_HEARTBEAT && !active && !(flags & ackPending)) {
            // A running main application can detect a reset USB controller.
            // Stay quiet when the 2560 is still in its serial bootloader.
            if (beginTx(5)) send(BRIDGE_HELLO, 0, 0);
        } else if ((type == BRIDGE_DATA || type == BRIDGE_SYSEX) && active) {
            pendingLength = receiver->length;
            pendingCompact = type == BRIDGE_SYSEX;
            flags |= waitingIn;
            sendUsbBank();
        }
    }
    return 1;
}

static void sendDeviceGrant(void) {
    if (!deviceGrant || !beginTx(15)) return;
    const uint8_t credit[] = {BRIDGE_DEVICE_BANK, 0, 0, 0, deviceGrant};
    send(BRIDGE_CREDIT, credit, sizeof(credit));
    deviceGrant = 0;
}

void dualUsbPoll(void) __attribute__((section(".text.dual_usb_poll")));
void dualUsbPoll(void) {
    // The stock firmware services USB setup on the polling path in this
    // mode. If a setup interrupt is enabled, let the stock dispatcher
    // run it with its shallow stack before entering the transport.
    UENUM = 0;
    if (UEINTX & _BV(RXSTPI)) return;
    if (usbState != stateConfigured || UDINT & (_BV(SUSPI) | _BV(EORSTI))) {
        // Once polling has started, honor a recovery request during a
        // host configuration change without touching the MIDI endpoints.
        if (!active && !flags) serviceRx(48, 0);
        return;
    }
    if (seenEpoch != bridgeEpoch || rxError) loseLink();
    if (flags & discardPending) {
        discardMidiBanks();
        flags = 0;
    }

    // One frame can be in the hardware bank and another can be pending
    // in SRAM. Return its credit once the host consumes the hardware bank.
    if (flags & waitingIn) sendUsbBank();
    else if (flags & inCommitted) inBankReady();
    serviceRx(48, 1);
    if (REG8(0x11b) != 2) return;
    if ((flags & inCommitted) && !(flags & waitingIn)) inBankReady();
    if (flags & ackPending) {
        if (!beginTx(5)) return;
        send(BRIDGE_ACK, 0, 0);
        flags = 0;
        deviceGrant = BRIDGE_DEVICE_WINDOW;
        active = 1;
    }
    if (!active) return;
    sendDeviceGrant();
    serviceOut();
    if (REG8(0x11b) != 2) return;
    if ((flags & inCommitted) && !(flags & waitingIn)) inBankReady();
    sendDeviceGrant();
}
