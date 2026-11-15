#include "TurboLink.h"
#include "MidiStream.h"

#include <string.h>

namespace {
// DIN negotiation follows the TurboMIDI protocol. See THIRD_PARTY_NOTICES.md.
constexpr uint8_t header[] = {0xF0, 0x00, 0x20, 0x3C, 0x00, 0x00};
constexpr uint8_t pattern[] = {0x55, 0x55, 0x55, 0x55, 0, 0, 0, 0};
constexpr uint8_t activeSense = 0xFE;
constexpr uint8_t capabilityBit(uint8_t speed) { return 1u << (speed - 2); }

enum Command : uint8_t {
  SpeedRequest = 0x10,
  SpeedAnswer = 0x11,
  SpeedNegotiation = 0x12,
  SpeedAck = 0x13,
  SpeedTest1 = 0x14,
  SpeedResult1 = 0x15,
  SpeedTest2 = 0x16,
  SpeedResult2 = 0x17,
};

constexpr uint8_t localCapabilities = capabilityBit(2) | capabilityBit(4) |
                                      capabilityBit(TurboLink::maximumSpeed);
constexpr uint16_t senseInterval = 150;
constexpr uint16_t senseTimeout = 300;
constexpr uint16_t recoveryInterval = 350;
constexpr uint16_t restartRetryInterval = 1200;
constexpr uint8_t settlingInterval = 10;

uint8_t highestSpeed(uint8_t mask, uint8_t limit) {
  for (uint8_t speed = limit; speed > TurboLink::normalSpeed; --speed) {
    if (mask & capabilityBit(speed)) return speed;
  }
  return TurboLink::normalSpeed;
}

uint8_t allowedMask(uint8_t limit) {
  return localCapabilities & ((1u << (limit - 1)) - 1);
}
}

TurboLink::TurboLink(bool (*sendDin)(const uint8_t*, uint8_t), bool (*dinTxIdle)(),
                     void (*setDinRate)(uint8_t), void (*notifySpeed)(uint8_t),
                     bool (*fastTestAvailable)())
    : sendDin_(sendDin), dinTxIdle_(dinTxIdle), setDinRate_(setDinRate),
      notifySpeed_(notifySpeed), fastTestAvailable_(fastTestAvailable) {}

bool TurboLink::busy() const {
  return state_ != State::Normal && state_ != State::Active;
}

void TurboLink::enter(State state, uint32_t now) {
  if (state == State::SendRequest) initiator_ = true;
  if (state == State::SendAnswer) initiator_ = false;
  state_ = state;
  stateSince_ = now;
  awaitingFrame_ = false;
}

void TurboLink::setWireSpeed(uint8_t speedCode) {
  if (wireSpeed_ == speedCode) return;
  setDinRate_(speedCode);
  wireSpeed_ = speedCode;
}

void TurboLink::publishSpeed(uint8_t speedCode, bool force) {
  if (speed_ == speedCode && !force) return;
  speed_ = speedCode;
  notifySpeed_(speed_);
}

void TurboLink::recover(uint32_t now, uint16_t quietMs) {
  recoveryDrained_ = false;
  recoveryQuietMs_ = quietMs;
  enter(State::Recovering, now);
}

void TurboLink::stop(uint32_t now, uint8_t restartLimit, bool failed, bool waitForPeer) {
  failed_ = failed;
  restartLimit_ = restartLimit;

  // A peer can switch speeds before its acknowledgement arrives.
  // Stop keepalives so the peer can time out.
  if (state_ == State::Recovering) return;
  const bool peerMayHaveSwitched = wireSpeed_ != normalSpeed ||
                                   state_ == State::WaitAck ||
                                   state_ == State::DrainAck;
  if (peerMayHaveSwitched) {
    recover(now, waitForPeer ? recoveryInterval : 0);
  } else if (restartLimit_ > normalSpeed) {
    attemptLimit_ = restartLimit_;
    enter(State::SendRequest, now);
  } else {
    enter(State::Normal, now);
    publishSpeed(normalSpeed, true);
  }
}

void TurboLink::failNegotiation(uint32_t now, bool retry) {
  const uint8_t retryLimit = retry && finalSpeed_ > 2 ? finalSpeed_ - 1 : 0;
  stop(now, retryLimit, retryLimit == 0);
}

void TurboLink::toggle(uint32_t now) {
  if (state_ == State::Recovering && restartLimit_ == 0) {
    if (hostLimit_ == normalSpeed) hostLimit_ = maximumSpeed;
    restartLimit_ = hostLimit_;
    failed_ = false;
    return;
  }
  if (state_ != State::Normal) {
    stop(now, 0, false);
    return;
  }
  if (hostLimit_ == normalSpeed) hostLimit_ = maximumSpeed;
  failed_ = false;
  attemptLimit_ = hostLimit_;
  enter(State::SendRequest, now);
}

void TurboLink::setLimit(uint8_t speedCode, uint32_t now) {
  const uint8_t previousLimit = hostLimit_;
  if (speedCode > maximumSpeed) speedCode = maximumSpeed;
  hostLimit_ = highestSpeed(localCapabilities, speedCode);
  if (hostLimit_ == normalSpeed) {
    stop(now, 0, false);
    return;
  }
  if (state_ == State::Normal) {
    failed_ = false;
    attemptLimit_ = hostLimit_;
    enter(State::SendRequest, now);
  } else if (state_ == State::Recovering && restartLimit_ == 0) {
    restartLimit_ = hostLimit_;
    failed_ = false;
  } else if ((hostLimit_ != previousLimit && busy()) ||
             (!busy() && speed_ != hostLimit_)) {
    stop(now, hostLimit_, false);
  } else if (!busy()) {
    publishSpeed(speed_, true);
  }
}

bool TurboLink::sendCommand(uint8_t command, const uint8_t* payload,
                            uint8_t length, uint8_t padding) {
  uint8_t frame[32] = {};
  memcpy(frame + padding, header, sizeof(header));
  frame[padding + sizeof(header)] = command;
  if (length) memcpy(frame + padding + sizeof(header) + 1, payload, length);
  const uint8_t size = padding + sizeof(header) + length + 2;
  frame[size - 1] = 0xF7;
  return sendDin_(frame, size);
}

void TurboLink::chooseSpeeds(const uint8_t* answer, uint32_t now) {
  const uint8_t common = answer[0] & allowedMask(attemptLimit_);
  const uint8_t certified = answer[2] & common;
  const uint8_t highestTest = attemptLimit_ == maximumSpeed &&
                              (common & capabilityBit(maximumSpeed)) &&
                              (answer[0] & capabilityBit(fastTestSpeed))
      ? fastTestSpeed : highestSpeed(common, attemptLimit_);

  // An equal pair requires certification. A faster common test rate can
  // qualify an uncertified final rate. 10× is available only for tests.
  for (uint8_t candidate = attemptLimit_; candidate > normalSpeed; --candidate) {
    const uint8_t bit = capabilityBit(candidate);
    if (!(common & bit)) continue;
    if (!(certified & bit) && highestTest <= candidate) continue;
    finalSpeed_ = candidate;
    testSpeed_ = (certified & bit) ? candidate : highestTest;
    enter(State::SendNegotiation, now);
    return;
  }
  failNegotiation(now, false);
}

bool TurboLink::receive(const uint8_t* frame, uint8_t size, uint32_t now) {
  if (size < sizeof(header) + 2 || memcmp(frame, header, sizeof(header)) != 0) {
    return false;
  }
  const uint8_t command = frame[sizeof(header)];
  if (command < SpeedRequest || command > SpeedResult2) return false;
  if (size != turboDinFrameLength(command) || frame[size - 1] != 0xF7) {
    return false;
  }
  const uint8_t* payload = frame + sizeof(header) + 1;

  switch (command) {
    case SpeedRequest:
      if (state_ == State::Normal || state_ == State::WaitNegotiation ||
          state_ == State::SendRequest || state_ == State::WaitAnswer) {
        failed_ = false;
        enter(State::SendAnswer, now);
      }
      break;
    case SpeedAnswer:
      if (state_ == State::WaitAnswer) {
        retryRequest_ = false;
        chooseSpeeds(payload, now);
      }
      break;
    case SpeedNegotiation:
      if (state_ != State::WaitNegotiation) break;
      if (payload[0] == normalSpeed && payload[1] == normalSpeed) {
        stop(now, 0, false);
      } else if (payload[1] > normalSpeed && payload[0] >= payload[1] &&
                 payload[0] <= fastTestSpeed && payload[1] <= hostLimit_ &&
                 (responderMask_ & capabilityBit(payload[0])) &&
                 (responderMask_ & capabilityBit(payload[1]))) {
        testSpeed_ = payload[0];
        finalSpeed_ = payload[1];
        enter(State::SendAck, now);
      } else {
        failNegotiation(now, false);
      }
      break;
    case SpeedAck:
      if (state_ == State::WaitAck) enter(State::SendTest1, now);
      break;
    case SpeedTest1:
      if (state_ == State::WaitTest1) {
        if (memcmp(payload, pattern, sizeof(pattern)) == 0) {
          enter(State::SendResult1, now);
        } else {
          failNegotiation(now, false);
        }
      }
      break;
    case SpeedResult1:
      if (state_ == State::WaitResult1) {
        if (memcmp(payload, pattern, sizeof(pattern)) == 0) {
          enter(State::SendTest2, now);
        } else {
          failNegotiation(now, true);
        }
      }
      break;
    case SpeedTest2:
      if (state_ == State::WaitTest2) enter(State::SendResult2, now);
      break;
    case SpeedResult2:
      if (state_ == State::WaitResult2) {
        // The peer sends this result at the test rate and switches after F7.
        setWireSpeed(finalSpeed_);
        enter(State::Settling, now);
      }
      break;
  }
  return true;
}

uint8_t TurboLink::byteTimeout() const {
  switch (state_) {
    case State::WaitAnswer:
    case State::WaitAck:
    case State::WaitResult1:
    case State::WaitResult2:
      return 45;
    case State::WaitNegotiation:
    case State::WaitTest1:
    case State::WaitTest2:
      return 23;
    default:
      return 0;
  }
}

void TurboLink::observeDinByte(uint8_t byte, uint32_t now) {
  noteDinActivity(now);
  if (state_ == State::WaitPeer && byte >= 0x80 && byte != 0xf7) {
    failed_ = false;
    retryRequest_ = false;
    enter(State::Active, now);
    publishSpeed(finalSpeed_, true);
    return;
  }
  if (!byteTimeout() || byte >= 0xF8) return;
  if (byte == 0xF0) awaitingFrame_ = true;
  if (!awaitingFrame_) return;
  lastFrameByte_ = now;
  if (byte >= 0x80 && byte != 0xF0) awaitingFrame_ = false;
}

bool TurboLink::reportTransportError(uint32_t now, bool framingOnly) {
  if (framingOnly && ((state_ == State::WaitAnswer && retryRequest_) ||
      state_ == State::WaitAck || state_ == State::Settling || state_ == State::WaitPeer)) {
    // Old-rate bytes can arrive while the peer changes its UART. Discard
    // the partial frame and let the ordinary reply deadline apply.
    awaitingFrame_ = false;
    return true;
  }
  if (state_ == State::WaitResult1 || state_ == State::WaitResult2 ||
      (state_ == State::SendTest2 && wireSpeed_ == fastTestSpeed)) {
    failNegotiation(now, true);
    return framingOnly;
  }
  if (framingOnly && (state_ == State::WaitTest1 || state_ == State::WaitTest2 ||
      state_ == State::SendResult1 || state_ == State::SendResult2 ||
      state_ == State::DrainResult2)) {
    failNegotiation(now, false);
    return true;
  } else if (!recovering()) {
    stop(now, 0, true);
  }
  return false;
}

void TurboLink::tick(uint32_t now) {
  const uint32_t elapsed = now - stateSince_;
  switch (state_) {
    case State::Normal:
      return;
    case State::SendRequest:
      if (dinTxIdle_() && sendCommand(SpeedRequest)) enter(State::WaitAnswer, now);
      else if (elapsed > 120) failNegotiation(now, false);
      return;
    case State::SendNegotiation: {
      const uint8_t rates[] = {testSpeed_, finalSpeed_};
      if (sendCommand(SpeedNegotiation, rates, sizeof(rates))) enter(State::WaitAck, now);
      else if (elapsed > 45) failNegotiation(now, false);
      return;
    }
    case State::SendTest1:
      if (dinTxIdle_()) {
        setWireSpeed(testSpeed_);
        if (sendCommand(SpeedTest1, pattern, sizeof(pattern), 16)) {
          enter(State::WaitResult1, now);
          return;
        }
      }
      if (elapsed > 45) failNegotiation(now, true);
      return;
    case State::SendTest2:
      if (sendCommand(SpeedTest2)) enter(State::WaitResult2, now);
      else if (elapsed > 45) failNegotiation(now, true);
      return;
    case State::SendAnswer: {
      const uint8_t certified = allowedMask(hostLimit_);
      responderMask_ = certified;
      if (hostLimit_ == maximumSpeed && fastTestAvailable_ && fastTestAvailable_()) {
        responderMask_ |= fastTestCapability;
      }
      const uint8_t response[] = {responderMask_, 0, certified, 0};
      if (sendCommand(SpeedAnswer, response, sizeof(response))) {
        enter(State::WaitNegotiation, now);
      } else if (elapsed > 23) {
        failNegotiation(now, false);
      }
      return;
    }
    case State::SendAck:
      if (sendCommand(SpeedAck)) enter(State::DrainAck, now);
      else if (elapsed > 23) failNegotiation(now, false);
      return;
    case State::DrainAck:
      if (dinTxIdle_()) {
        setWireSpeed(testSpeed_);
        enter(State::WaitTest1, now);
      } else if (elapsed > 23) {
        failNegotiation(now, false);
      }
      return;
    case State::SendResult1:
      if (sendCommand(SpeedResult1, pattern, sizeof(pattern))) enter(State::WaitTest2, now);
      else if (elapsed > 23) failNegotiation(now, false);
      return;
    case State::SendResult2:
      if (sendCommand(SpeedResult2)) enter(State::DrainResult2, now);
      else if (elapsed > 23) failNegotiation(now, false);
      return;
    case State::DrainResult2:
      if (dinTxIdle_()) {
        setWireSpeed(finalSpeed_);
        enter(State::Settling, now);
      } else if (elapsed > 23) {
        failNegotiation(now, false);
      }
      return;
    case State::Settling:
      if (elapsed >= settlingInterval && sendDin_(&activeSense, 1)) {
        lastSenseSent_ = now;
        // Confirm the peer has reached the final baud rate before reporting it.
        enter(State::WaitPeer, now);
      } else if (elapsed > 45) {
        failNegotiation(now, false);
      }
      return;
    case State::WaitPeer:
      if (elapsed > senseTimeout) {
        failNegotiation(now, initiator_);
      } else if (now - lastSenseSent_ >= senseInterval && sendDin_(&activeSense, 1)) {
        lastSenseSent_ = now;
      }
      return;
    case State::Active:
      if (now - lastDinByte_ > senseTimeout) {
        stop(now, 0, true, false);
      } else if (now - lastSenseSent_ >= senseInterval && sendDin_(&activeSense, 1)) {
        lastSenseSent_ = now;
      }
      return;
    case State::Recovering:
      if (!recoveryDrained_) {
        if (!dinTxIdle_()) return;
        // Let the peer's Active Sensing timeout expire before switching
        // the UART. It may still be transmitting at the negotiated rate.
        stateSince_ = now;
        recoveryDrained_ = true;
        if (recoveryQuietMs_) return;
      } else if (elapsed < recoveryQuietMs_) {
        return;
      }
      // Some peers miss the first 1× request even after the Active Sensing
      // guard. Allow one retry on the next handshake after a settled link.
      if (speed_ != normalSpeed) retryRequest_ = true;
      setWireSpeed(normalSpeed);
      publishSpeed(normalSpeed, true);
      if (restartLimit_ > normalSpeed) {
        attemptLimit_ = restartLimit_;
        enter(State::SendRequest, now);
      } else {
        enter(State::Normal, now);
      }
      return;
    default:
      break;
  }

  const uint8_t timeout = byteTimeout();
  if (awaitingFrame_ && now - lastFrameByte_ <= timeout && elapsed <= 400) return;
  if (!awaitingFrame_ && elapsed <= timeout) return;

  if (state_ == State::WaitAnswer && retryRequest_) {
    retryRequest_ = false;
    restartLimit_ = attemptLimit_;
    // Give a slow peer time to finish falling back to 1× before resending.
    recover(now, restartRetryInterval);
  } else if (state_ == State::WaitNegotiation) {
    // Both ends can answer at the same time and then wait for the other to
    // choose a rate. Publish the resulting 1× state so a host can retry.
    stop(now, 0, false);
  } else {
    const bool retry = state_ == State::WaitAck || state_ == State::WaitResult1 ||
                       state_ == State::WaitResult2;
    failNegotiation(now, retry);
  }
}
