#pragma once

#include <stdint.h>

namespace TurboTestSerial {

constexpr uint32_t sampleRate = 800000;
constexpr uint32_t baud = 312500;
// Relative to the first low sample, with the edge in the preceding period.
constexpr uint8_t centers[] = {3, 6, 8, 11, 14, 16, 19, 21, 24};

// 8N1 at 312500 baud, packed LSB first for the USART in master SPI mode.
// The ratio is 64 samples per 25 bits. At each edge round to the nearest
// sample and carry the remainder across the entire message.
constexpr uint8_t packedSize(uint8_t length) {
    return (uint16_t(length) * 16 + 4) / 5;
}

template<typename Emit>
void encode(const uint8_t *data, uint8_t length, Emit emit) {
    uint8_t fraction = 12;
    uint8_t packed = 0;
    uint8_t mask = 1;
    while (length--) {
        uint16_t frame = (uint16_t(*data++) << 1) | 0x200;
        for (uint8_t bit = 0; bit < 10; ++bit, frame >>= 1) {
            uint8_t count = 2;
            fraction += 14;
            if (fraction >= 25) {
                fraction -= 25;
                ++count;
            }
            do {
                if (frame & 1) packed |= mask;
                mask <<= 1;
                if (!mask) {
                    emit(packed);
                    packed = 0;
                    mask = 1;
                }
            } while (--count);
        }
    }
    if (mask != 1) emit(packed | -mask);
}

class Decoder {
public:
    enum class Result : uint8_t { Pending, Byte, BadStop };

    Result accept(uint8_t packed, uint8_t &value) {
        Result result = Result::Pending;
        if (position_ == idle && packed == 0xff) {
            high_ = true;
            return result;
        }
        for (uint8_t bit = 0; bit < 8; ++bit, packed >>= 1) {
            const bool high = packed & 1;
            if (position_ == idle) {
                if (high_ && !high) {
                    position_ = 0;
                    bit_ = 0;
                    data_ = 0;
                }
            } else if (++position_ == centers[bit_]) {
                if (bit_ == 8) {
                    value = data_;
                    result = high ? Result::Byte : Result::BadStop;
                    position_ = idle;
                } else {
                    data_ = (data_ >> 1) | (high ? 0x80 : 0);
                    ++bit_;
                }
            }
            high_ = high;
        }
        return result;
    }

private:
    static constexpr uint8_t idle = 0xff;
    uint8_t position_ = idle;
    uint8_t bit_ = 0;
    uint8_t data_ = 0;
    bool high_ = true;
};

} // namespace TurboTestSerial
