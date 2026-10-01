// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "z2s/backend.h"
#include "z2s/channel.h"
#include "z2s/identity_store.h"
#include "z2s/protocol_info.h"
#include "z2s/transport.h"

namespace z2s {

struct SuplaServerConfig {
  std::string server;
  int port = 2016;
  std::string email;
  int protoVersion = kDefaultProtoVersion;
  int activityTimeoutS = 120;  // requested server activity timeout
  std::string softVersion = "z2s " Z2S_VERSION;
};

// One ZigBee device registered in Supla as a separate device: owns its own
// connection (TLS), GUID/AuthKey and SRPC protocol instance.
class SuplaSession {
 public:
  enum class State { Idle, Connecting, Registering, Registered, Disabled };

  // channelNumber, command -> true when the command was accepted.
  using CommandHandler =
      std::function<bool(int channelNumber, const ChannelCommand &command)>;

  SuplaSession(const SuplaServerConfig &config, TransportFactory *factory);
  ~SuplaSession();
  SuplaSession(const SuplaSession &) = delete;
  SuplaSession &operator=(const SuplaSession &) = delete;

  // Sets identity, device name and channel layout. When the layout changes
  // on a registered session, the device re-registers.
  void configure(const DeviceIdentity &identity,
                 const DeviceDescriptor &descriptor);

  // Marks which channels of the layout are currently provided by the backend.
  void setChannelPresent(int channelNumber, bool present);
  // Turn-on timer of the device behind the channel (ChannelSpec::countdown*).
  // A change on a registered session makes the device register again, since
  // it changes the declared channel flags and functions.
  void setChannelCountdown(int channelNumber, uint32_t stepMs, uint32_t maxMs);
  // Actions a button channel can report (ChannelSpec::actionCaps); a change
  // on a registered session makes the device register again.
  void setChannelActionCaps(int channelNumber, uint32_t caps);
  // Reports a button action (SUPLA_ACTION_CAP_*) to the server. Dropped when
  // the session is not registered: a late action would surprise the user.
  void triggerAction(int channelNumber, uint32_t action);
  void setChannelState(int channelNumber, const ChannelState &state);
  void setDeviceOnline(bool online);
  // Battery and link quality, reported in the state of every channel.
  void setDeviceHealth(const DeviceHealth &update);
  void setCommandHandler(CommandHandler handler) {
    handler_ = std::move(handler);
  }

  // Connection lifecycle, driven by the Gateway.
  bool wantsToConnect(uint64_t nowMs) const;
  void startConnect(uint64_t nowMs);
  void iterate(uint64_t nowMs);
  // Disconnects and stops reconnecting (device removed from backend).
  void disable();
  void enable();
  // A suspended session is disconnected from the server and does not
  // reconnect: the ZigBee device is unreachable, so in Supla it looks like a
  // device that was switched off. Its identity and channel values are kept.
  void setSuspended(bool suspended);
  bool isSuspended() const { return suspended_; }

  State state() const { return state_; }
  bool isRegistered() const { return state_ == State::Registered; }
  bool isConnecting() const {
    return state_ == State::Connecting || state_ == State::Registering;
  }
  int fd() const;
  bool hasPendingWrite() const;
  const std::string &deviceId() const { return deviceId_; }
  const std::string &name() const { return name_; }
  int lastResultCode() const { return lastResultCode_; }
  const ChannelState *channelState(int channelNumber) const;

  // Internal SRPC callbacks (public for C linkage helpers).
  int onDataRead(void *buf, int count);
  int onDataWrite(void *buf, int count);
  void onRemoteCall();

 private:
  struct Channel {
    ChannelSpec spec;
    ChannelState state;
    bool present = false;
    char sentValue[8] = {};
    bool sentOffline = true;
    bool sentOnce = false;
    // Turn-on timer started from Supla: when it ends (0 = none) and whether
    // its state still has to be reported to the server.
    bool captionPending = false;
    uint64_t timerEndMs = 0;
    int32_t timerSenderId = 0;
    bool timerReportPending = false;
    // ElectricityMeter: last extended value sent and when.
    bool extSentOnce = false;
    double extSentEnergy = NAN;
    uint64_t extSentMs = 0;
    char extSentValue[sizeof(TSuplaChannelExtendedValue)] = {};
    // Config exchange (GeneralPurposeMeasurement, Thermostat): the server
    // sends the channel config after registration (and after changes in
    // Cloud); when the part owned by the device (defaults, read-only and
    // hidden parameters) differs, the gateway sends it back with the user's
    // settings unchanged.
    bool configFinished = false;
    bool configPending = false;
    int configAttempts = 0;
    // Thermostat: values set from Supla that the device has not confirmed
    // yet (NaN = confirmed), until expectedUntilMs.
    ChannelState expected;
    uint64_t expectedUntilMs = 0;
    char configData[SUPLA_CHANNEL_CONFIG_MAXSIZE] = {};
    uint16_t configSize = 0;
    // Thermostat: whether the server sent its weekly schedule after
    // registration; without one, a default schedule is sent.
    bool scheduleReceived = false;
    bool schedulePending = false;
  };

  void disconnect();
  void scheduleReconnect(uint64_t nowMs, uint64_t delayMs);
  void scheduleReconnectAfterFailure(uint64_t nowMs);
  void sendRegistration();
  void handleRegisterResult(int resultCode, int activityTimeout);
  void reportTimer(int channelNumber, uint64_t endMs, int32_t senderId);
  void pushTimerStates();
  void pushCaptions();
  void pushElectricityMeters();
  void pushChannelStates();
  void sendChannelState(int channelNumber, int32_t receiverId);
  void sendRelatedMeterConfig();
  void handleChannelConfig(const TSD_ChannelConfig &config);
  void pushChannelConfigs();
  bool handleThermostatValue(int channelNumber, const char *value,
                             uint32_t durationMs);
  void handleNewValue(int channelNumber, const char *value, uint32_t durationMs,
                      int senderId, bool fromGroup);
  bool isChannelOffline(const Channel &channel) const;
  void pushChannelValues(bool force);
  void ping(uint64_t nowMs);
  uint32_t jitter(uint32_t maxMs);

  SuplaServerConfig config_;
  TransportFactory *factory_;
  std::unique_ptr<Transport> transport_;
  void *srpc_ = nullptr;
  CommandHandler handler_;

  std::string deviceId_;
  std::string name_;
  std::string manufacturer_;
  std::string model_;
  std::array<uint8_t, 16> guid_{};
  std::array<uint8_t, 16> authKey_{};
  std::vector<Channel> channels_;
  bool deviceOnline_ = true;
  bool suspended_ = false;
  bool layoutChanged_ = false;
  // The relay's related meter is declared once per layout (see
  // sendRelatedMeterConfig()).
  bool relatedMeterSent_ = false;
  int batteryPowered_ = -1;
  DeviceHealth health_;
  // The battery changed (or the device registered): the state of all
  // channels has to be sent. Link quality alone is sent only on request.
  bool channelStatePending_ = false;

  State state_ = State::Idle;
  uint64_t nowMs_ = 0;
  uint64_t stateSinceMs_ = 0;
  uint64_t nextConnectMs_ = 0;
  uint64_t lastResponseMs_ = 0;
  uint64_t lastSentMs_ = 0;
  uint64_t lastPingMs_ = 0;
  int activityTimeoutS_ = 30;
  int failureCount_ = 0;
  int lastResultCode_ = 0;
  bool writeFailed_ = false;
  bool closeRequested_ = false;
  uint32_t rngState_ = 1;
};

const char *resultCodeToString(int resultCode);

}  // namespace z2s
