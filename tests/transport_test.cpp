#include "MidiPacketizer.h"
#include "MidiStream.h"
#include "TurboLink.h"
#include "TurboTestSerial.h"

#include <cassert>
#include <cstdint>
#include <initializer_list>
#include <vector>

using Bytes = std::vector<uint8_t>;

struct FilterResult {
    Bytes forwarded;
    std::vector<Bytes> controls;
};

FilterResult filter(MidiSide side, const Bytes &input) {
    FilterResult result;
    ControlFilter parser(side);
    const auto forward = [&](uint8_t byte) { result.forwarded.push_back(byte); };
    const auto control = [&](const uint8_t *data, uint8_t count) {
        result.controls.emplace_back(data, data + count);
    };
    uint32_t now = 0;
    for (const auto byte : input) parser.accept(byte, now++, forward, control);
    parser.expire(now + 30, forward);
    return result;
}

Bytes message(uint8_t product, std::initializer_list<uint8_t> payload) {
    Bytes result{0xf0, 0, 0x20, 0x3c, product, 0};
    result.insert(result.end(), payload.begin(), payload.end());
    result.push_back(0xf7);
    return result;
}

void testForwarding() {
    // Standard data paths, including this project's two open firmware families.
    for (auto product : {2, 3}) {
        for (auto command : {0x52, 0x67, 0x6e, 0x6f, 0x70, 0x74}) {
            const auto frame = message(product, {uint8_t(command), 1, 0x7f, 0, 2, 3, 4, 5, 6, 7, 8});
            for (auto side : {MidiSide::Usb, MidiSide::Din}) {
                const auto result = filter(side, frame);
                assert(result.forwarded == frame);
                assert(result.controls.empty());
            }
        }
    }
    const Bytes ordinary{0xfa, 0x90, 36, 100, 38, 120, 0xf8, 0x80, 36, 0, 0xfc};
    assert(filter(MidiSide::Usb, ordinary).forwarded == ordinary);
    assert(filter(MidiSide::Din, ordinary).forwarded == ordinary);

    // These share an address or command prefix with a bridge control.
    for (const auto &frame : {message(4, {1, 5}), message(4, {3}),
                              message(4, {3, 12}), message(4, {3, 2, 1})}) {
        const auto result = filter(MidiSide::Usb, frame);
        assert(result.forwarded == frame && result.controls.empty());
    }
    for (const auto &frame : {message(0, {0x20, 2}), message(0, {0x10, 5}),
                              message(0, {0x11, 1}), message(0, {0x17, 1, 2, 3})}) {
        const auto result = filter(MidiSide::Din, frame);
        assert(result.forwarded == frame && result.controls.empty());
    }
    const Bytes incomplete{0xf0, 0, 0x20, 0x3c, 4, 0, 3, 2};
    assert(filter(MidiSide::Usb, incomplete).forwarded == incomplete);
}

void testControlIsolation() {
    const auto query = message(4, {1});
    auto withClock = query;
    withClock.insert(withClock.begin() + 3, 0xf8);
    const auto received = filter(MidiSide::Usb, withClock);
    assert(received.forwarded == Bytes{0xf8});
    assert(received.controls == std::vector<Bytes>{query});
    assert(filter(MidiSide::Din, query).forwarded == query);

    const auto limit = message(4, {3, 7});
    assert(filter(MidiSide::Usb, limit).controls == std::vector<Bytes>{limit});
    const auto off = message(4, {3, 0});
    assert(filter(MidiSide::Usb, off).controls == std::vector<Bytes>{off});
    const auto answer = message(0, {0x11, 0x25, 0, 0x25, 0});
    assert(filter(MidiSide::Din, answer).controls == std::vector<Bytes>{answer});
    assert(filter(MidiSide::Usb, answer).forwarded == answer);
    const Bytes recovery{0xf0, 0x7d, 0x4d, 0x43, 0x4c, 1, 3, 0xf7};
    assert(filter(MidiSide::Usb, recovery).controls == std::vector<Bytes>{recovery});
    assert(filter(MidiSide::Din, recovery).forwarded == recovery);
    const Bytes status{0xf0, 0x7d, 0x4d, 0x43, 0x4c, 1, 1, 0xf7};
    assert(filter(MidiSide::Usb, status).controls == std::vector<Bytes>{status});
    assert(filter(MidiSide::Din, status).forwarded == status);
    const Bytes panel{0xf0, 0x7d, 0x4d, 0x43, 0x4c, 2, 1, 127, 10, 63, 64, 65, 127, 0xf7};
    assert(filter(MidiSide::Din, panel).forwarded == panel);
}

void testRealtimeInsideControlPrefix() {
    for (auto side : {MidiSide::Usb, MidiSide::Din}) {
        ControlFilter parser(side);
        Bytes wire;
        std::vector<Bytes> controls;
        const auto forward = [&](uint8_t byte) { wire.push_back(byte); };
        const auto control = [&](const uint8_t *frame, uint8_t size) {
            controls.emplace_back(frame, frame + size);
        };
        const auto feed = [&](const Bytes &data, uint32_t now) {
            for (auto byte : data) parser.accept(byte, now, forward, control);
        };
        const Bytes ordinary{0xf0, 0, 0x20, 0xf8, 0x3c, 0xfa, 2, 0, 0xf7};
        feed(ordinary, 0);
        assert(wire == ordinary && controls.empty());

        // An unfinished prefix cannot indefinitely delay realtime. Exercise
        // the two-ms deadline across millis rollover and an excess clock burst.
        wire.clear();
        feed({0xf0, 0, 0x20, 0xf8}, UINT32_MAX);
        parser.expire(1, forward);
        assert(wire == Bytes{0xf8});
        feed({0x3c, 0xf8, 0xfa, 0xfb, 0xfc, 0xff}, 2);
        const bool usb = side == MidiSide::Usb;
        feed({static_cast<uint8_t>(usb ? 4 : 0), 0,
              static_cast<uint8_t>(usb ? 1 : 0x10), 0xf7}, 2);
        assert((wire == Bytes{0xf8, 0xf8, 0xfa, 0xfb, 0xfc, 0xff}));
        assert(controls.size() == 1);
    }
}

void testInterruptedMidi() {
    MidiPacketizer parser;
    std::vector<Bytes> events;
    const auto emit = [&](const uint8_t *event) { events.emplace_back(event, event + 4); };
    const auto feed = [&](const Bytes &bytes) {
        for (uint8_t byte : bytes) parser.accept(byte, 0x10, emit);
    };

    feed({0x45, 0xf0, 1, 0xf8, 0x90, 60, 90, 61, 91});
    assert((events == std::vector<Bytes>{{0x1f, 0xf8, 0, 0}, {0x19, 0x90, 60, 90},
                                         {0x19, 0x90, 61, 91}}));
    events.clear();
    feed({0xf0, 1, 2, 3, 0xf1, 0x37, 0x60});
    assert((events == std::vector<Bytes>{{0x14, 0xf0, 1, 2}, {0x16, 3, 0xf7, 0},
                                         {0x12, 0xf1, 0x37, 0}}));
    events.clear();
    feed({0xf0, 1, 2, 3});
    const uint8_t run[] = {4, 5, 6, 7, 8, 0xf8, 9};
    assert(parser.acceptSysExRun(run, sizeof(run), 0x10, emit) == 5);
    parser.abortSysEx(0x10, emit);
    feed({9, 10, 0xf7, 0xf6, 0xc0, 4, 5});
    assert((events == std::vector<Bytes>{{0x14, 0xf0, 1, 2}, {0x14, 3, 4, 5},
                                         {0x14, 6, 7, 8}, {0x15, 0xf7, 0, 0},
                                         {0x15, 0xf6, 0, 0}, {0x1c, 0xc0, 4, 0},
                                         {0x1c, 0xc0, 5, 0}}));
}

struct Endpoint {
    Bytes tx;
    uint8_t rate = 1;
    uint8_t reported = 1;
    unsigned reports = 0;
};

Endpoint endpoints[4];

template <unsigned N>
bool send(const uint8_t *data, uint8_t size) {
    endpoints[N].tx.insert(endpoints[N].tx.end(), data, data + size);
    return true;
}

template <unsigned N>
void setRate(uint8_t rate) { endpoints[N].rate = rate; }

template <unsigned N>
void report(uint8_t rate) {
    endpoints[N].reported = rate;
    ++endpoints[N].reports;
}

bool idle() { return true; }

void deliver(unsigned from, unsigned to, TurboLink &target, uint32_t now) {
    const auto &tx = endpoints[from].tx;
    if (endpoints[from].rate != endpoints[to].rate) {
        endpoints[from].tx.clear();
        return;
    }
    const auto deliverControl = [&](const uint8_t *data, uint8_t size) {
        assert(target.receive(data, size, now));
    };
    ControlFilter parser(MidiSide::Din);
    for (auto byte : tx) {
        target.observeDinByte(byte, now);
        parser.accept(byte, now, [](uint8_t) {}, deliverControl);
    }
    endpoints[from].tx.clear();
}

void testIndependentTurboAndFallback() {
    TurboLink first(send<0>, idle, setRate<0>, report<0>);
    TurboLink second(send<1>, idle, setRate<1>, report<1>);
    TurboLink peer(send<2>, idle, setRate<2>, report<2>);
    first.setLimit(4, 0);
    second.setLimit(7, 0);

    // The first link has a Turbo peer. The second gets no reply.
    for (uint32_t now = 0; now < 1000; ++now) {
        first.tick(now);
        second.tick(now);
        peer.tick(now);
        deliver(0, 2, peer, now);
        deliver(2, 0, first, now);
    }
    assert(first.speed() == 4 && peer.speed() == 4);
    assert(endpoints[0].rate == 4 && endpoints[0].reported == 4);
    assert(second.speed() == 1 && second.failed() && !second.busy());
    assert(endpoints[1].rate == 1 && endpoints[1].reported == 1);
    for (uint32_t now = 1000; now < 1900; ++now) first.tick(now);
    assert(first.speed() == 1 && first.failed() && !first.busy());
    assert(endpoints[0].rate == 1 && endpoints[0].reported == 1);
}

void testTwoDifferentRates() {
    for (auto &endpoint : endpoints) endpoint = Endpoint{};
    TurboLink first(send<0>, idle, setRate<0>, report<0>);
    TurboLink second(send<1>, idle, setRate<1>, report<1>);
    TurboLink firstPeer(send<2>, idle, setRate<2>, report<2>);
    TurboLink secondPeer(send<3>, idle, setRate<3>, report<3>);
    first.setLimit(4, 0);
    second.setLimit(7, 0);
    for (uint32_t now = 0; now < 1000; ++now) {
        first.tick(now);
        second.tick(now);
        firstPeer.tick(now);
        secondPeer.tick(now);
        deliver(0, 2, firstPeer, now);
        deliver(1, 3, secondPeer, now);
        deliver(2, 0, first, now);
        deliver(3, 1, second, now);
    }
    assert(first.speed() == 4 && firstPeer.speed() == 4);
    assert(second.speed() == 7 && secondPeer.speed() == 7);

    bool missedRequest = false;
    for (uint32_t now = 1000; now < 2900; ++now) {
        // Queue an explicit remote restart before the peer falls back.
        if (now == 1000) second.setLimit(1, now);
        if (now == 1100) second.setLimit(7, now);
        if (now == 1150) {
            assert(second.recovering() && endpoints[1].rate == 7);
            assert(secondPeer.speed() == 7);
        }
        first.tick(now);
        second.tick(now);
        firstPeer.tick(now);
        secondPeer.tick(now);
        if (!missedRequest && endpoints[1].tx == message(0, {0x10})) {
            missedRequest = true;
            endpoints[1].tx.clear();
        } else {
            deliver(1, 3, secondPeer, now);
        }
        deliver(3, 1, second, now);
        deliver(0, 2, firstPeer, now);
        deliver(2, 0, first, now);
        assert(first.speed() == 4 && firstPeer.speed() == 4);
    }
    assert(missedRequest && second.speed() == 7 && secondPeer.speed() == 7);
    assert(!second.reportTransportError(2900, true) && second.failed());
}

void testIndependentFinalRateConfirmation() {
    const auto receive = [](TurboLink &link, uint8_t command, const Bytes &payload, uint32_t now) {
        const auto frame = message(0, {command});
        Bytes full(frame.begin(), frame.end() - 1);
        full.insert(full.end(), payload.begin(), payload.end());
        full.push_back(0xf7);
        assert(link.receive(full.data(), full.size(), now));
    };
    for (bool swapped : {false, true}) {
        for (auto &endpoint : endpoints) endpoint = Endpoint{};
        TurboLink left(send<0>, idle, setRate<0>, report<0>);
        TurboLink right(send<1>, idle, setRate<1>, report<1>);
        TurboLink *const links[] = {&left, &right};
        const unsigned capable = swapped ? 1 : 0;
        const unsigned monomachine = 1 - capable;
        for (unsigned i = 0; i < 2; ++i) {
            auto &link = *links[i];
            link.toggle(0);
            link.tick(0);
            assert(endpoints[i].tx == message(0, {0x10}));
            endpoints[i].tx.clear();
            const uint8_t certified = i == capable ? 0x2f : 0x0f;
            const uint8_t testRate = i == capable ? 7 : 8;
            receive(link, 0x11, {0x7f, 1, certified, 0}, 5);
            link.tick(5);
            assert(endpoints[i].tx == message(0, {0x12, testRate, 7}));
            endpoints[i].tx.clear();
            receive(link, 0x13, {}, 10);
            link.tick(10);
            assert(endpoints[i].rate == testRate && endpoints[i].reports == 0);
            receive(link, 0x15, {0x55, 0x55, 0x55, 0x55, 0, 0, 0, 0}, 12);
            link.tick(12);
            assert(endpoints[i].rate == testRate && endpoints[i].reports == 0);
            receive(link, 0x17, {}, 14);
            link.tick(24);
            assert(link.busy() && link.speed() == 1 && endpoints[i].rate == 7);
            assert(endpoints[i].reported == 1);
        }
        links[capable]->observeDinByte(0xfe, 25);
        assert(links[capable]->speed() == 7 && endpoints[capable].reported == 7);
        // A data-only batch refreshes liveness, but cannot stand in for a status byte.
        links[monomachine]->observeDinByte(0x40, 25);
        links[monomachine]->noteDinActivity(25);
        assert(links[monomachine]->busy() && links[monomachine]->speed() == 1);
        assert(endpoints[monomachine].reports == 0);
        links[monomachine]->observeDinByte(0xf8, 26);
        assert(links[monomachine]->speed() == 7 && endpoints[monomachine].reported == 7);

        links[monomachine]->toggle(27);
        links[monomachine]->toggle(28);
        assert(links[monomachine]->recovering() && links[capable]->speed() == 7);
        auto &retry = *links[monomachine];
        retry.tick(27);
        retry.tick(377);
        retry.tick(378);
        receive(retry, 0x11, {0x7f, 1, 0x0f, 0}, 380);
        retry.tick(380);
        receive(retry, 0x13, {}, 385);
        retry.tick(385);
        assert(endpoints[monomachine].rate == 8 && retry.speed() == 1);
        assert(retry.reportTransportError(386, true));
        assert(retry.recovering());
        retry.tick(386);
        retry.tick(736);
        retry.tick(737);
        endpoints[monomachine].tx.clear();
        receive(retry, 0x11, {0x7f, 1, 0x0f, 0}, 740);
        retry.tick(740);
        assert(endpoints[monomachine].tx == message(0, {0x12, 4, 4}));
        assert(endpoints[capable].rate == 7 && links[capable]->speed() == 7);
    }
}

void testFastTestSerial() {
    // Independent UART receive model for the packed transmit waveform.
    for (uint8_t length = 1; length <= 32; ++length) {
        Bytes data;
        Bytes samples;
        for (uint8_t i = 0; i < length; ++i) data.push_back(i * 43 + length * 19);
        TurboTestSerial::encode(data.data(), length, [&](uint8_t packed) {
            for (uint8_t bit = 0; bit < 8; ++bit) samples.push_back((packed >> bit) & 1);
        });
        assert(samples.size() == TurboTestSerial::packedSize(length) * 8u);
        size_t cursor = 0;
        for (const auto byte : data) {
            while (samples.at(cursor)) ++cursor;
            const auto sampleBit = [&](unsigned bit) {
                return samples.at(cursor + ((bit * 2 + 1) * 32) / 25);
            };
            assert(!sampleBit(0) && sampleBit(9));
            for (uint8_t bit = 0; bit < 8; ++bit) assert(sampleBit(bit + 1) == ((byte >> bit) & 1));
            cursor += 24;
        }
        while (cursor < samples.size()) assert(samples[cursor++]);
    }
    // Independent continuous transmitter. Sweep the incoming phase across a
    // whole sample period and cover every byte value at nominal and ±0.5%.
    Bytes data;
    for (unsigned i = 0; i < 256; ++i) data.push_back(i);
    for (unsigned bitPeriod : {50944u, 51200u, 51456u}) {
        for (unsigned phase = 0; phase < 20000; phase += 125) {
            TurboTestSerial::Decoder decoder;
            Bytes received;
            unsigned sample = 0;
            for (unsigned at = 0; at < phase + 2560 * bitPeriod + 160000; at += 20000) {
                const unsigned bit = at < phase ? 2560 : (at - phase) / bitPeriod;
                const unsigned symbol = bit % 10;
                const bool high = bit >= 2560 || symbol == 9 ||
                                  (symbol > 0 && ((data.at(bit / 10) >> (symbol - 1)) & 1));
                sample = (sample >> 1) | (high ? 0x80 : 0);
                if (at % 160000 != 140000) continue;
                uint8_t value;
                const auto result = decoder.accept(sample, value);
                assert(result != TurboTestSerial::Decoder::Result::BadStop);
                if (result == TurboTestSerial::Decoder::Result::Byte) received.push_back(value);
            }
            assert(received == data);
        }
    }
}

void testSimultaneousRequestsSettleAndCanBeRetried() {
    for (auto &endpoint : endpoints) endpoint = Endpoint{};
    TurboLink first(send<0>, idle, setRate<0>, report<0>);
    TurboLink peer(send<1>, idle, setRate<1>, report<1>);
    first.setLimit(4, 0);
    peer.setLimit(7, 0);
    for (uint32_t now = 0; now < 100; ++now) {
        first.tick(now);
        peer.tick(now);
        deliver(0, 1, peer, now);
        deliver(1, 0, first, now);
    }
    assert(!first.busy() && !peer.busy());
    assert(endpoints[0].reported == 1 && endpoints[0].reports == 1);
    assert(endpoints[1].reported == 1 && endpoints[1].reports == 1);
    first.setLimit(4, 100);
    for (uint32_t now = 100; now < 500; ++now) {
        first.tick(now);
        peer.tick(now);
        deliver(0, 1, peer, now);
        deliver(1, 0, first, now);
    }
    assert(first.speed() == 4 && peer.speed() == 4);
}

void testCancelAfterPeerAcknowledges() {
    endpoints[0] = Endpoint{};
    TurboLink link(send<0>, idle, setRate<0>, report<0>);
    link.setLimit(4, 0);
    link.tick(0);
    const auto answer = message(0, {0x11, 0x25, 0, 0x25, 0});
    assert(link.receive(answer.data(), answer.size(), 4));
    link.tick(4);
    const auto ack = message(0, {0x13});
    assert(link.receive(ack.data(), ack.size(), 8));
    endpoints[0].tx.clear();
    // The peer can already be at 4x when a host cancels before the
    // foreground sends Test1. Keep the DIN wire quiet for the peer timeout.
    link.setLimit(1, 8);
    assert(link.recovering() && link.pausePayload());
    link.tick(8);
    link.setLimit(2, 12);
    link.tick(357);
    assert(link.recovering() && endpoints[0].tx.empty());
    link.tick(358);
    link.tick(359);
    assert(endpoints[0].tx == message(0, {0x10}));
    // An answer supporting only a rate outside our cap leaves 1x usable.
    const auto unsupported = message(0, {0x11, 0x20, 0, 0x20, 0});
    assert(link.receive(unsupported.data(), unsupported.size(), 363));
    assert(link.failed() && !link.busy() && link.speed() == 1);
}

void testMachinedrumUpgradeReceiver() {
    const auto answer = message(0, {0x11, 0x7f, 0, 0, 0});
    enum Reply { SilentReceiver, MissingResult2, MissingFinalAnswer, SensingPeer, RequiresHigherTest };
    struct Attempt { uint8_t cap; Reply reply; };
    const Attempt cases[] = {{7, SilentReceiver}, {4, SilentReceiver},
                             {2, SilentReceiver}, {2, MissingResult2},
                             {2, MissingFinalAnswer}, {2, SensingPeer}, {4, RequiresHigherTest}};
    for (const auto &attempt : cases) {
        const uint8_t cap = attempt.cap;
        endpoints[0] = Endpoint{};
        TurboLink link(send<0>, idle, setRate<0>, report<0>);
        const auto receive = [&](const Bytes &frame, uint32_t now) {
            for (auto byte : frame) link.observeDinByte(byte, now);
            assert(link.receive(frame.data(), frame.size(), now));
        };
        link.setLimit(cap, 0);
        link.tick(0);
        assert(endpoints[0].tx == message(0, {0x10}));
        endpoints[0].tx.clear();
        receive(answer, 4);
        link.tick(4);
        assert(endpoints[0].tx == message(0, {0x12, cap, cap}));
        if (attempt.reply == RequiresHigherTest) {
            endpoints[0].tx.clear();
            for (unsigned now = 5; now <= 410; ++now) link.tick(now);
            assert(endpoints[0].tx == message(0, {0x10}));
            endpoints[0].tx.clear();
            receive(answer, 411);
            link.tick(411);
            assert(endpoints[0].tx == message(0, {0x12, 7, 4}));
            continue;
        }
        receive(message(0, {0x13}), 8);
        link.tick(8);
        assert(endpoints[0].rate == cap);
        receive(message(0, {0x15, 0x55, 0x55, 0x55, 0x55, 0, 0, 0, 0}), 12);
        link.tick(12);
        if (attempt.reply == MissingResult2) {
            endpoints[0].tx.clear();
            link.tick(60);
            link.tick(61);
            assert(endpoints[0].rate == cap && endpoints[0].tx == message(0, {0x12, 1, 1}));
            link.tick(62);
            link.tick(413);
            assert(link.failed() && endpoints[0].rate == 1 && !link.busy());
            continue;
        }
        receive(message(0, {0x17}), 16);
        endpoints[0].tx.clear();
        link.tick(26);
        assert(endpoints[0].tx == Bytes{0xfe});
        endpoints[0].tx.clear();
        if (attempt.reply == SensingPeer) {
            link.observeDinByte(0xfe, 30);
            assert(link.speed() == cap && !link.busy());
            for (unsigned now = 31; now <= 700; ++now) link.tick(now);
            assert(link.failed() && link.speed() == 1);
            continue;
        }
        link.tick(327);
        assert(endpoints[0].rate == cap && endpoints[0].tx == message(0, {0x10}));
        receive(message(0, {0x11, 0x7f, 1, 0x0f, 0}), 329);
        link.observeDinByte(0xfe, 330);
        assert(link.pausePayload() && endpoints[0].reports == 0);
        if (attempt.reply == MissingFinalAnswer) {
            for (unsigned now = 331; now <= 950; ++now) link.tick(now);
            assert(link.failed() && !link.busy() && endpoints[0].reported == 1);
            continue;
        }
        receive(answer, 331);
        endpoints[0].tx.clear();
        for (unsigned now = 332; now <= 1000; ++now) link.tick(now);
        assert(!link.pausePayload() && link.speed() == cap);
        for (auto byte : endpoints[0].tx) assert(byte == 0xfe);
        endpoints[0].tx.clear();
        link.toggle(1001);
        link.tick(1001);
        assert(endpoints[0].rate == cap && endpoints[0].tx == message(0, {0x12, 1, 1}));
        link.tick(1002);
        link.tick(1353);
        assert(link.speed() == 1 && endpoints[0].rate == 1 && !link.busy());
    }
}

void testNoFinalRateConfirmationRetriesLower() {
    for (auto &endpoint : endpoints) endpoint = Endpoint{};
    TurboLink initiator(send<0>, idle, setRate<0>, report<0>);
    TurboLink responder(send<1>, idle, setRate<1>, report<1>);
    initiator.setLimit(7, 0);
    bool lostConfirmation = false;
    for (uint32_t now = 0; now < 1300; ++now) {
        initiator.tick(now);
        responder.tick(now);
        deliver(0, 1, responder, now);
        if (endpoints[1].rate == 7 && endpoints[1].tx == Bytes{0xfe}) {
            lostConfirmation = true;
            endpoints[1].tx.clear();
        } else {
            deliver(1, 0, initiator, now);
        }
        assert(initiator.speed() != 7);
    }
    assert(lostConfirmation && initiator.limit() == 7);
    assert(initiator.speed() == 4 && responder.speed() == 4);
}

int main() {
    testForwarding();
    testControlIsolation();
    testRealtimeInsideControlPrefix();
    testInterruptedMidi();
    testIndependentTurboAndFallback();
    testCancelAfterPeerAcknowledges();
    testTwoDifferentRates();
    testIndependentFinalRateConfirmation();
    testMachinedrumUpgradeReceiver();
    testFastTestSerial();
    testSimultaneousRequestsSettleAndCanBeRetried();
    testNoFinalRateConfirmationRetriesLower();
}
