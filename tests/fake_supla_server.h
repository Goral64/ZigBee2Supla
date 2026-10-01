// SPDX-License-Identifier: GPL-2.0-or-later
// In-memory Supla server for tests. It parses and builds SRPC frames on its
// own (independently of srpc.c), so the wire format produced by the gateway
// is verified end to end.
#pragma once

#include <proto.h>

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "z2s/transport.h"

namespace z2s::test {

struct Registration {
  std::string email;
  std::string name;
  std::string softVer;
  std::string serverName;
  std::array<uint8_t, 16> guid{};
  std::array<uint8_t, 16> authKey{};
  std::vector<TDS_SuplaDeviceChannel_E> channels;
};

struct ValueChange {
  int channel;
  char value[8];
  int offline;
};

struct CaptionRequest {
  int channel;
  std::string caption;
};

struct TimerReport {
  int channel;
  uint32_t remainingMs;
  int targetValue;
  int senderId;
};

// EV_TYPE_ELECTRICITY_METER_MEASUREMENT_V3, phase 1.
struct MeterReport {
  int channel;
  uint64_t energy;  // 0.00001 kWh
  int measuredValues;
  int voltage;  // 0.01 V
  int current;  // 0.001 A
  int power;    // 0.00001 W
};

struct ChannelConfigRequest {
  int channel;
  int func;
  int configType;
  TChannelConfig_PowerSwitch powerSwitch;
  TChannelConfig_GeneralPurposeMeasurement gpm;
  TChannelConfig_HVAC hvac;
};

struct ChannelConfigResult {
  int channel;
  int configType;
  int result;
};

struct ChannelStateReport {
  int channel;
  int receiverId;
  TDSC_ChannelState state;
};

struct ActionReport {
  int channel;
  int action;
};

struct SetResult {
  int channel;
  int senderId;
  int success;
};

class FakeSuplaServer;

struct FakeConnection {
  FakeSuplaServer *server = nullptr;
  unsigned char version = 0;  // protocol version used by the device
  unsigned int nextRrId = 1;
  std::deque<char> toServer;
  std::deque<char> toDevice;
  bool closed = false;
  std::vector<Registration> registrations;
  std::vector<ValueChange> values;
  std::vector<SetResult> setResults;
  std::vector<TimerReport> timers;
  std::vector<CaptionRequest> captions;
  std::vector<MeterReport> meters;
  std::vector<ChannelConfigRequest> configs;
  std::vector<ChannelStateReport> channelStates;
  std::vector<ActionReport> actions;
  // Results of configs sent by the server (SUPLA_DS_CALL_SET_CHANNEL_CONFIG_
  // RESULT).
  std::vector<ChannelConfigResult> configResults;
  int pings = 0;
  int activityTimeoutRequests = 0;
};

class FakeSuplaServer {
 public:
  FakeSuplaServer();
  ~FakeSuplaServer();

  std::shared_ptr<FakeConnection> accept();
  // Processes all pending packets on all connections.
  void iterate();
  void closeAll();

  // A client asks for the state of a channel (channel information in the
  // app).
  void sendGetChannelState(FakeConnection *conn, int channel, int senderId);
  // CHANNEL_SET_VALUE with a raw 8-byte value (e.g. THVACValue).
  void sendSetValueRaw(FakeConnection *conn, int channel, int senderId,
                       const char value[8]);
  void sendSetValue(FakeConnection *conn, int channel, int senderId, bool on,
                    uint32_t durationMs = 0);

  int registerResultCode = SUPLA_RESULTCODE_TRUE;
  int activityTimeout = 120;
  int channelConfigResult = SUPLA_CONFIG_RESULT_TRUE;
  // General purpose measurement configs stored by the "server", by channel
  // number (one device per test). Like the real server, channels with
  // SUPLA_CHANNEL_FLAG_RUNTIME_CHANNEL_CONFIG_UPDATE get their config after
  // registration (empty when none is stored) followed by
  // CHANNEL_CONFIG_FINISHED, and again after each SET_CHANNEL_CONFIG.
  std::map<int, TChannelConfig_GeneralPurposeMeasurement> gpmConfigs;
  std::map<int, TChannelConfig_HVAC> hvacConfigs;
  // Weekly schedules (channels with SUPLA_CHANNEL_FLAG_WEEKLY_SCHEDULE).
  std::map<int, TChannelConfig_WeeklySchedule> schedules;
  bool refuseConnections = false;
  std::vector<std::shared_ptr<FakeConnection>> connections;
  int acceptedCount = 0;
};

class FakeTransportFactory : public TransportFactory {
 public:
  explicit FakeTransportFactory(FakeSuplaServer *server) : server_(server) {}
  std::unique_ptr<Transport> create() override;

 private:
  FakeSuplaServer *server_;
};

}  // namespace z2s::test
