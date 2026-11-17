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
    ports[1].speed = ports[1].limit = 1;
    GPIOR0 = 5;
    panel.service(250, ports, true);
    GPIOR0 = 6;
    settle(251, true);
    GPIOR0 = 1;
    panel.midiIn(0, 350);
    panel.service(350, ports, true);
    GPIOR0 = 2;
    settle(351, true);
    GPIOR0 = 3;
    ports[0].fault = Panel::Fault::Din;
    ports[1].fault = Panel::Fault::Usb;
    settle(500, false);
    GPIOR0 = 4;
    ports[0].negotiating = true;
    panel.service(640, ports, false);
    GPIOR0 = 7;
    panel.service(768, ports, false);
    GPIOR0 = 8;
    for (;;) {}
}
