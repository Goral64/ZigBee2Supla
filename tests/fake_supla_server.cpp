// SPDX-License-Identifier: GPL-2.0-or-later

#include "fake_supla_server.h"

#include <cstring>
#include <stdexcept>

namespace z2s::test {

namespace {

constexpr size_t kTagSize = SUPLA_TAG_SIZE;
// tag + version + rr_id + call_id + data_size
constexpr size_t kHeaderSize = kTagSize + 1 + 4 + 4 + 4;
const char kTag[kTagSize] = {'S', 'U', 'P', 'L', 'A'};

uint32_t readU32(const std::deque<char> &q, size_t pos) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; i--) {
    v = (v << 8) | static_cast<uint8_t>(q[pos + i]);
  }
  return v;
}

void appendU32(std::deque<char> *q, uint32_t v) {
  for (int i = 0; i < 4; i++)
    q->push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

void sendPacket(FakeConnection *conn, uint32_t callId, const void *data,
                size_t size) {
  auto &q = conn->toDevice;
  q.insert(q.end(), kTag, kTag + kTagSize);
  q.push_back(static_cast<char>(conn->version ? conn->version : 25));
  appendU32(&q, conn->nextRrId++);
  appendU32(&q, callId);
  appendU32(&q, static_cast<uint32_t>(size));
  const char *p = static_cast<const char *>(data);
  q.insert(q.end(), p, p + size);
  q.insert(q.end(), kTag, kTag + kTagSize);
}

template <typename T>
T copyPayload(const std::vector<char> &data) {
  T value;
  memset(&value, 0, sizeof(value));
  if (data.size() > sizeof(value)) {
    throw std::runtime_error("payload larger than struct");
  }
  memcpy(&value, data.data(), data.size());
  return value;
}

void sendStoredSchedule(FakeConnection *conn, int channel, int func) {
  TSDS_SetChannelConfig config = {};
  config.ChannelNumber = static_cast<unsigned char>(channel);
  config.Func = func;
  config.ConfigType = SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE;
  // Like the real server: nothing is sent when there is no schedule.
  auto it = conn->server->schedules.find(channel);
  if (it == conn->server->schedules.end()) return;
  config.ConfigSize = sizeof(it->second);
  memcpy(config.Config, &it->second, sizeof(it->second));
  sendPacket(conn, SUPLA_SD_CALL_SET_CHANNEL_CONFIG, &config,
             offsetof(TSDS_SetChannelConfig, Config) + config.ConfigSize);
}

void sendStoredConfig(FakeConnection *conn, int channel, int func) {
  TSDS_SetChannelConfig config = {};
  config.ChannelNumber = static_cast<unsigned char>(channel);
  config.Func = func;
  config.ConfigType = SUPLA_CONFIG_TYPE_DEFAULT;
  auto it = conn->server->gpmConfigs.find(channel);
  if (it != conn->server->gpmConfigs.end()) {
    config.ConfigSize = sizeof(it->second);
    memcpy(config.Config, &it->second, sizeof(it->second));
  }
  auto hvac = conn->server->hvacConfigs.find(channel);
  if (hvac != conn->server->hvacConfigs.end()) {
    config.ConfigSize = sizeof(hvac->second);
    memcpy(config.Config, &hvac->second, sizeof(hvac->second));
  }
  sendPacket(conn, SUPLA_SD_CALL_SET_CHANNEL_CONFIG, &config,
             offsetof(TSDS_SetChannelConfig, Config) + config.ConfigSize);
}

void handlePacket(FakeConnection *conn, uint32_t callId,
                  const std::vector<char> &data) {
  switch (callId) {
    case SUPLA_DS_CALL_REGISTER_DEVICE_G: {
      auto *r = new TDS_SuplaRegisterDevice_G;
      memset(r, 0, sizeof(*r));
      memcpy(r, data.data(), std::min(data.size(), sizeof(*r)));
      size_t expected = sizeof(TDS_SuplaRegisterDevice_G) -
                        sizeof(r->channels) +
                        r->channel_count * sizeof(TDS_SuplaDeviceChannel_E);
      if (data.size() != expected) {
        delete r;
        throw std::runtime_error("invalid REGISTER_DEVICE_G size");
      }
      Registration reg;
      reg.email = r->Email;
      reg.name = r->Name;
      reg.softVer = r->SoftVer;
      reg.serverName = r->ServerName;
      memcpy(reg.guid.data(), r->GUID, 16);
      memcpy(reg.authKey.data(), r->AuthKey, 16);
      reg.channels.assign(r->channels, r->channels + r->channel_count);
      conn->registrations.push_back(reg);
      delete r;

      TSD_SuplaRegisterDeviceResult_B result = {};
      result.result_code = conn->server->registerResultCode;
      result.activity_timeout =
          static_cast<unsigned char>(conn->server->activityTimeout);
      result.version = SUPLA_PROTO_VERSION;
      result.version_min = 1;
      result.channel_report_size = 0;
      sendPacket(conn, SUPLA_SD_CALL_REGISTER_DEVICE_RESULT_B, &result,
                 sizeof(result) - CHANNEL_REPORT_MAXSIZE);
      if (result.result_code != SUPLA_RESULTCODE_TRUE) break;
      for (const auto &ch : conn->registrations.back().channels) {
        if (!(ch.Flags & SUPLA_CHANNEL_FLAG_RUNTIME_CHANNEL_CONFIG_UPDATE)) {
          continue;
        }
        sendStoredConfig(conn, ch.Number, ch.Default);
        if (ch.Flags & SUPLA_CHANNEL_FLAG_WEEKLY_SCHEDULE) {
          sendStoredSchedule(conn, ch.Number, ch.Default);
        }
        TSD_ChannelConfigFinished finished = {};
        finished.ChannelNumber = ch.Number;
        sendPacket(conn, SUPLA_SD_CALL_CHANNEL_CONFIG_FINISHED, &finished,
                   sizeof(finished));
      }
      break;
    }
    case SUPLA_DS_CALL_DEVICE_CHANNEL_VALUE_CHANGED_C: {
      auto v = copyPayload<TDS_SuplaDeviceChannelValue_C>(data);
      ValueChange change;
      change.channel = v.ChannelNumber;
      memcpy(change.value, v.value, 8);
      change.offline = v.Offline;
      conn->values.push_back(change);
      break;
    }
    case SUPLA_DS_CALL_DEVICE_CHANNEL_EXTENDEDVALUE_CHANGED: {
      auto v = copyPayload<TDS_SuplaDeviceChannelExtendedValue>(data);
      if (v.value.type == EV_TYPE_TIMER_STATE_V1) {
        TTimerState_ExtendedValue timer;
        memcpy(&timer, v.value.value, sizeof(timer));
        conn->timers.push_back({v.ChannelNumber, timer.RemainingTimeMs,
                                timer.TargetValue[0], timer.SenderID});
      } else if (v.value.type == EV_TYPE_ELECTRICITY_METER_MEASUREMENT_V3) {
        TElectricityMeter_ExtendedValue_V3 em = {};
        constexpr size_t kOneMeasurement =
            sizeof(em) - sizeof(em.m) + sizeof(TElectricityMeter_Measurement);
        if (v.value.size != kOneMeasurement) {
          throw std::runtime_error("unexpected meter value size");
        }
        memcpy(&em, v.value.value, v.value.size);
        if (em.m_count != 1) throw std::runtime_error("unexpected m_count");
        conn->meters.push_back({v.ChannelNumber,
                                em.total_forward_active_energy[0],
                                em.measured_values, em.m[0].voltage[0],
                                em.m[0].current[0], em.m[0].power_active[0]});
      }
      break;
    }
    case SUPLA_DS_CALL_ACTIONTRIGGER: {
      auto a = copyPayload<TDS_ActionTrigger>(data);
      if (data.size() != sizeof(TDS_ActionTrigger)) {
        throw std::runtime_error("invalid ACTIONTRIGGER size");
      }
      conn->actions.push_back({a.ChannelNumber, a.ActionTrigger});
      break;
    }
    case SUPLA_DSC_CALL_CHANNEL_STATE_RESULT: {
      auto s = copyPayload<TDSC_ChannelState>(data);
      if (data.size() != sizeof(TDSC_ChannelState)) {
        throw std::runtime_error("invalid CHANNEL_STATE_RESULT size");
      }
      conn->channelStates.push_back({s.ChannelNumber, s.ReceiverID, s});
      break;
    }
    case SUPLA_DS_CALL_SET_CHANNEL_CONFIG: {
      auto c = copyPayload<TSDS_SetChannelConfig>(data);
      if (data.size() !=
          offsetof(TSDS_SetChannelConfig, Config) + c.ConfigSize) {
        throw std::runtime_error("invalid SET_CHANNEL_CONFIG size");
      }
      ChannelConfigRequest request = {};
      request.channel = c.ChannelNumber;
      request.func = c.Func;
      request.configType = c.ConfigType;
      if (c.ConfigSize == sizeof(TChannelConfig_PowerSwitch)) {
        memcpy(&request.powerSwitch, c.Config, c.ConfigSize);
      }
      bool stored =
          conn->server->channelConfigResult == SUPLA_CONFIG_RESULT_TRUE;
      // Configs of channels with RUNTIME_CHANNEL_CONFIG_UPDATE are sent back.
      bool echo = false;
      if (c.ConfigType == SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE) {
        if (c.ConfigSize != sizeof(TChannelConfig_WeeklySchedule)) {
          throw std::runtime_error("invalid weekly schedule size");
        }
        TChannelConfig_WeeklySchedule schedule;
        memcpy(&schedule, c.Config, sizeof(schedule));
        if (stored) conn->server->schedules[c.ChannelNumber] = schedule;
      } else if (c.ConfigSize ==
                 sizeof(TChannelConfig_GeneralPurposeMeasurement)) {
        echo = true;
        memcpy(&request.gpm, c.Config, c.ConfigSize);
        if (stored) conn->server->gpmConfigs[c.ChannelNumber] = request.gpm;
      }
      if (c.ConfigSize == sizeof(TChannelConfig_HVAC)) {
        echo = true;
        memcpy(&request.hvac, c.Config, c.ConfigSize);
        if (stored) conn->server->hvacConfigs[c.ChannelNumber] = request.hvac;
      }
      conn->configs.push_back(request);

      TSDS_SetChannelConfigResult result = {};
      result.Result =
          static_cast<unsigned char>(conn->server->channelConfigResult);
      result.ConfigType = c.ConfigType;
      result.ChannelNumber = c.ChannelNumber;
      sendPacket(conn, SUPLA_SD_CALL_SET_CHANNEL_CONFIG_RESULT, &result,
                 sizeof(result));
      if (echo) sendStoredConfig(conn, c.ChannelNumber, c.Func);
      if (c.ConfigType == SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE) {
        sendStoredSchedule(conn, c.ChannelNumber, c.Func);
      }
      break;
    }
    case SUPLA_DS_CALL_SET_CHANNEL_CONFIG_RESULT: {
      auto r = copyPayload<TSDS_SetChannelConfigResult>(data);
      conn->configResults.push_back({r.ChannelNumber, r.ConfigType, r.Result});
      break;
    }
    case SUPLA_DCS_CALL_SET_CHANNEL_CAPTION: {
      auto c = copyPayload<TDCS_SetCaption>(data);
      size_t size = data.size() - offsetof(TDCS_SetCaption, Caption);
      if (c.CaptionSize != size || size == 0 || c.Caption[size - 1] != '\0') {
        throw std::runtime_error("invalid SET_CHANNEL_CAPTION");
      }
      conn->captions.push_back({c.ChannelNumber, c.Caption});
      break;
    }
    case SUPLA_DS_CALL_CHANNEL_SET_VALUE_RESULT: {
      auto r = copyPayload<TDS_SuplaChannelNewValueResult>(data);
      conn->setResults.push_back({r.ChannelNumber, r.SenderID, r.Success});
      break;
    }
    case SUPLA_DCS_CALL_PING_SERVER: {
      conn->pings++;
      TSDC_SuplaPingServerResult result = {};
      sendPacket(conn, SUPLA_SDC_CALL_PING_SERVER_RESULT, &result,
                 sizeof(result));
      break;
    }
    case SUPLA_DCS_CALL_SET_ACTIVITY_TIMEOUT: {
      conn->activityTimeoutRequests++;
      auto req = copyPayload<TDCS_SuplaSetActivityTimeout>(data);
      TSDC_SuplaSetActivityTimeoutResult result = {};
      result.activity_timeout = req.activity_timeout;
      result.min = 10;
      result.max = 240;
      sendPacket(conn, SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT, &result,
                 sizeof(result));
      break;
    }
    default:
      break;
  }
}

class FakeTransport : public Transport {
 public:
  explicit FakeTransport(FakeSuplaServer *server) : server_(server) {}
  ~FakeTransport() override { close(); }

  bool connect(const std::string &, int) override {
    if (server_->refuseConnections) {
      status_ = TransportStatus::Failed;
      return true;
    }
    conn_ = server_->accept();
    status_ = TransportStatus::Connected;
    return true;
  }
  TransportStatus status() override {
    if (conn_ && conn_->closed) status_ = TransportStatus::Failed;
    return status_;
  }
  int read(void *buf, int len) override {
    if (!conn_ || conn_->closed) return 0;
    if (conn_->toDevice.empty()) return -1;
    int n = 0;
    char *out = static_cast<char *>(buf);
    while (n < len && !conn_->toDevice.empty()) {
      out[n++] = conn_->toDevice.front();
      conn_->toDevice.pop_front();
    }
    return n;
  }
  bool write(const void *buf, int len) override {
    if (!conn_ || conn_->closed) return false;
    const char *in = static_cast<const char *>(buf);
    conn_->toServer.insert(conn_->toServer.end(), in, in + len);
    return true;
  }
  bool flush() override { return !(conn_ && conn_->closed); }
  void close() override {
    if (conn_) conn_->closed = true;
    conn_.reset();
    status_ = TransportStatus::Idle;
  }
  int fd() const override { return -1; }
  bool hasPendingWrite() const override { return false; }

 private:
  FakeSuplaServer *server_;
  std::shared_ptr<FakeConnection> conn_;
  TransportStatus status_ = TransportStatus::Idle;
};

}  // namespace

FakeSuplaServer::FakeSuplaServer() = default;
FakeSuplaServer::~FakeSuplaServer() = default;

std::shared_ptr<FakeConnection> FakeSuplaServer::accept() {
  auto conn = std::make_shared<FakeConnection>();
  conn->server = this;
  connections.push_back(conn);
  acceptedCount++;
  return conn;
}

void FakeSuplaServer::iterate() {
  for (auto &conn : connections) {
    if (conn->closed) continue;
    auto &q = conn->toServer;
    while (q.size() >= kHeaderSize) {
      for (size_t i = 0; i < kTagSize; i++) {
        if (q[i] != kTag[i]) throw std::runtime_error("missing begin tag");
      }
      uint32_t dataSize = readU32(q, kTagSize + 1 + 8);
      size_t total = kHeaderSize + dataSize + kTagSize;
      if (q.size() < total) break;
      for (size_t i = 0; i < kTagSize; i++) {
        if (q[kHeaderSize + dataSize + i] != kTag[i]) {
          throw std::runtime_error("missing end tag");
        }
      }
      conn->version = static_cast<unsigned char>(q[kTagSize]);
      uint32_t callId = readU32(q, kTagSize + 1 + 4);
      std::vector<char> data(q.begin() + kHeaderSize,
                             q.begin() + kHeaderSize + dataSize);
      q.erase(q.begin(), q.begin() + total);
      handlePacket(conn.get(), callId, data);
    }
  }
}

void FakeSuplaServer::closeAll() {
  for (auto &conn : connections) conn->closed = true;
}

void FakeSuplaServer::sendGetChannelState(FakeConnection *conn, int channel,
                                          int senderId) {
  TCSD_ChannelStateRequest request = {};
  request.SenderID = senderId;
  request.ChannelNumber = static_cast<unsigned char>(channel);
  sendPacket(conn, SUPLA_CSD_CALL_GET_CHANNEL_STATE, &request, sizeof(request));
}

void FakeSuplaServer::sendSetValueRaw(FakeConnection *conn, int channel,
                                      int senderId, const char value[8]) {
  TSD_SuplaChannelNewValue v = {};
  v.SenderID = senderId;
  v.ChannelNumber = static_cast<unsigned char>(channel);
  memcpy(v.value, value, sizeof(v.value));
  sendPacket(conn, SUPLA_SD_CALL_CHANNEL_SET_VALUE, &v, sizeof(v));
}

void FakeSuplaServer::sendSetValue(FakeConnection *conn, int channel,
                                   int senderId, bool on, uint32_t durationMs) {
  TSD_SuplaChannelNewValue value = {};
  value.SenderID = senderId;
  value.DurationMS = durationMs;
  value.ChannelNumber = static_cast<unsigned char>(channel);
  value.value[0] = on ? 1 : 0;
  sendPacket(conn, SUPLA_SD_CALL_CHANNEL_SET_VALUE, &value, sizeof(value));
}

std::unique_ptr<Transport> FakeTransportFactory::create() {
  return std::make_unique<FakeTransport>(server_);
}

}  // namespace z2s::test
