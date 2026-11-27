#pragma once

#include <stdint.h>

// TurboMIDI control on the DIN link. The callbacks are serviced by the main
// loop. sendDin must enqueue the entire buffer or leave the queue unchanged.
class TurboLink {
 public:
  static constexpr uint8_t normalSpeed = 1;
  static constexpr uint8_t maximumSpeed = 7;
  static constexpr uint8_t fastTestSpeed = 8;
  static constexpr uint8_t fastTestCapability = 1 << (fastTestSpeed - 2);

  TurboLink(bool (*sendDin)(const uint8_t*, uint8_t), bool (*dinTxIdle)(),
            void (*setDinRate)(uint8_t), void (*notifySpeed)(uint8_t),
            bool (*fastTestAvailable)() = nullptr);

  void toggle(uint32_t now);
  void setLimit(uint8_t speedCode, uint32_t now);
  bool receive(const uint8_t* frame, uint8_t size, uint32_t now);
  void observeDinByte(uint8_t byte, uint32_t now);
  // Buffered data can refresh liveness without confirming the final baud rate.
  void noteDinActivity(uint32_t now) { lastDinByte_ = now; }
  // True when a framing error is contained within negotiation.
  bool reportTransportError(uint32_t now, bool framingOnly = false);
  void tick(uint32_t now);

  // During a transition, speed is the last settled rate reported to the host.
  uint8_t speed() const { return speed_; }
  uint8_t limit() const { return hostLimit_; }
  bool mayUseFastTest() const { return state_ == State::SendRequest && attemptLimit_ == maximumSpeed; }
  bool selectedFastTest() const { return testSpeed_ == fastTestSpeed; }
  bool busy() const;
  bool pausePayload() const { return busy() && state_ != State::WaitNegotiation; }
  bool recovering() const { return state_ == State::Recovering; }
  bool failed() const { return failed_; }

 private:
  enum class State : uint8_t {
    Normal,
    SendRequest,
    WaitAnswer,
    SendNegotiation,
    WaitAck,
    SendTest1,
    WaitResult1,
    SendTest2,
    WaitResult2,
    SendAnswer,
    WaitNegotiation,
    SendAck,
    DrainAck,
    WaitTest1,
    SendResult1,
    WaitTest2,
    SendResult2,
    DrainResult2,
    Settling,
    WaitPeer,
    WaitUpgradeAnswer,
    Active,
    Recovering,
  };

  void enter(State state, uint32_t now);
  void stop(uint32_t now, uint8_t restartLimit, bool failed, bool waitForPeer = true);
  void failNegotiation(uint32_t now, bool retry);
  void recover(uint32_t now, uint16_t quietMs);
  void setWireSpeed(uint8_t speedCode);
  void publishSpeed(uint8_t speedCode, bool force = false);
  void chooseSpeeds(const uint8_t* answer, uint32_t now);
  bool sendCommand(uint8_t command, const uint8_t* payload = nullptr,
                   uint8_t length = 0, uint8_t padding = 0);
  uint8_t byteTimeout() const;

  bool (*sendDin_)(const uint8_t*, uint8_t);
  bool (*dinTxIdle_)();
  void (*setDinRate_)(uint8_t);
  void (*notifySpeed_)(uint8_t);
  bool (*fastTestAvailable_)();

  State state_ = State::Normal;
  uint32_t stateSince_ = 0;
  uint32_t lastFrameByte_ = 0;
  uint32_t lastDinByte_ = 0;
  uint32_t lastSenseSent_ = 0;
  uint8_t hostLimit_ = maximumSpeed;
  uint8_t attemptLimit_ = maximumSpeed;
  uint8_t restartLimit_ = 0;
  uint8_t testSpeed_ = normalSpeed;
  uint8_t finalSpeed_ = normalSpeed;
  uint8_t wireSpeed_ = normalSpeed;
  uint8_t speed_ = normalSpeed;
  uint8_t responderMask_ = 0;
  bool awaitingFrame_ = false;
  bool initiator_ = false;
  bool upgradeReceiver_ = false;
  uint8_t higherTestForRate_ = 0;
  bool recoveryDrained_ = false;
  uint16_t recoveryQuietMs_ = 0;
  bool retryRequest_ = false;
  bool failed_ = false;
};
