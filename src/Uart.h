#pragma once

#include <avr/io.h>
#include <util/atomic.h>
#include <stdint.h>

template <uint16_t Size>
class ByteQueue {
public:
    bool push(uint8_t byte) {
        const uint16_t next = (head_ + 1) & (Size - 1);
        if (next == tail_) {
            return false;
        }
        data_[head_] = byte;
        head_ = next;
        return true;
    }

    bool push(const uint8_t *data, uint8_t count) {
        if (count > free()) {
            return false;
        }
        for (uint8_t i = 0; i < count; ++i) {
            push(data[i]);
        }
        return true;
    }

    uint16_t free() const { return (tail_ - head_ - 1) & (Size - 1); }
    bool empty() const { return head_ == tail_; }
    uint8_t front() const { return data_[tail_]; }
    void pop() { tail_ = (tail_ + 1) & (Size - 1); }
    void clear() { tail_ = head_; }

private:
    static_assert((Size & (Size - 1)) == 0, "Queue size must be a power of two");
    uint8_t data_[Size];
    uint16_t head_ = 0;
    uint16_t tail_ = 0;
};

// USART0 and USART1 share the same register layout on the ATmega2560.
class Uart {
public:
    explicit Uart(volatile uint8_t *status) : status_(status) {}

    void setSpeed(uint8_t code) {
        // Standard 16 MHz AVR divisors for TurboMIDI speed codes 1, 2, 4 and 7.
        uint8_t divisor = 31;
        switch (code) {
        case 2: divisor = 15; break;
        case 4: divisor = 7; break;
        case 7: divisor = 3; break;
        }
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            status_[1] = 0;
            rx_.clear();
            error_ = ReceiveError::None;
            status_[5] = 0;
            status_[4] = divisor;
            status_[2] = _BV(UCSZ01) | _BV(UCSZ00);
            status_[0] = _BV(TXC0);
            status_[1] = _BV(RXEN0) | _BV(TXEN0) | _BV(RXCIE0);
            transmitting_ = false;
        }
    }

    bool read(uint8_t &byte) {
        bool available = false;
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            if (!rx_.empty()) {
                byte = rx_.front();
                rx_.pop();
                available = true;
            }
        }
        return available;
    }

    bool write(const uint8_t *bytes, uint8_t count) {
        bool queued = false;
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            queued = tx_.push(bytes, count);
            if (queued) {
                status_[1] |= _BV(UDRIE0);
            }
        }
        return queued;
    }

    bool txIdle() const {
        bool idle;
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            idle = tx_.empty() && (!transmitting_ || (status_[0] & _BV(TXC0)));
        }
        return idle;
    }

    bool takeError(bool reportInitialFraming = true) {
        bool error;
        ATOMIC_BLOCK(ATOMIC_RESTORESTATE) {
            error = error_ == ReceiveError::DataLoss ||
                    (reportInitialFraming && error_ == ReceiveError::InitialFraming);
            if (error) {
                rx_.clear();
            }
            error_ = ReceiveError::None;
        }
        return error;
    }

    void receiveInterrupt() {
        const uint8_t flags = status_[0];
        const uint8_t byte = status_[6];
        if (flags & (_BV(DOR0) | _BV(UPE0))) {
            error_ = ReceiveError::DataLoss;
            return;
        }
        if (flags & _BV(FE0)) {
            // Enabling RX in the middle of an incoming byte can produce a
            // framing error before this UART sees a MIDI message boundary.
            if (hasReceivedStatus_) {
                error_ = ReceiveError::DataLoss;
            } else if (error_ == ReceiveError::None) {
                error_ = ReceiveError::InitialFraming;
            }
            return;
        }
        if (!rx_.push(byte)) {
            error_ = ReceiveError::DataLoss;
        } else if (byte >= 0x80 && byte < 0xF7) {
            hasReceivedStatus_ = true;
        }
    }

    void transmitInterrupt() {
        if (tx_.empty()) {
            status_[1] &= ~_BV(UDRIE0);
            return;
        }
        status_[6] = tx_.front();
        status_[0] = _BV(TXC0);
        tx_.pop();
        transmitting_ = true;
    }

private:
    enum class ReceiveError : uint8_t { None, InitialFraming, DataLoss };

    volatile uint8_t *status_;
    ByteQueue<512> rx_;
    ByteQueue<128> tx_;
    volatile ReceiveError error_ = ReceiveError::None;
    bool hasReceivedStatus_ = false;
    volatile bool transmitting_ = false;
};
