#pragma once

#include <stdint.h>

class Panel {
public:
    void begin();
    bool recoveryHeld() const { return recoveryHeld_; }
    bool takeTurboPress(uint32_t now);
    void midiIn(uint32_t now);
    void midiOut(uint32_t now);
    void service(uint32_t now, uint8_t speedCode, bool negotiating,
                 bool failed = false);

private:
    struct Activity {
        uint32_t last = 0;
        bool active = false;

        void pulse(uint32_t now);
        bool visible(uint32_t now);
    };

    void draw(uint8_t speedCode, bool negotiating, bool failed,
              bool input, bool output);
    void drawText(uint8_t x, uint8_t y, const char *text, uint8_t scale);
    void drawIndicator(uint8_t x, bool active);
    void pixel(uint8_t x, uint8_t y);
    void flushChunk();

    static constexpr uint8_t width = 128;
    static constexpr uint8_t height = 32;
    static constexpr uint16_t frameBytes = width * height / 8;

    uint8_t frame_[frameBytes] = {};
    uint16_t nextByte_ = frameBytes;
    uint32_t lastButtonPoll_ = 0;
    uint32_t rawChangedAt_ = 0;
    Activity input_;
    Activity output_;
    uint8_t displayedSpeed_ = 0xFF;
    bool displayedNegotiating_ = false;
    bool displayedFailed_ = false;
    bool displayedInput_ = false;
    bool displayedOutput_ = false;
    bool recoveryHeld_ = false;
    bool rawDown_ = false;
    bool stableDown_ = false;
};
