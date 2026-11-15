#pragma once

#include <stdint.h>
#include <avr/pgmspace.h>

#define BRIDGE_START 0xff
#define BRIDGE_ESCAPE 0xfe
#define BRIDGE_BANK 64
#define BRIDGE_DEVICE_BANK 64
#define BRIDGE_DEVICE_WINDOW 2
#define BRIDGE_DATA 1
#define BRIDGE_CREDIT 2
#define BRIDGE_HELLO 3
#define BRIDGE_RESET 4
#define BRIDGE_CDC 5
#define BRIDGE_ACK 6
#define BRIDGE_HEARTBEAT 7
#define BRIDGE_SYSEX 8
#define BRIDGE_BAD_FRAME 255
#define BRIDGE_HOST_BANK 64
#define BRIDGE_HOST_WINDOW 2
#define BRIDGE_DIN_CREDIT 7680
#define BRIDGE_CDC_FRAME_GAP_MS 80

// A normal ATmega2560 application boot can return a paired USB controller
// from CDC to MIDI. This token is outside the STK500v2 command protocol.
#define BRIDGE_MIDI_STARTUP_BYTES 0xa7, 0x3d, 0x91, 0x48, 0xbc, 0x62, 0x15, 0xe3, 0x56, 0x89, 0xd2, 0x6c

/* A frame starts with FF, then type, payload and CRC-8 (polynomial 0x07).
   DATA carries a length byte followed by 4-byte USB-MIDI events.
   SYSEX carries three 7-bit continuation bytes per event, with the port
   in bit 7 of the first byte. Its length field is the expanded bank length.
   CREDIT carries two little-endian DIN byte counts and one host batch count.
   From the USB chip, the first byte is the device-to-host batch size
   and the last byte grants additional device-to-host batches.
   FE 00 and FE 01 encode literal FE and FF inside a frame. */
static const uint8_t bridgeCrcTable[] PROGMEM = {
    0x00, 0x07, 0x0e, 0x09, 0x1c, 0x1b, 0x12, 0x15, 0x38, 0x3f, 0x36, 0x31, 0x24, 0x23, 0x2a, 0x2d,
    0x70, 0x77, 0x7e, 0x79, 0x6c, 0x6b, 0x62, 0x65, 0x48, 0x4f, 0x46, 0x41, 0x54, 0x53, 0x5a, 0x5d,
    0xe0, 0xe7, 0xee, 0xe9, 0xfc, 0xfb, 0xf2, 0xf5, 0xd8, 0xdf, 0xd6, 0xd1, 0xc4, 0xc3, 0xca, 0xcd,
    0x90, 0x97, 0x9e, 0x99, 0x8c, 0x8b, 0x82, 0x85, 0xa8, 0xaf, 0xa6, 0xa1, 0xb4, 0xb3, 0xba, 0xbd,
    0xc7, 0xc0, 0xc9, 0xce, 0xdb, 0xdc, 0xd5, 0xd2, 0xff, 0xf8, 0xf1, 0xf6, 0xe3, 0xe4, 0xed, 0xea,
    0xb7, 0xb0, 0xb9, 0xbe, 0xab, 0xac, 0xa5, 0xa2, 0x8f, 0x88, 0x81, 0x86, 0x93, 0x94, 0x9d, 0x9a,
    0x27, 0x20, 0x29, 0x2e, 0x3b, 0x3c, 0x35, 0x32, 0x1f, 0x18, 0x11, 0x16, 0x03, 0x04, 0x0d, 0x0a,
    0x57, 0x50, 0x59, 0x5e, 0x4b, 0x4c, 0x45, 0x42, 0x6f, 0x68, 0x61, 0x66, 0x73, 0x74, 0x7d, 0x7a,
    0x89, 0x8e, 0x87, 0x80, 0x95, 0x92, 0x9b, 0x9c, 0xb1, 0xb6, 0xbf, 0xb8, 0xad, 0xaa, 0xa3, 0xa4,
    0xf9, 0xfe, 0xf7, 0xf0, 0xe5, 0xe2, 0xeb, 0xec, 0xc1, 0xc6, 0xcf, 0xc8, 0xdd, 0xda, 0xd3, 0xd4,
    0x69, 0x6e, 0x67, 0x60, 0x75, 0x72, 0x7b, 0x7c, 0x51, 0x56, 0x5f, 0x58, 0x4d, 0x4a, 0x43, 0x44,
    0x19, 0x1e, 0x17, 0x10, 0x05, 0x02, 0x0b, 0x0c, 0x21, 0x26, 0x2f, 0x28, 0x3d, 0x3a, 0x33, 0x34,
    0x4e, 0x49, 0x40, 0x47, 0x52, 0x55, 0x5c, 0x5b, 0x76, 0x71, 0x78, 0x7f, 0x6a, 0x6d, 0x64, 0x63,
    0x3e, 0x39, 0x30, 0x37, 0x22, 0x25, 0x2c, 0x2b, 0x06, 0x01, 0x08, 0x0f, 0x1a, 0x1d, 0x14, 0x13,
    0xae, 0xa9, 0xa0, 0xa7, 0xb2, 0xb5, 0xbc, 0xbb, 0x96, 0x91, 0x98, 0x9f, 0x8a, 0x8d, 0x84, 0x83,
    0xde, 0xd9, 0xd0, 0xd7, 0xc2, 0xc5, 0xcc, 0xcb, 0xe6, 0xe1, 0xe8, 0xef, 0xfa, 0xfd, 0xf4, 0xf3
};

static inline uint8_t bridgeCrc(uint8_t crc, uint8_t byte) {
    return pgm_read_byte(bridgeCrcTable + (crc ^ byte));
}

static inline uint8_t bridgeControlLength(uint8_t type) {
    return type == BRIDGE_CREDIT ? 5 : 0;
}

typedef struct {
#ifdef __AVR_ATmega16U2__
    uint8_t data[BRIDGE_DEVICE_BANK];
#else
    uint8_t data[BRIDGE_HOST_BANK];
#endif
    uint8_t control[5];
    uint8_t type;
    uint8_t length;
    uint8_t next;
    uint8_t end;
    uint8_t crc;
    uint8_t escaped;
    uint8_t phase;
} BridgeReceiver;

static inline __attribute__((always_inline)) void bridgePayload(BridgeReceiver *receiver, uint8_t byte) {
    // Offsets address either the data or the control array.
    ((uint8_t *)receiver)[receiver->next++] = byte;
    receiver->crc = bridgeCrc(receiver->crc, byte);
    if (receiver->next == receiver->end) receiver->phase = 4;
}

static inline uint8_t bridgeReceive(BridgeReceiver *receiver, uint8_t byte) {
    const uint8_t phase = receiver->phase;
    if (phase == 3 && byte < BRIDGE_ESCAPE && !receiver->escaped) {
        bridgePayload(receiver, byte);
        return 0;
    }
    if (byte == BRIDGE_START) {
        receiver->phase = 1;
        receiver->escaped = 0;
        return phase ? BRIDGE_BAD_FRAME : 0;
    }
    if (!phase) return 0;
    if (receiver->escaped) {
        receiver->escaped = 0;
        if (byte > 1) {
            receiver->phase = 0;
            return BRIDGE_BAD_FRAME;
        }
        byte += BRIDGE_ESCAPE;
    } else if (byte == BRIDGE_ESCAPE) {
        receiver->escaped = 1;
        return 0;
    }

    switch (phase) {
    case 1:
        if (byte < BRIDGE_DATA || byte > BRIDGE_SYSEX) {
            receiver->phase = 0;
            return BRIDGE_BAD_FRAME;
        }
        receiver->type = byte;
        receiver->crc = bridgeCrc(0, byte);
        receiver->next = sizeof(receiver->data);
        receiver->length = bridgeControlLength(byte);
        receiver->end = receiver->next + receiver->length;
        receiver->phase = byte == BRIDGE_DATA || byte == BRIDGE_SYSEX ? 2 : receiver->length ? 3 : 4;
        return 0;
    case 2:
        if (!byte || byte > sizeof(receiver->data) || (byte & 3)) {
            receiver->phase = 0;
            return BRIDGE_BAD_FRAME;
        }
        receiver->length = byte;
        receiver->crc = bridgeCrc(receiver->crc, byte);
        if (receiver->type == BRIDGE_SYSEX) receiver->length -= receiver->length / 4;
        receiver->next = 0;
        receiver->end = receiver->length;
        receiver->phase = 3;
        return 0;
    case 3:
        bridgePayload(receiver, byte);
        return 0;
    }
    receiver->phase = 0;
    return byte == receiver->crc ? receiver->type : BRIDGE_BAD_FRAME;
}

// USB-MIDI 1.0 also permits raw bytes with CIN F.
static inline uint8_t bridgeUsbMidiLength(const uint8_t *event) {
    if (event[0] >= 0x20) return 0;
    const uint8_t cin = event[0] & 15;
    const uint8_t first = event[1];
    if (cin == 4) {
        return (first < 0x80 || first == 0xf0) && (event[2] | event[3]) < 0x80 ? 3 : 0;
    }
    if (cin == 15) return 1;
    if (cin == 5) return first == 0xf6 || first == 0xf7;
    if (cin == 6 || cin == 7) {
        return (first < 0x80 || first == 0xf0) &&
            (cin == 6 ? event[2] == 0xf7 : event[2] < 0x80 && event[3] == 0xf7)
            ? cin - 4 : 0;
    }
    if (cin == 2) return (first == 0xf1 || first == 0xf3) && event[2] < 0x80 ? 2 : 0;
    if (cin == 3) return first == 0xf2 && (event[2] | event[3]) < 0x80 ? 3 : 0;
    if (cin < 8 || (first >> 4) != cin || event[2] >= 0x80) return 0;
    if (cin == 12 || cin == 13) return 2;
    return event[3] < 0x80 ? 3 : 0;
}
