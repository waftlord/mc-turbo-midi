#include "Panel.h"

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <string.h>
#include <util/delay.h>

namespace {

constexpr uint8_t shiftData = _BV(PL0);
constexpr uint8_t shiftLoad = _BV(PL1);
constexpr uint8_t shiftClock = _BV(PL2);
constexpr uint8_t pageButton = _BV(5); // MCL Buttons.BUTTON2, active low.
constexpr uint8_t oledSelect = _BV(PL7);
constexpr uint8_t oledData = _BV(PL5);
constexpr uint8_t oledReset = _BV(PD7);
constexpr uint8_t turboLed = _BV(PE5);
constexpr uint8_t activityMs = 100;
constexpr uint8_t debounceMs = 16;

// Adapted from the MCL fork of Adafruit SSD1305. See THIRD_PARTY_NOTICES.md.
constexpr uint8_t oledInit[] PROGMEM = {
    0xAE,             // Display off.
    0x00, 0x10, 0x40, // Column zero and display start line zero.
    0x2E,             // Stop scroll.
    0x81, 0x32,       // Contrast.
    0x82, 0x80,       // Brightness.
    0xA0, 0xA4, 0xA6, // Segment mapping, RAM display, and normal pixels.
    0xA8, 0x3F,       // Multiplex ratio used by this module.
    0xAD, 0x8E,       // External display supply.
    0xC0, 0xD3, 0x40, // Scan direction and display offset.
    0xD5, 0xF0,       // Display clock divider.
    0xD8, 0x05,       // Area colour mode.
    0xD9, 0xF1,       // Precharge.
    0xDA, 0x12,       // COM pin configuration.
    0x91, 0x3F, 0x3F, 0x3F, 0x3F, // Lookup table.
    0x20, 0x02,       // Page addressing.
    0xAF,             // Display on.
};

struct Glyph {
    char symbol;
    uint8_t columns[5];
};

// Includes glyphs from the Adafruit GFX classic font.
constexpr Glyph glyphs[] PROGMEM = {
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'0', {0x3E, 0x51, 0x49, 0x45, 0x3E}},
    {'1', {0x00, 0x42, 0x7F, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4B, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7F, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3C, 0x4A, 0x49, 0x49, 0x30}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'A', {0x7E, 0x11, 0x11, 0x11, 0x7E}},
    {'B', {0x7F, 0x49, 0x49, 0x49, 0x36}},
    {'D', {0x7F, 0x41, 0x41, 0x22, 0x1C}},
    {'E', {0x7F, 0x49, 0x49, 0x49, 0x41}},
    {'I', {0x00, 0x41, 0x7F, 0x41, 0x00}},
    {'K', {0x7F, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7F, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7F, 0x02, 0x0C, 0x02, 0x7F}},
    {'N', {0x7F, 0x04, 0x08, 0x10, 0x7F}},
    {'O', {0x3E, 0x41, 0x41, 0x41, 0x3E}},
    {'P', {0x7F, 0x09, 0x09, 0x09, 0x06}},
    {'R', {0x7F, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7F, 0x01, 0x01}},
    {'U', {0x3F, 0x40, 0x40, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
};

bool pageDown() {
    PORTL &= ~shiftLoad;
    PORTL |= shiftLoad;

    // The first eight bits contain the panel buttons. The encoder signals
    // occupy the following eight bits and can be left unread.
    uint8_t buttons = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        buttons = static_cast<uint8_t>((buttons << 1) | (PINL & shiftData));
        PORTL &= ~shiftClock;
        PORTL |= shiftClock;
    }
    return !(buttons & pageButton);
}

void spiByte(uint8_t data) {
    SPDR = data;
    while (!(SPSR & _BV(SPIF))) {
    }
    static_cast<void>(SPDR);
}

const char *speedLabel(uint8_t code) {
    switch (code) {
    case 2: return "2X";
    case 3: return "3.3X";
    case 4: return "4X";
    case 5: return "5X";
    case 6: return "6.7X";
    case 7: return "8X";
    case 8: return "10X";
    case 9: return "13.3X";
    case 10: return "16X";
    case 11: return "20X";
    default: return "1X";
    }
}

} // namespace

void Panel::begin() {
    PORTL = (PORTL | shiftLoad | shiftClock | oledSelect | _BV(PL4))
            & ~(shiftData | oledData);
    DDRL = (DDRL | shiftLoad | shiftClock | oledSelect | oledData | _BV(PL4))
           & ~shiftData;
    PORTD |= _BV(PD4) | oledReset;
    DDRD |= _BV(PD4) | oledReset;
    PORTE &= ~turboLed;
    DDRE |= turboLed;

    const bool heldBeforeReset = pageDown();

    // Keep the SD card deselected. PB0 is also the AVR hardware SPI SS pin.
    PORTB |= _BV(PB0);
    DDRB |= _BV(PB0) | _BV(PB1) | _BV(PB2);
    SPCR = _BV(SPE) | _BV(MSTR); // Mode 0, MSB first, 4 MHz at 16 MHz CPU.
    SPSR = 0;

    _delay_ms(1);
    PORTD &= ~oledReset;
    _delay_ms(10);
    PORTD |= oledReset;
    _delay_ms(1);

    // Send each command byte with the same CS framing and settling delay as
    // MCL's initializer. Runtime updates only use the short SPI bursts below.
    for (uint8_t i = 0; i < sizeof(oledInit); ++i) {
        _delay_ms(1);
        PORTL &= ~oledSelect;
        spiByte(pgm_read_byte(&oledInit[i]));
        PORTL |= oledSelect;
    }

    rawDown_ = stableDown_ = pageDown();
    recoveryHeld_ = heldBeforeReset && stableDown_;
}

bool Panel::takeTurboPress(uint32_t now) {
    if (now - lastButtonPoll_ < 2) {
        return false;
    }
    lastButtonPoll_ = now;
    const bool down = pageDown();
    if (down != rawDown_) {
        rawDown_ = down;
        rawChangedAt_ = now;
    }
    if (rawDown_ == stableDown_ || now - rawChangedAt_ < debounceMs) {
        return false;
    }
    stableDown_ = rawDown_;
    return stableDown_;
}

void Panel::Activity::pulse(uint32_t now) {
    last = now;
    active = true;
}

bool Panel::Activity::visible(uint32_t now) {
    if (active && now - last >= activityMs) {
        active = false;
    }
    return active;
}

void Panel::midiIn(uint32_t now) { input_.pulse(now); }
void Panel::midiOut(uint32_t now) { output_.pulse(now); }

void Panel::service(uint32_t now, uint8_t speedCode, bool negotiating,
                    bool failed) {
    const bool input = input_.visible(now);
    const bool output = output_.visible(now);
    if (nextByte_ == frameBytes &&
        (speedCode != displayedSpeed_ || negotiating != displayedNegotiating_ ||
         failed != displayedFailed_ || input != displayedInput_ ||
         output != displayedOutput_)) {
        draw(speedCode, negotiating, failed, input, output);
        displayedSpeed_ = speedCode;
        displayedNegotiating_ = negotiating;
        displayedFailed_ = failed;
        displayedInput_ = input;
        displayedOutput_ = output;
        nextByte_ = 0;
    }
    if (nextByte_ < frameBytes) {
        flushChunk();
    }
}

void Panel::draw(uint8_t speedCode, bool negotiating, bool failed,
                 bool input, bool output) {
    memset(frame_, 0, sizeof(frame_));
    if (recoveryHeld_) {
        drawText(4, 8, "USB UPDATE", 2);
        return;
    }

    drawText(4, 5, "TURBO", 1);
    drawText(40, 1, speedLabel(speedCode), 2);
    if (negotiating) {
        drawText(102, 6, "LINK", 1);
    } else if (failed) {
        drawText(102, 6, "ERR", 1);
    }
    if (speedCode > 1 && !negotiating) {
        PORTE |= turboLed;
    } else {
        PORTE &= ~turboLed;
    }

    for (uint8_t x = 4; x < 124; ++x) {
        pixel(x, 18);
    }
    drawText(4, 23, "MIDI IN", 1);
    drawText(68, 23, "MIDI OUT", 1);
    drawIndicator(50, input);
    drawIndicator(118, output);
}

void Panel::drawText(uint8_t x, uint8_t y, const char *text, uint8_t scale) {
    for (; *text; ++text, x += 6 * scale) {
        for (const Glyph &glyph : glyphs) {
            if (pgm_read_byte(&glyph.symbol) != *text) {
                continue;
            }
            for (uint8_t column = 0; column < 5; ++column) {
                const uint8_t dots = pgm_read_byte(&glyph.columns[column]);
                for (uint8_t row = 0; row < 7; ++row) {
                    if (dots & _BV(row)) {
                        for (uint8_t dx = 0; dx < scale; ++dx) {
                            for (uint8_t dy = 0; dy < scale; ++dy) {
                                pixel(x + column * scale + dx, y + row * scale + dy);
                            }
                        }
                    }
                }
            }
            break;
        }
    }
}

void Panel::drawIndicator(uint8_t x, bool active) {
    for (uint8_t column = 0; column < 8; ++column) {
        for (uint8_t row = 0; row < 7; ++row) {
            if (active || column == 0 || column == 7 || row == 0 || row == 6) {
                pixel(x + column, 23 + row);
            }
        }
    }
}

void Panel::pixel(uint8_t x, uint8_t y) {
    if (x < width && y < height) {
        frame_[x + (y / 8) * width] |= _BV(y & 7);
    }
}

void Panel::flushChunk() {
    constexpr uint8_t chunkBytes = 16;
    const uint8_t column = nextByte_ % width;
    PORTL &= ~(oledSelect | oledData);
    spiByte(0xB0 | (nextByte_ / width));
    spiByte(column & 0x0F);
    spiByte(0x10 | (column >> 4));
    PORTL |= oledSelect;

    PORTL |= oledData;
    PORTL &= ~oledSelect;
    for (uint8_t i = 0; i < chunkBytes; ++i) {
        spiByte(frame_[nextByte_++]);
    }
    PORTL |= oledSelect;
}
