/* Isolated macOS DIN OUT -> named DIN IN loopback, fixed 1x. */
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <errno.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { fixtureSize = 4038, captureCapacity = 32768 };
static const unsigned sizes[] = {1176, 1176, 873, 813};
static const char *const names[] = {"MegaCMD Port 1", "MegaCMD Port 2"};

typedef struct {
    pthread_mutex_t lock;
    size_t length;
    bool overflow;
    MIDITimeStamp first, last;
    uint8_t bytes[captureCapacity];
} Capture;

static unsigned portNumber(const char *name) {
    for (unsigned i = 0; i < 2; ++i)
        if (strcmp(name, names[i]) == 0) return i + 1;
    return 0;
}

static void fixture(unsigned port, uint8_t *bytes) {
    for (unsigned record = 0; record < sizeof(sizes) / sizeof(*sizes); ++record) {
        const unsigned n = sizes[record];
        const uint8_t header[] = {0xf0, 0x7d, 'D', 'T', 'L', port, record};
        memcpy(bytes, header, sizeof(header));
        for (unsigned i = sizeof(header); i < n - 1; ++i)
            bytes[i] = (i * 31 + (i >> 3) + record * 17 + port * 19) & 127;
        bytes[n - 1] = 0xf7;
        bytes += n;
    }
}

static bool saveNew(const char *path, const uint8_t *bytes, size_t length) {
    FILE *file = fopen(path, "wbx");
    if (!file) { perror(path); return false; }
    bool ok = fwrite(bytes, 1, length, file) == length;
    if (fclose(file)) ok = false;
    if (!ok) fprintf(stderr, "Could not finish writing %s\n", path);
    return ok;
}

/* Check all bytes, including the first byte after the expected end. Only the
   exact generated noncommercial fixture for this port is eligible to send. */
static bool matchesFile(const char *path, const uint8_t *expected) {
    FILE *file = fopen(path, "rb");
    if (!file) { perror(path); return false; }
    size_t offset = 0;
    while (offset < fixtureSize && fgetc(file) == expected[offset]) ++offset;
    bool match = offset == fixtureSize && fgetc(file) == EOF && !ferror(file);
    if (fclose(file)) match = false;
    if (!match) fprintf(stderr, "%s differs from this port's synthetic fixture at offset %zu\n",
                        path, offset);
    return match;
}

static void receive(const MIDIPacketList *list, void *context, void *connection) {
    (void)connection;
    Capture *capture = context;
    const MIDIPacket *packet = list->packet;
    pthread_mutex_lock(&capture->lock);
    for (UInt32 i = 0; i < list->numPackets; ++i, packet = MIDIPacketNext(packet)) {
        if (!packet->length) continue;
        if (packet->length > captureCapacity - capture->length) {
            capture->overflow = true;
            break;
        }
        if (!capture->length) capture->first = packet->timeStamp;
        capture->last = packet->timeStamp;
        memcpy(capture->bytes + capture->length, packet->data, packet->length);
        capture->length += packet->length;
    }
    pthread_mutex_unlock(&capture->lock);
}

static MIDIEndpointRef endpoint(const char *name, bool source) {
    ItemCount count = source ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations();
    MIDIEndpointRef result = 0;
    for (ItemCount i = 0; i < count; ++i) {
        MIDIEndpointRef candidate = source ? MIDIGetSource(i) : MIDIGetDestination(i);
        SInt32 offline = 0;
        MIDIObjectGetIntegerProperty(candidate, kMIDIPropertyOffline, &offline);
        if (offline) continue;
        CFStringRef display = NULL;
        if (MIDIObjectGetStringProperty(candidate, kMIDIPropertyDisplayName, &display) || !display)
            continue;
        char text[256];
        bool match = CFStringGetCString(display, text, sizeof(text), kCFStringEncodingUTF8)
                     && strcmp(text, name) == 0;
        CFRelease(display);
        if (!match) continue;
        if (result) { fprintf(stderr, "Ambiguous endpoint %s\n", name); return 0; }
        result = candidate;
    }
    return result;
}

static void waitMs(unsigned ms) {
    struct timespec delay = {ms / 1000, (long)(ms % 1000) * 1000000};
    while (nanosleep(&delay, &delay) && errno == EINTR) {}
}

static size_t observed(Capture *capture) {
    pthread_mutex_lock(&capture->lock);
    size_t length = capture->overflow ? SIZE_MAX : capture->length;
    pthread_mutex_unlock(&capture->lock);
    return length;
}

static int run(const char *destination, const char *source, const uint8_t *expected, const char *path) {
    union { MIDIPacketList list; uint8_t storage[fixtureSize + 256]; } send;
    MIDIPacket *packet = MIDIPacketListInit(&send.list);
    size_t offset = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(*sizes); ++i) {
        packet = MIDIPacketListAdd(&send.list, sizeof(send), packet, 0, sizes[i], expected + offset);
        if (!packet) { fputs("Could not prepare test packets\n", stderr); return 1; }
        offset += sizes[i];
    }

    MIDIClientRef client = 0;
    MIDIPortRef input = 0, output = 0;
    Capture capture = { .lock = PTHREAD_MUTEX_INITIALIZER };
    OSStatus status = MIDIClientCreate(CFSTR("MegaCMD isolated loopback"), NULL, NULL, &client);
    if (!status) status = MIDIInputPortCreate(client, CFSTR("capture"), receive, &capture, &input);
    if (!status) status = MIDIOutputPortCreate(client, CFSTR("sender"), &output);
    if (status) {
        fprintf(stderr, "CoreMIDI setup error %d\n", (int)status);
        if (client) MIDIClientDispose(client);
        return 1;
    }
    int result = 1;
    MIDIEndpointRef src = endpoint(source, true), dst = endpoint(destination, false);
    MIDIEntityRef sourceEntity = 0, destEntity = 0;
    MIDIDeviceRef sourceDevice = 0, destDevice = 0;
    if (!src || !dst || MIDIEndpointGetEntity(src, &sourceEntity) ||
        MIDIEndpointGetEntity(dst, &destEntity) || MIDIEntityGetDevice(sourceEntity, &sourceDevice) ||
        MIDIEntityGetDevice(destEntity, &destDevice) || !sourceDevice || sourceDevice != destDevice ||
        MIDIPortConnectSource(input, src, NULL)) {
        fprintf(stderr, "Could not bind %s OUT and %s IN on the same MegaCMD\n", destination, source);
        goto done;
    }
    waitMs(500);
    if (observed(&capture)) { fputs("Ambient MIDI on selected input; send skipped\n", stderr); goto done; }
    FILE *file = fopen(path, "wbx");
    if (!file) { perror(path); goto done; }
    printf("Sending four synthetic records (%u bytes) on %s OUT; capturing %s IN\n",
           fixtureSize, destination, source);
    fflush(stdout);
    status = MIDISend(output, dst, &send.list);
    if (status) {
        fprintf(stderr, "CoreMIDI send error %d\n", (int)status);
        fclose(file);
        goto done;
    }

    size_t previous = 0;
    unsigned quietMs = 0;
    for (unsigned elapsed = 0; elapsed < 7000 && quietMs < 600; elapsed += 50) {
        waitMs(50);
        size_t length = observed(&capture);
        quietMs = length == previous && length >= fixtureSize ? quietMs + 50 : 0;
        previous = length;
    }
    MIDIPortDisconnectSource(input, src);
    pthread_mutex_lock(&capture.lock);
    bool ok = fwrite(capture.bytes, 1, capture.length, file) == capture.length;
    if (fclose(file)) ok = false;
    offset = 0;
    while (offset < fixtureSize && offset < capture.length &&
           capture.bytes[offset] == expected[offset]) ++offset;
    mach_timebase_info_data_t timebase;
    mach_timebase_info(&timebase);
    double ms = (capture.last - capture.first) * (double)timebase.numer / timebase.denom / 1e6;
    printf("%zu/%u bytes; first-to-last CoreMIDI packet timestamps %.3f ms.\n",
           capture.length, fixtureSize, ms);
    if (ok && !capture.overflow && quietMs >= 600 && offset == fixtureSize &&
        capture.length == fixtureSize) {
        puts("Exact contents, boundaries and length, including the trailing quiet window.");
        result = 0;
    } else {
        fprintf(stderr, "Verification failed (matching prefix %zu, receive overflow %d, "
                "trailing quiet %u ms, capture saved %d)\n", offset, capture.overflow, quietMs, ok);
    }
    pthread_mutex_unlock(&capture.lock);
done:
    MIDIClientDispose(client);
    return result;
}

int main(int argc, char **argv) {
    if (argc >= 4 && argc <= 6) {
        unsigned port = portNumber(argv[2]);
        if (!port) { fputs("Specify MegaCMD Port 1 or MegaCMD Port 2 exactly\n", stderr); return 2; }
        uint8_t expected[fixtureSize];
        fixture(port, expected);
        if (argc == 4 && strcmp(argv[1], "--generate") == 0)
            return saveNew(argv[3], expected, sizeof(expected)) ? 0 : 1;
        if (argc == 4 && strcmp(argv[1], "--check") == 0)
            return matchesFile(argv[3], expected) ? 0 : 1;
        bool checkRoute = argc == 5 && strcmp(argv[1], "--check") == 0;
        if (checkRoute || (argc == 6 && strcmp(argv[1], "--run") == 0)) {
            if (!portNumber(argv[3])) { fputs("Specify the named MegaCMD return port\n", stderr); return 2; }
            if (!matchesFile(argv[4], expected)) return 1;
            if (checkRoute) return 0;
            return run(argv[2], argv[3], expected, argv[5]);
        }
    }
    fprintf(stderr, "Usage:\n  %s --generate PORT fresh.syx\n  %s --check DESTINATION [CAPTURE_SOURCE] file\n"
            "  %s --run DESTINATION CAPTURE_SOURCE fixture.syx fresh.midistream\n",
            argv[0], argv[0], argv[0]);
    return 2;
}
