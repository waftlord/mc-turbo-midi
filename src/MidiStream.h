#pragma once

#include <stdint.h>

class MidiBoundary {
public:
    bool complete() const { return !sysex_ && remaining_ == 0; }
    bool sysex() const { return sysex_; }
    uint8_t runningStatus() const { return running_; }

    void accept(uint8_t byte) {
        if (byte >= 0xF8) {
            return;
        }
        if (byte < 0x80) {
            if (!sysex_) {
                if (remaining_ == 0 && running_) {
                    remaining_ = dataLength(running_);
                }
                if (remaining_) {
                    --remaining_;
                }
            }
            return;
        }

        sysex_ = byte == 0xF0;
        running_ = byte < 0xF0 ? byte : 0;
        remaining_ = dataLength(byte);
    }

private:
    static uint8_t dataLength(uint8_t status) {
        if (status < 0xF0) {
            return (status & 0xE0) == 0xC0 ? 1 : 2;
        }
        switch (status) {
        case 0xF1:
        case 0xF3: return 1;
        case 0xF2: return 2;
        default: return 0;
        }
    }

    uint8_t running_ = 0;
    uint8_t remaining_ = 0;
    bool sysex_ = false;
};

enum class MidiSide : uint8_t { Usb, Din };

// Buffer short control frames while forwarding other MIDI messages.
class ControlFilter {
public:
    explicit ControlFilter(MidiSide side) : side_(side) {}

    template <typename Forward, typename Control>
    void accept(uint8_t byte, uint32_t now, Forward forward, Control control) {
        if (byte >= 0xF8) {
            forward(byte);
            return;
        }
        if (size_ && byte >= 0x80 && byte != 0xF7) {
            flush(forward);
        }
        if (byte == 0xF0) {
            bytes_[0] = byte;
            size_ = 1;
            lastByte_ = now;
            return;
        }
        if (!size_) {
            forward(byte);
            return;
        }
        if (size_ == sizeof(bytes_)) {
            flush(forward);
            forward(byte);
            return;
        }
        bytes_[size_++] = byte;
        lastByte_ = now;
        if (byte == 0xF7) {
            const bool mclMode = side_ == MidiSide::Din && bytes_[1] == 0x7D;
            if (mclMode && size_ == 8) {
                // This is also a private command on the USB chip.
                size_ = 0;
            } else if (!mclMode && size_ >= 8) {
                control(bytes_, size_);
                size_ = 0;
            } else {
                flush(forward);
            }
        } else if (!matchesPrefix(byte)) {
            flush(forward);
        }
    }

    template <typename Forward>
    void expire(uint32_t now, Forward forward) {
        if (size_ && now - lastByte_ >= 30) {
            flush(forward);
        }
    }

    bool pending() const { return size_ != 0; }
    void clear() { size_ = 0; }

private:
    template <typename Forward>
    void flush(Forward forward) {
        for (uint8_t i = 0; i < size_; ++i) {
            forward(bytes_[i]);
        }
        size_ = 0;
    }

    bool matchesPrefix(uint8_t byte) const {
        if (side_ == MidiSide::Din && bytes_[1] == 0x7D) {
            const uint8_t prefix[] = {0xF0, 0x7D, 0x4D, 0x43, 0x4C, 0x01};
            return size_ <= sizeof(prefix) ? byte == prefix[size_ - 1] : size_ == 7;
        }
        const uint8_t prefix[] = {0xF0, 0x00, 0x20, 0x3C,
                                  side_ == MidiSide::Usb ? uint8_t{0x04} : uint8_t{0x00},
                                  0x00};
        if (size_ <= sizeof(prefix)) {
            return byte == prefix[size_ - 1];
        }
        if (size_ == sizeof(prefix) + 1) {
            return side_ == MidiSide::Usb ? byte == 0x01 || byte == 0x03
                                          : (byte >= 0x10 && byte <= 0x17) || byte == 0x20;
        }
        return true;
    }

    MidiSide side_;
    uint8_t bytes_[16];
    uint8_t size_ = 0;
    uint32_t lastByte_ = 0;
};
