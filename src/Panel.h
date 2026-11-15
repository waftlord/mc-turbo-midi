#pragma once

#include <stdint.h>

class Panel {
public:
    enum class Fault : uint8_t { None, RxFull, Din, TxFull, Usb };

    struct Status {
        uint8_t speed;
        uint8_t limit;
        bool showLimit;
        bool negotiating;
        bool linkFailed;
        Fault fault;
    };

    void begin();
    void dinReady();
    bool recoveryHeld() const { return recoveryHeld_; }
    void showUsbUpdate();
    uint8_t takeTurboPresses(uint32_t now);
    void midiIn(uint8_t port, uint32_t now);
    void midiOut(uint8_t port, uint32_t now);
    void service(uint32_t now, const Status *ports, bool usbReady);
    void flushPriorityChunk();

private:
    enum class Screen : uint8_t { Starting, Ports, Recovery };

    struct Activity {
        uint32_t last = 0;
        bool active = false;

        void pulse(uint32_t now);
        bool visible(uint32_t now);
    };

    void drawPorts(const Status *ports, const uint8_t *activity, bool usbReady);
    void drawProgress(uint8_t filled);
    void drawText(uint8_t x, uint8_t y, const char *text, uint8_t scale);
    void drawIndicator(uint8_t x, uint8_t y, bool active);
    void writePixels(uint8_t column, uint8_t page, const uint8_t *data, uint8_t count);
    void flushChunk();

    static constexpr uint8_t width = 128;
    static constexpr uint8_t height = 32;
    static constexpr uint16_t frameBytes = width * height / 8;

    uint8_t frame_[frameBytes] = {};
    uint16_t nextByte_ = frameBytes;
    uint32_t lastButtonPoll_ = 0;
    uint32_t lastDraw_ = 0;
    uint32_t rawChangedAt_[4] = {};
    Activity input_[2], output_[2];
    uint16_t displayed_[2] = {0xffff, 0xffff};
    Screen screen_ = Screen::Starting;
    bool usbDisplayed_ = false;
    bool fastFlush_ = false;
    bool recoveryHeld_ = false;
    uint8_t rawDown_ = 0;
    uint8_t stableDown_ = 0;
};
