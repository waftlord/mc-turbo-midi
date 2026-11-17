/* Capture the legacy MIDI byte stream and MIDI 1.0 UMP from the same port.
 * clang -Wall -Wextra -Werror -fblocks tests/passive_coremidi_capture.c \
 *   -framework CoreMIDI -framework CoreFoundation -o /tmp/mc-passive-capture
 * /tmp/mc-passive-capture /tmp/mc-bench-run 60
 */

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FILE *bytes;
    FILE *packets;
    FILE *ump;
    size_t offset;
} Capture;

static void receive_bytes(const MIDIPacketList *list, void *context, void *connection) {
    (void)context;
    Capture *capture = connection;
    const MIDIPacket *packet = list->packet;
    for (UInt32 i = 0; i < list->numPackets; ++i, packet = MIDIPacketNext(packet)) {
        fwrite(packet->data, 1, packet->length, capture->bytes);
        fprintf(capture->packets, "%llu %zu %u\n",
                (unsigned long long)packet->timeStamp, capture->offset, packet->length);
        capture->offset += packet->length;
    }
}

static void receive_ump(const MIDIEventList *list, void *connection) {
    Capture *capture = connection;
    const MIDIEventPacket *packet = list->packet;
    for (UInt32 i = 0; i < list->numPackets; ++i, packet = MIDIEventPacketNext(packet)) {
        fprintf(capture->ump, "%llu", (unsigned long long)packet->timeStamp);
        for (UInt32 word = 0; word < packet->wordCount; ++word) {
            fprintf(capture->ump, " %08x", (unsigned)packet->words[word]);
        }
        fputc('\n', capture->ump);
    }
}

static unsigned port_number(MIDIEndpointRef source) {
    SInt32 offline = 0;
    (void)MIDIObjectGetIntegerProperty(source, kMIDIPropertyOffline, &offline);
    if (offline) return 0;
    CFStringRef name = NULL;
    if (MIDIObjectGetStringProperty(source, kMIDIPropertyDisplayName, &name) || !name) return 0;
    unsigned number = 0;
    if (CFEqual(name, CFSTR("MegaCMD Port 1"))) number = 1;
    if (CFEqual(name, CFSTR("MegaCMD Port 2"))) number = 2;
    CFRelease(name);
    return number;
}

static FILE *capture_file(const char *prefix, unsigned port, const char *suffix) {
    char path[1024];
    int length = snprintf(path, sizeof(path), "%s-port%u%s", prefix, port, suffix);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "Output prefix too long\n");
        return NULL;
    }
    FILE *file = fopen(path, "wbx");
    if (!file) perror(path);
    return file;
}

int main(int argc, char **argv) {
    char *end = NULL;
    const double seconds = argc == 3 ? strtod(argv[2], &end) : 0;
    if (argc != 3 || !end || *end || !(seconds > 0)) {
        fprintf(stderr, "Usage: %s output-prefix seconds\n", argv[0]);
        return 2;
    }
    MIDIClientRef client = 0;
    MIDIPortRef bytes = 0;
    MIDIPortRef ump = 0;
    OSStatus status = MIDIClientCreate(CFSTR("MC passive bench capture"), NULL, NULL, &client);
    if (!status) status = MIDIInputPortCreate(client, CFSTR("Raw MIDI bytes"), receive_bytes, NULL, &bytes);
    if (!status) {
        status = MIDIInputPortCreateWithProtocol(client, CFSTR("MIDI 1.0 UMP"), kMIDIProtocol_1_0,
                                                 &ump, ^(const MIDIEventList *list, void *connection) {
            receive_ump(list, connection);
        });
    }
    if (status) {
        fprintf(stderr, "CoreMIDI setup failed (%d)\n", (int)status);
        return 1;
    }

    unsigned connected = 0;
    Capture captures[2] = {0};
    for (ItemCount i = 0; i < MIDIGetNumberOfSources(); ++i) {
        MIDIEndpointRef source = MIDIGetSource(i);
        const unsigned port = port_number(source);
        if (!port) continue;
        Capture *capture = &captures[port - 1];
        if (capture->bytes) continue;
        capture->bytes = capture_file(argv[1], port, ".midistream");
        capture->packets = capture_file(argv[1], port, "-packets.txt");
        capture->ump = capture_file(argv[1], port, "-ump.txt");
        if (!capture->bytes || !capture->packets || !capture->ump) return 1;
        status = MIDIPortConnectSource(bytes, source, capture);
        if (!status) status = MIDIPortConnectSource(ump, source, capture);
        if (status) {
            fprintf(stderr, "CoreMIDI port %u connection failed (%d)\n", port, (int)status);
            return 1;
        }
        ++connected;
        printf("Listening to MegaCMD Port %u\n", port);
    }
    if (!connected) {
        fprintf(stderr, "No online MegaCMD inputs found\n");
        return 1;
    }
    printf("Armed for %g seconds. No MIDI is sent.\n", seconds);
    fflush(stdout);
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
    MIDIClientDispose(client);
    int io_error = 0;
    for (unsigned i = 0; i < 2; ++i) {
        Capture *capture = &captures[i];
        if (!capture->bytes) continue;
        io_error |= (fclose(capture->bytes) == EOF);
        io_error |= (fclose(capture->packets) == EOF);
        io_error |= (fclose(capture->ump) == EOF);
        printf("Port %u captured %zu MIDI bytes\n", i + 1, capture->offset);
    }
    return io_error ? 1 : 0;
}
