#pragma once

#include <avr/io.h>
#include <string.h>
#include "FastUartLayout.h"
#include <stdint.h>

/* Single-producer, single-consumer queues. Publishing an 8-bit index makes
   the completed bytes visible to the opposite side of the queue. */
struct FastUart {
    volatile uint8_t rxHead, rxTail, txHead, txTail, error;
    uint8_t rx[256], tx[256];
    volatile uint8_t rxEpoch, tailEpoch;
    uint8_t *extendedRx;
    volatile uint8_t testIdle, testTxDrain;

    bool read(uint8_t &byte) {
        if (rxTail == rxHead) return false;
        byte = rx[rxTail++];
        return true;
    }

    uint16_t dinReadable() const {
        uint8_t head, epoch;
        const uint8_t saved = SREG;
        asm volatile("cli" ::: "memory");
        head = rxHead;
        epoch = rxEpoch;
        SREG = saved;
        const uint16_t position = (uint16_t(tailEpoch) << 8) | rxTail;
        const uint16_t end = (uint16_t(epoch) << 8) | head;
        return end >= position ? end - position : DIN_RX_SIZE - position + end;
    }

    uint8_t readDin(uint8_t *bytes, uint8_t budget) {
        const uint16_t used = dinReadable();
        const uint8_t count = used < budget ? used : budget;
        if (!count) return 0;
        const uint16_t position = (uint16_t(tailEpoch) << 8) | rxTail;
        const uint16_t remaining = DIN_RX_SIZE - position;
        if (remaining >= count) memcpy(bytes, extendedRx + position, count);
        else {
            memcpy(bytes, extendedRx + position, remaining);
            memcpy(bytes + remaining, extendedRx, count - remaining);
        }
        const uint16_t next = remaining > count ? position + count : count - remaining;
        const uint8_t restored = SREG;
        asm volatile("cli" ::: "memory");
        rxTail = next;
        tailEpoch = next >> 8;
        SREG = restored;
        return count;
    }

    uint8_t writable() const { return static_cast<uint8_t>(txTail - txHead - 1); }
};

extern "C" FastUart usbUart, midiUart1, midiUart2;

inline void writeUart(FastUart &queue, volatile uint8_t &control,
                      const uint8_t *bytes, uint8_t length) {
    uint8_t head = queue.txHead;
    for (uint8_t i = 0; i < length; ++i) queue.tx[head++] = bytes[i];
    queue.txHead = head;
    // The transmit ISR may disable UDRE just before this atomic bit set.
    const uint8_t saved = SREG;
    asm volatile("cli" ::: "memory");
    control |= _BV(UDRIE0);
    SREG = saved;
}
