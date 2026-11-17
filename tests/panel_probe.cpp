#include "Panel.h"
#include <avr/io.h>

namespace {
Panel panel;
Panel::Status ports[2] = {};

void settle(uint32_t now, bool usbReady) {
    for (uint8_t tick = 0; tick < 35; ++tick) {
        panel.service(now + tick, ports, usbReady);
        panel.flushPriorityChunk();
    }
}
}

int main() {
    ports[0].speed = 4;
    ports[0].limit = 7;
    ports[1].speed = 1;
    ports[1].limit = 2;
    GPIOR0 = 1;
    panel.service(250, ports, true);
    GPIOR0 = 2;
    settle(251, true);
    GPIOR0 = 3;

    panel.midiIn(0, 350);
    panel.service(350, ports, true);
    GPIOR0 = 4;
    settle(351, true);
    GPIOR0 = 5;

    // Three edges at once, serviced with at most one indicator per tick.
    panel.midiOut(0, 390);
    panel.midiIn(1, 390);
    panel.midiOut(1, 390);
    for (uint8_t tick = 0; tick < 3; ++tick) {
        panel.service(390 + tick, ports, true);
        GPIOR0 = 6 + tick;
    }
    ports[0].speed = 1;
    ports[1].speed = 4;
    settle(500, true);
    GPIOR0 = 9;

#ifdef PANEL_HAS_PREVIEW
    ports[0].previewLimit = ports[1].previewLimit = true;
#endif
    settle(545, true);
    GPIOR0 = 10;
#ifdef PANEL_HAS_PREVIEW
    ports[0].previewLimit = ports[1].previewLimit = false;
#endif
    // Only the preview changes from stages 9-11.
    settle(590, true);
    GPIOR0 = 11;

    ports[0].negotiating = true;
    ports[1].linkFailed = true;
    settle(640, true);
    GPIOR0 = 12;
    panel.service(768, ports, true);
    GPIOR0 = 13;
    ports[0].negotiating = false;
    ports[0].linkFailed = true;
    ports[1].linkFailed = false;
    ports[1].negotiating = true;
    settle(800, true);
    GPIOR0 = 14;

    ports[0].linkFailed = false;
    ports[1].negotiating = false;
    settle(850, false);
    GPIOR0 = 15;
    ports[0].fault = Panel::Fault::Din;
    ports[1].fault = Panel::Fault::Usb;
    ports[1].negotiating = true;
#ifdef PANEL_HAS_PREVIEW
    ports[0].previewLimit = true;
#endif
    settle(900, false);
    GPIOR0 = 16;

    ports[0].fault = Panel::Fault::RxFull;
    ports[1].fault = Panel::Fault::TxFull;
    ports[1].negotiating = false;
#ifdef PANEL_HAS_PREVIEW
    ports[0].previewLimit = false;
#endif
    settle(950, true);
    GPIOR0 = 17;
    ports[0].fault = ports[1].fault = Panel::Fault::None;
    settle(1000, true);
    GPIOR0 = 18;
#ifdef PANEL_HAS_PREVIEW
    panel.previewLimit(0, 1100);
    settle(1100, true);
    GPIOR0 = 19;
    panel.previewLimit(1, 1150);
    settle(1150, true);
    GPIOR0 = 20;
    panel.previewLimit(0, 2200);  // Extends only P1's window.
    settle(2200, true);
    GPIOR0 = 21;
    panel.service(2649, ports, true);
    GPIOR0 = 22;
    settle(2650, true);
    GPIOR0 = 23;
    panel.service(3699, ports, true);
    GPIOR0 = 24;
    settle(3700, true);
    GPIOR0 = 25;

    panel.previewLimit(1, UINT32_MAX - 750);
    settle(UINT32_MAX - 500, true);
    GPIOR0 = 26;
    panel.service(748, ports, true);  // 1499 ms after edit.
    GPIOR0 = 27;
    settle(749, true);  // 1500 ms since the edit, across wrap.
    GPIOR0 = 28;
#endif
    for (;;) {}
}
