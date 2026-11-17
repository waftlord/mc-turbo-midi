#pragma once
#include <stdint.h>

// Convert each independent DIN byte stream to USB-MIDI events.
class MidiPacketizer {
public:
    template <typename Emit>
    void accept(uint8_t byte, uint8_t cable, Emit emit) {
        if (byte >= 0xF8) {
            const uint8_t event[] = {static_cast<uint8_t>(cable | 15), byte, 0, 0};
            emit(event);
            return;
        }
        if (byte == 0xF7) {
            if (sysex_) {
                data_[count_++] = byte;
                send(cable | (4 + count_), emit);
            }
            reset();
            return;
        }
        if (byte >= 0x80) {
            if (sysex_) abortSysEx(cable, emit);
            count_ = 0;
            sysex_ = byte == 0xF0;
            status_ = byte == 0xF4 || byte == 0xF5 ? 0 : byte;
            if (byte == 0xF6) {
                data_[count_++] = byte;
                send(cable | 5, emit);
                reset();
                return;
            }
            if (status_) data_[count_++] = byte;
            return;
        }
        if (sysex_) {
            acceptSysExData(byte, cable, emit);
            return;
        }
        if (!status_) return;
        if (!count_) data_[count_++] = status_;
        data_[count_++] = byte;
        const uint8_t group = status_ >> 4;
        const uint8_t expected = group == 12 || group == 13 ||
                                 status_ == 0xF1 || status_ == 0xF3 ? 2 : 3;
        if (count_ == expected) {
            const uint8_t cin = group != 15 ? group : expected;
            send(cable | cin, emit);
            if (group == 15) status_ = 0;
        }
    }

    template <typename Emit>
    void acceptSysExData(uint8_t byte, uint8_t cable, Emit emit) {
        data_[count_++] = byte;
        if (count_ == 3) {
            send(cable | 4, emit);
            sysexSent_ = true;
        }
    }

    // Each destination has its own packetizer, so a run of plain SysEx
    // data can be emitted without revisiting the state machine per byte.
    template <typename Emit>
    uint8_t acceptSysExRun(const uint8_t *bytes, uint8_t count, uint8_t cable, Emit emit) {
        const uint8_t *cursor = bytes;
        const uint8_t *end = bytes + count;
        while (cursor != end && count_ && *cursor < 0x80) {
            acceptSysExData(*cursor++, cable, emit);
        }
        while (end - cursor >= 3 && (cursor[0] | cursor[1] | cursor[2]) < 0x80) {
            const uint8_t event[] = {static_cast<uint8_t>(cable | 4),
                                     cursor[0], cursor[1], cursor[2]};
            emit(event);
            sysexSent_ = true;
            cursor += 3;
        }
        while (cursor != end && *cursor < 0x80) {
            acceptSysExData(*cursor++, cable, emit);
        }
        return cursor - bytes;
    }

    template <typename Emit>
    void abortSysEx(uint8_t cable, Emit emit) {
        if (sysexSent_) {
            // The buffered one or two bytes preceded the fault. They fit
            // in the same USB event as the recovery terminator.
            data_[count_++] = 0xf7;
            send(cable | (4 + count_), emit);
        }
        reset();
    }

    void reset() { status_ = count_ = 0; sysex_ = sysexSent_ = false; }
    bool sysex() const { return sysex_; }

private:
    template <typename Emit>
    void send(uint8_t header, Emit emit) {
        const uint8_t event[] = {header, data_[0],
                                 count_ >= 2 ? data_[1] : uint8_t{0},
                                 count_ >= 3 ? data_[2] : uint8_t{0}};
        emit(event);
        count_ = 0;
    }

    uint8_t status_ = 0;
    uint8_t count_ = 0;
    uint8_t data_[3] = {};
    bool sysex_ = false;
    bool sysexSent_ = false;
};
