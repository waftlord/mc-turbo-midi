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

static inline uint8_t turboDinFrameLength(uint8_t command) {
    static constexpr uint8_t lengths[] = {8, 12, 10, 8, 16, 16, 8, 8};
    return command >= 0x10 && command <= 0x17 ? lengths[command - 0x10] : 0;
}

// Buffer short control frames while forwarding other MIDI messages.
class ControlFilter {
public:
    static constexpr uint8_t frameCapacity = 16;
    static constexpr uint8_t realtimeCapacity = 4;
    static constexpr uint8_t maxBuffered = frameCapacity + realtimeCapacity;

    explicit ControlFilter(MidiSide side) : side_(side) {}

    template <typename Forward, typename Control>
    void accept(uint8_t byte, uint32_t now, Forward forward, Control control) {
        if (byte >= 0xF8) {
            if (size_ && realtimeCount_ < realtimeCapacity) {
                // Remember where this event fell in the undecided prefix.
                // size_ is 1..15 while waiting for a possible control EOX.
                if (!realtimeCount_) firstRealtimeAt_ = now;
                realtime_[realtimeCount_++] = (size_ << 3) | (byte & 7);
            } else {
                releaseRealtime(forward);
                forward(byte);
            }
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
            if (size_ >= 8 && size_ == controlLength()) {
                releaseRealtime(forward);
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
        } else if (realtimeCount_ && now - firstRealtimeAt_ >= 2) {
            // A stalled control prefix must not hold transport clock until
            // the 30 ms prefix timeout. Keep the prefix eligible for control.
            releaseRealtime(forward);
        }
    }

    bool pending() const { return size_ != 0; }
    void clear() { size_ = realtimeCount_ = 0; }

private:
    uint8_t controlLength() const {
        const uint8_t command = bytes_[6];
        if (side_ == MidiSide::Din) return turboDinFrameLength(command);
        if (bytes_[1] == 0x7D) return command == 1 || command == 3 ? 8 : 0;
        if (command == 1) return 8;
        if (command != 3) return 0;
        if (size_ >= 8 && bytes_[7] > 11) return 0;
        return 9;
    }

    template <typename Forward>
    void releaseRealtime(Forward forward) {
        for (uint8_t i = 0; i < realtimeCount_; ++i) {
            forward(0xf8 | (realtime_[i] & 7));
        }
        realtimeCount_ = 0;
    }

    template <typename Forward>
    void flush(Forward forward) {
        uint8_t index = 0;
        for (uint8_t i = 0; i < size_; ++i) {
            forward(bytes_[i]);
            while (index < realtimeCount_ && (realtime_[index] >> 3) == i + 1) {
                forward(0xf8 | (realtime_[index++] & 7));
            }
        }
        clear();
    }

    bool matchesPrefix(uint8_t byte) const {
        if (size_ >= 2 && bytes_[1] == 0x7D) {
            if (side_ != MidiSide::Usb) return false;
            const uint8_t prefix[] = {0xF0, 0x7D, 0x4D, 0x43, 0x4C, 0x01};
            return size_ <= sizeof(prefix) ? byte == prefix[size_ - 1] : size_ < controlLength();
        }
        const uint8_t prefix[] = {0xF0, 0x00, 0x20, 0x3C,
                                  side_ == MidiSide::Usb ? uint8_t{0x04} : uint8_t{0x00},
                                  0x00};
        if (size_ <= sizeof(prefix)) {
            return byte == prefix[size_ - 1];
        }
        return size_ < controlLength();
    }

    MidiSide side_;
    uint8_t bytes_[frameCapacity];
    uint8_t realtime_[realtimeCapacity];
    uint8_t realtimeCount_ = 0;
    uint32_t firstRealtimeAt_ = 0;
    uint8_t size_ = 0;
    uint32_t lastByte_ = 0;
};
