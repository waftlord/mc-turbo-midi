#include "Panel.h"

#include <avr/io.h>
#include <avr/pgmspace.h>
#include <string.h>
#include <util/delay.h>

namespace {

constexpr uint8_t shiftData = _BV(PL0);
constexpr uint8_t shiftLoad = _BV(PL1);
constexpr uint8_t shiftClock = _BV(PL2);
constexpr uint8_t pageButton = _BV(5); // MCL Buttons.BUTTON2
constexpr uint8_t shiftButton = _BV(6); // MCL Buttons.BUTTON3
constexpr uint8_t saveButton = _BV(4);
constexpr uint8_t loadButton = _BV(7);
constexpr uint8_t oledSelect = _BV(PL7);
constexpr uint8_t oledData = _BV(PL5);
constexpr uint8_t oledReset = _BV(PD7);
constexpr uint8_t turboLed[] = {_BV(PE5), _BV(PE4)};
constexpr uint8_t activityMs = 100;
constexpr uint8_t debounceMs = 16;
constexpr uint8_t splashMs = 200;
constexpr uint8_t progressX = 28;
constexpr uint8_t progressWidth = 72;
constexpr uint8_t progressInterior = progressWidth - 2;
constexpr uint8_t progressPage = 3;
constexpr uint16_t portStatusMask = 0x7030; // Link and fault bits.

// Map four source pixels to eight pixels for the large OLED text.
constexpr uint8_t doubledNibble[] PROGMEM = {
    0x00, 0x03, 0x0c, 0x0f, 0x30, 0x33, 0x3c, 0x3f,
    0xc0, 0xc3, 0xcc, 0xcf, 0xf0, 0xf3, 0xfc, 0xff,
};

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
    {'F', {0x7F, 0x09, 0x09, 0x09, 0x01}},
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
    {'W', {0x3F, 0x40, 0x38, 0x40, 0x3F}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
};

uint8_t shiftReadByte() {
    uint8_t value = 0;
    for (uint8_t i = 0; i < 8; ++i) {
        value = static_cast<uint8_t>((value << 1) | (PINL & shiftData));
        PORTL &= ~shiftClock;
        PORTL |= shiftClock;
    }
    return value;
}

void latchPanel() {
    PORTL &= ~shiftLoad;
    PORTL |= shiftLoad;
}

uint8_t pressedButtons() {
    latchPanel();
    const uint8_t buttons = shiftReadByte();
    return static_cast<uint8_t>((!(buttons & pageButton) ? 1 : 0) |
                                (!(buttons & shiftButton) ? 2 : 0) |
                                (!(buttons & saveButton) ? 4 : 0) |
                                (!(buttons & loadButton) ? 8 : 0));
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
    PORTE &= ~(turboLed[0] | turboLed[1]);
    DDRE |= turboLed[0] | turboLed[1];

    const bool heldBeforeReset = pressedButtons() & 1;

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

    rawDown_ = stableDown_ = pressedButtons();
#ifdef MCDT_PANEL
    latchPanel();
    encoderButtons_ = rawEncoderButtons_ = static_cast<uint8_t>(~shiftReadByte()) & 0x0f;
    encoderAB_ = previousAB_ = shiftReadByte();
#endif
    recoveryHeld_ = heldBeforeReset && (stableDown_ & 1);
    if (recoveryHeld_) {
        showUsbUpdate();
    } else {
        drawText(10, 0, "MCL TURBO", 2);
        drawProgress(progressInterior / 3);
        nextByte_ = 0;
    }
    // Complete the first frame before configuring UARTs, then keep later
    // transfers short enough for the main loop to service MIDI between them.
    while (nextByte_ < frameBytes) flushChunk();
    fastFlush_ = false;
}

void Panel::dinReady() {
    if (screen_ != Screen::Starting) return;
    drawProgress(2 * progressInterior / 3);
    nextByte_ = progressPage * width;
    fastFlush_ = true;
}

void Panel::showUsbUpdate() {
    if (screen_ == Screen::Recovery) return;
    screen_ = Screen::Recovery;
    PORTE &= ~(turboLed[0] | turboLed[1]);
    memset(frame_, 0, sizeof(frame_));
    drawText(4, 8, "USB UPDATE", 2);
    nextByte_ = 0;
    fastFlush_ = true;
}

uint8_t Panel::takeTurboPresses(uint32_t now) {
    if (now - lastButtonPoll_ < 2) return 0;
    lastButtonPoll_ = now;
    const uint8_t down = pressedButtons();
    uint8_t pressed = 0;
    for (uint8_t i = 0; i < 4; ++i) {
        const uint8_t mask = 1 << i;
        if ((down ^ rawDown_) & mask) rawChangedAt_[i] = now;
        if (((down ^ stableDown_) & mask) && now - rawChangedAt_[i] >= debounceMs) {
            stableDown_ ^= mask;
            pressed |= down & mask;
        }
    }
    rawDown_ = down;
    return pressed;
}

#ifdef MCDT_PANEL
void Panel::pollEncoders(uint32_t now) {
    latchPanel();
    const uint8_t buttons = static_cast<uint8_t>(~shiftReadByte()) & 0x0f;
    const uint8_t inputs = shiftReadByte();
    if (buttons != rawEncoderButtons_) {
        rawEncoderButtons_ = buttons;
        encoderButtonsChangedAt_ = now;
    }
    if (now - encoderButtonsChangedAt_ >= debounceMs) encoderButtons_ = buttons;

    // Match the half-step transitions in MCL's AVR GUI hardware decoder.
    for (uint8_t i = 0; i < 4; ++i) {
        const uint8_t shift = i * 2;
        const uint8_t old = (encoderAB_ >> shift) & 3;
        const uint8_t next = (inputs >> shift) & 3;
        if (old == next) continue;
        const uint8_t mask = 3 << shift;
        const uint8_t previous = (previousAB_ >> shift) & 3;
        previousAB_ = (previousAB_ & ~mask) | (old << shift);
        if ((previous == 0 && old == 1 && next == 3) ||
            (previous == 3 && old == 2 && next == 0)) {
            if (encoderDelta_[i] < 32767) ++encoderDelta_[i];
        } else if ((previous == 0 && old == 2 && next == 3) ||
                   (previous == 3 && old == 1 && next == 0)) {
            if (encoderDelta_[i] > -32767) --encoderDelta_[i];
        }
    }
    encoderAB_ = inputs;
}

bool Panel::takeEncoderReport(uint32_t now, uint8_t *data) {
    const uint32_t elapsed = now - lastEncoderReport_;
    if (elapsed < 8) return false;
    bool changed = encoderButtons_ != reportedEncoderButtons_;
    for (uint8_t i = 0; i < 4; ++i) changed |= encoderDelta_[i] != 0;
    if (!changed && elapsed < 500) return false;

    lastEncoderReport_ = now;
    reportedEncoderButtons_ = encoderButtons_;
    data[0] = encoderSequence_++ & 0x7f;
    data[1] = encoderButtons_;
    for (uint8_t i = 0; i < 4; ++i) {
        const int16_t delta = encoderDelta_[i] > 63 ? 63 : encoderDelta_[i] < -63 ? -63 : encoderDelta_[i];
        data[2 + i] = delta + 64;
        encoderDelta_[i] -= delta;
    }
    return true;
}
#endif

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

void Panel::midiIn(uint8_t port, uint32_t now) { input_[port].pulse(now); }
void Panel::midiOut(uint8_t port, uint32_t now) { output_[port].pulse(now); }

void Panel::service(uint32_t now, const Status *ports, bool usbReady) {
    uint16_t state[2] = {};
    uint8_t activity[2] = {};
    for (uint8_t i = 0; i < 2; ++i) {
        const Status &port = ports[i];
        activity[i] = input_[i].visible(now) | (output_[i].visible(now) << 1);
        state[i] = port.speed | (port.negotiating << 4) | (port.linkFailed << 5)
                   | (activity[i] << 6) | (port.limit << 8)
                   | (static_cast<uint16_t>(port.fault) << 12)
                   | (port.showLimit ? uint16_t{0x8000} : uint16_t{0});
        const bool lit = port.negotiating ? (now & 128) : port.speed > 1;
        if (lit) PORTE |= turboLed[i];
        else PORTE &= ~turboLed[i];
    }
    const bool firstPorts = screen_ == Screen::Starting;
    if (firstPorts && now < splashMs) {
        if (usbReady && !usbDisplayed_) {
            drawProgress(progressInterior);
            nextByte_ = progressPage * width;
            fastFlush_ = true;
            usbDisplayed_ = true;
        }
        return;
    }
    const bool usbChanged = usbReady != usbDisplayed_;
    const bool statusChanged = ((state[0] ^ displayed_[0]) | (state[1] ^ displayed_[1])) & portStatusMask;
    const bool priority = firstPorts || usbChanged || statusChanged;
    if (priority || (nextByte_ == frameBytes && now - lastDraw_ >= 50 &&
        (state[0] != displayed_[0] || state[1] != displayed_[1]))) {
        lastDraw_ = now;
        drawPorts(ports, activity, usbReady);
        displayed_[0] = state[0];
        displayed_[1] = state[1];
        screen_ = Screen::Ports;
        usbDisplayed_ = usbReady;
        nextByte_ = 0;
        fastFlush_ = priority;
    }
    if (!fastFlush_ && nextByte_ < frameBytes) flushChunk();
}

void Panel::flushPriorityChunk() {
    if (!fastFlush_) return;
    flushChunk();
    fastFlush_ = nextByte_ < frameBytes;
}

void Panel::drawProgress(uint8_t filled) {
    uint8_t *bar = frame_ + progressPage * width + progressX;
    bar[0] = bar[progressWidth - 1] = 0x1f;
    for (uint8_t i = 1; i < progressWidth - 1; ++i) {
        bar[i] = i <= filled ? 0x1f : 0x11;
    }
}

void Panel::drawPorts(const Status *ports, const uint8_t *activity, bool usbReady) {
    memset(frame_, 0, sizeof(frame_));
    constexpr uint8_t portWidth = width / 2;
    frame_[portWidth - 1] = 0xff;
    frame_[width + portWidth - 1] = 0x7f;
    frame_[2 * width + portWidth - 1] = 0xfe;
    if (usbReady) frame_[3 * width + portWidth - 1] = 0x7f;
    for (uint8_t i = 0; i < 2; ++i) {
        const uint8_t x = i * portWidth;
        const Status &port = ports[i];
        const char *speed = speedLabel(port.speed);
        drawText(x + 5, 0, speed, strlen(speed) <= 2 ? 2 : 1);
        drawText(x + 39, 0, "I", 1);
        drawIndicator(x + 50, 0, activity[i] & 1);
        drawText(x + 39, 8, "O", 1);
        drawIndicator(x + 50, 8, activity[i] & 2);
        drawText(x + 26, 17, i ? "P2" : "P1", 1);
        if (!usbReady) continue;
        const char *label = nullptr;
        switch (port.fault) {
        case Fault::RxFull: label = "RX FULL"; break;
        case Fault::Din: label = "DIN ERR"; break;
        case Fault::TxFull: label = "TX FULL"; break;
        case Fault::Usb: label = "USB ERR"; break;
        case Fault::None: break;
        }
        if (!label && port.negotiating) label = "TURBO";
        if (!label && port.linkFailed) label = "NO TURBO";
        if (label) {
            const uint8_t textWidth = strlen(label) * 6 - 1;
            drawText(x + (portWidth - textWidth) / 2, 24, label, 1);
        } else if (port.showLimit) {
            const char *limit = speedLabel(port.limit);
            const uint8_t textWidth = (4 + strlen(limit)) * 6 - 1;
            const uint8_t left = x + (portWidth - textWidth) / 2;
            drawText(left, 24, "MAX", 1);
            drawText(left + 24, 24, limit, 1);
        }
    }
    if (!usbReady) drawText(40, 24, "WAIT USB", 1);
}

void Panel::drawText(uint8_t x, uint8_t y, const char *text, uint8_t scale) {
    uint8_t *row = frame_ + y / 8 * width;
    // Single-height glyphs can shift down one pixel within the page.
    const uint8_t shift = y & 1;
    for (; *text && x + 5 * scale <= width; ++text, x += 6 * scale) {
        for (const Glyph &glyph : glyphs) {
            if (pgm_read_byte(&glyph.symbol) != *text) continue;
            uint8_t *out = row + x;
            for (uint8_t column = 0; column < 5; ++column) {
                const uint8_t dots = pgm_read_byte(&glyph.columns[column]);
                if (scale == 1) {
                    *out++ |= dots << shift;
                } else {
                    const uint8_t top = pgm_read_byte(doubledNibble + (dots & 15));
                    const uint8_t bottom = pgm_read_byte(doubledNibble + (dots >> 4));
                    out[0] |= top;
                    out[1] |= top;
                    out[width] |= bottom;
                    out[width + 1] |= bottom;
                    out += 2;
                }
            }
            break;
        }
    }
}

void Panel::drawIndicator(uint8_t x, uint8_t y, bool active) {
    uint8_t *pixels = frame_ + (y / 8) * width + x;
    pixels[0] = pixels[3] = 0x7c;
    pixels[1] = pixels[2] = active ? 0x7c : 0x44;
}

void Panel::flushChunk() {
    constexpr uint8_t chunkBytes = 16;
    writePixels(nextByte_ % width, nextByte_ / width, frame_ + nextByte_, chunkBytes);
    nextByte_ += chunkBytes;
}

void Panel::writePixels(uint8_t column, uint8_t page, const uint8_t *data, uint8_t count) {
    PORTL &= ~(oledSelect | oledData);
    spiByte(0xB0 | page);
    spiByte(column & 0x0F);
    spiByte(0x10 | (column >> 4));
    PORTL |= oledSelect;

    PORTL |= oledData;
    PORTL &= ~oledSelect;
    for (uint8_t i = 0; i < count; ++i) spiByte(data[i]);
    PORTL |= oledSelect;
}
