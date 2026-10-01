// SPDX-License-Identifier: GPL-2.0-or-later

#include "z2s/supla_session.h"

#include <proto.h>
#include <srpc.h>

#include <algorithm>
#include <cstring>

#include "z2s/log.h"

namespace z2s {

namespace {

constexpr uint64_t kConnectTimeoutMs = 20000;
constexpr uint64_t kRegisterTimeoutMs = 15000;
constexpr uint64_t kMinReconnectDelayMs = 5000;
constexpr uint64_t kMaxReconnectDelayMs = 5 * 60 * 1000;
// srpc_iterate() handles at most one packet per call.
constexpr int kMaxPacketsPerIterate = 16;
// Electricity meter measurements (power, voltage, current) are sent at most
// this often; a change of the energy counter is sent at once.
constexpr uint64_t kMeasurementIntervalMs = 5000;
// How long a thermostat value set from Supla is kept against older reports
// of the device.
constexpr uint64_t kThermostatConfirmMs = 90000;

// srpc_ds_async_registerdevice_in_chunks_g() takes a callback without a user
// parameter. Registration is synchronous, so a thread local "current session"
// pointer is sufficient.
thread_local std::vector<TDS_SuplaDeviceChannel_E> *tlsRegisterChannels =
    nullptr;

TDS_SuplaDeviceChannel_E *getRegisterChannel(int index) {
  if (tlsRegisterChannels == nullptr || index < 0 ||
      static_cast<size_t>(index) >= tlsRegisterChannels->size()) {
    return nullptr;
  }
  return &(*tlsRegisterChannels)[index];
}

_supla_int_t srpcDataRead(void *buf, _supla_int_t count, void *userParams) {
  return static_cast<SuplaSession *>(userParams)->onDataRead(buf, count);
}

_supla_int_t srpcDataWrite(void *buf, _supla_int_t count, void *userParams) {
  return static_cast<SuplaSession *>(userParams)->onDataWrite(buf, count);
}

void srpcRemoteCall(void *, unsigned _supla_int_t, unsigned _supla_int_t,
                    void *userParams, unsigned char) {
  static_cast<SuplaSession *>(userParams)->onRemoteCall();
}

void copyString(char *dst, size_t dstSize, const std::string &src) {
  size_t len = std::min(src.size(), dstSize - 1);
  memcpy(dst, src.data(), len);
  dst[len] = '\0';
}

}  // namespace

const char *resultCodeToString(int resultCode) {
  switch (resultCode) {
    case SUPLA_RESULTCODE_TRUE:
      return "OK";
    case SUPLA_RESULTCODE_TEMPORARILY_UNAVAILABLE:
      return "server temporarily unavailable";
    case SUPLA_RESULTCODE_BAD_CREDENTIALS:
      return "bad credentials";
    case SUPLA_RESULTCODE_LOCATION_CONFLICT:
      return "location conflict";
    case SUPLA_RESULTCODE_CHANNEL_CONFLICT:
      return "channel conflict (channel layout differs from the one "
             "registered in Supla Cloud)";
    case SUPLA_RESULTCODE_DEVICE_DISABLED:
      return "device disabled in Supla Cloud";
    case SUPLA_RESULTCODE_LOCATION_DISABLED:
      return "location disabled";
    case SUPLA_RESULTCODE_DEVICE_LIMITEXCEEDED:
      return "device limit exceeded on this Supla account";
    case SUPLA_RESULTCODE_GUID_ERROR:
      return "invalid GUID";
    case SUPLA_RESULTCODE_DEVICE_LOCKED:
      return "device locked";
    case SUPLA_RESULTCODE_REGISTRATION_DISABLED:
      return "registration of new devices is disabled in Supla Cloud";
    case SUPLA_RESULTCODE_AUTHKEY_ERROR:
      return "invalid AuthKey";
    case SUPLA_RESULTCODE_NO_LOCATION_AVAILABLE:
      return "no location available";
    case SUPLA_RESULTCODE_UNAUTHORIZED:
      return "unauthorized";
    case SUPLA_RESULTCODE_RESTART_REQUESTED:
      return "OK (restart requested)";
    case SUPLA_RESULTCODE_IDENTIFY_REQUESTED:
      return "OK (identify requested)";
  }
  return "unknown result code";
}

SuplaSession::SuplaSession(const SuplaServerConfig &config,
                           TransportFactory *factory)
    : config_(config), factory_(factory) {}

SuplaSession::~SuplaSession() { disconnect(); }

void SuplaSession::configure(const DeviceIdentity &identity,
                             const DeviceDescriptor &descriptor) {
  bool nameChanged = name_ != descriptor.name;
  deviceId_ = identity.id;
  name_ = descriptor.name;
  manufacturer_ = descriptor.manufacturer;
  model_ = descriptor.model;
  batteryPowered_ = descriptor.batteryPowered;
  guid_ = identity.guid;
  authKey_ = identity.authKey;
  // Seed per-session jitter so that 100+ sessions do not reconnect in sync.
  rngState_ = 2166136261u;
  for (uint8_t b : guid_) {
    rngState_ = (rngState_ ^ b) * 16777619u;
  }
  if (rngState_ == 0) rngState_ = 1;

  bool layoutChanged = identity.channels.size() != channels_.size();
  std::vector<Channel> channels(identity.channels.size());
  for (size_t i = 0; i < identity.channels.size(); i++) {
    if (i < channels_.size()) {
      if (!(channels_[i].spec == identity.channels[i])) {
        layoutChanged = true;
      } else {
        channels[i] = channels_[i];
      }
    }
    // The layout comes from the identity; runtime capabilities are kept.
    uint32_t stepMs = channels[i].spec.countdownStepMs;
    uint32_t maxMs = channels[i].spec.countdownMaxMs;
    uint32_t actionCaps = channels[i].spec.actionCaps;
    channels[i].spec = identity.channels[i];
    channels[i].spec.countdownStepMs = stepMs;
    channels[i].spec.countdownMaxMs = maxMs;
    channels[i].spec.actionCaps = actionCaps;
    // The caption and unit describe the channel as the backend sees it now.
    channels[i].spec.caption.clear();
    channels[i].spec.unit.clear();
    channels[i].spec.precision = 0;
    channels[i].spec.thermometerKey.clear();
    channels[i].spec.setpointMin = NAN;
    channels[i].spec.setpointMax = NAN;
    channels[i].spec.setpointStep = NAN;
    for (const auto &c : descriptor.channels) {
      if (c.key == channels[i].spec.key && c.kind == channels[i].spec.kind) {
        channels[i].spec.caption = c.caption;
        channels[i].spec.unit = c.unit;
        channels[i].spec.precision = c.precision;
        channels[i].spec.thermometerKey = c.thermometerKey;
        channels[i].spec.setpointMin = c.setpointMin;
        channels[i].spec.setpointMax = c.setpointMax;
        channels[i].spec.setpointStep = c.setpointStep;
      }
    }
  }
  channels_ = std::move(channels);

  if (layoutChanged) relatedMeterSent_ = false;
  if ((layoutChanged || nameChanged) && state_ == State::Registered) {
    Z2S_LOG_INFO("[%s] Channel layout or name changed, re-registering",
                 name_.c_str());
    layoutChanged_ = true;
  }
}

void SuplaSession::setChannelPresent(int channelNumber, bool present) {
  if (channelNumber < 0 ||
      static_cast<size_t>(channelNumber) >= channels_.size()) {
    return;
  }
  channels_[channelNumber].present = present;
}

void SuplaSession::setChannelCountdown(int channelNumber, uint32_t stepMs,
                                       uint32_t maxMs) {
  if (channelNumber < 0 ||
      static_cast<size_t>(channelNumber) >= channels_.size()) {
    return;
  }
  auto &spec = channels_[channelNumber].spec;
  if (spec.countdownStepMs == stepMs && spec.countdownMaxMs == maxMs) return;
  spec.countdownStepMs = stepMs;
  spec.countdownMaxMs = maxMs;
  if (state_ == State::Registered) {
    Z2S_LOG_INFO("[%s] Channel %d: timer support changed, re-registering",
                 name_.c_str(), channelNumber);
    layoutChanged_ = true;
  }
}

void SuplaSession::setChannelActionCaps(int channelNumber, uint32_t caps) {
  if (channelNumber < 0 ||
      static_cast<size_t>(channelNumber) >= channels_.size()) {
    return;
  }
  auto &spec = channels_[channelNumber].spec;
  if (spec.actionCaps == caps) return;
  spec.actionCaps = caps;
  if (state_ == State::Registered) {
    Z2S_LOG_INFO("[%s] Channel %d: button actions changed, re-registering",
                 name_.c_str(), channelNumber);
    layoutChanged_ = true;
  }
}

void SuplaSession::triggerAction(int channelNumber, uint32_t action) {
  if (channelNumber < 0 ||
      static_cast<size_t>(channelNumber) >= channels_.size() ||
      channels_[channelNumber].spec.kind != ChannelKind::ActionTrigger ||
      !(channels_[channelNumber].spec.actionCaps & action)) {
    return;
  }
  if (state_ != State::Registered || srpc_ == nullptr) {
    Z2S_LOG_DEBUG("[%s] Channel %d: action 0x%X dropped (not registered)",
                  name_.c_str(), channelNumber, static_cast<unsigned>(action));
    return;
  }
  TDS_ActionTrigger at = {};
  at.ChannelNumber = static_cast<unsigned char>(channelNumber);
  at.ActionTrigger = static_cast<_supla_int_t>(action);
  Z2S_LOG_INFO("[%s] Channel %d: action 0x%X", name_.c_str(), channelNumber,
               static_cast<unsigned>(action));
  srpc_ds_async_action_trigger(srpc_, &at);
}

void SuplaSession::setChannelState(int channelNumber,
                                   const ChannelState &state) {
  if (channelNumber < 0 ||
      static_cast<size_t>(channelNumber) >= channels_.size()) {
    return;
  }
  auto &channel = channels_[channelNumber];
  ChannelState update = state;
  if (channel.spec.kind == ChannelKind::Thermostat &&
      nowMs_ < channel.expectedUntilMs) {
    // Until the device confirms a command, its older reports would undo
    // the value already shown in Supla.
    auto confirm = [](double expected, double *reported) {
      if (std::isnan(expected) || std::isnan(*reported)) return expected;
      if (std::fabs(expected - *reported) < 0.01) return double(NAN);
      *reported = NAN;
      return expected;
    };
    auto &e = channel.expected;
    e.primary = confirm(e.primary, &update.primary);
    e.mode = confirm(e.mode, &update.mode);
    e.program = confirm(e.program, &update.program);
  }
  channel.state = mergeState(channel.state, update);
  // The device switched itself off (or was switched off): the timer is over.
  if (channel.timerEndMs != 0 && channel.state.primary == 0) {
    reportTimer(channelNumber, 0, 0);
  }
}

void SuplaSession::reportTimer(int channelNumber, uint64_t endMs,
                               int32_t senderId) {
  auto &channel = channels_[channelNumber];
  channel.timerEndMs = endMs;
  channel.timerSenderId = senderId;
  channel.timerReportPending = true;
}

void SuplaSession::pushCaptions() {
  for (size_t i = 0; i < channels_.size(); i++) {
    auto &channel = channels_[i];
    if (!channel.captionPending) continue;
    channel.captionPending = false;
    TDCS_SetCaption caption = {};
    caption.ChannelNumber = static_cast<unsigned char>(i);
    copyString(caption.Caption, sizeof(caption.Caption),
               name_ + " – " + channel.spec.caption);
    caption.CaptionSize = static_cast<unsigned>(
        strnlen(caption.Caption, sizeof(caption.Caption)) + 1);
    srpc_dcs_async_set_channel_caption(srpc_, &caption);
  }
}

void SuplaSession::pushElectricityMeters() {
  for (size_t i = 0; i < channels_.size(); i++) {
    auto &channel = channels_[i];
    if (channel.spec.kind != ChannelKind::ElectricityMeter ||
        isChannelOffline(channel)) {
      continue;
    }
    TSuplaChannelExtendedValue ev = {};
    if (!encodeElectricityMeterExtendedValue(channel.state, &ev)) continue;
    if (channel.extSentOnce) {
      if (memcmp(&ev, channel.extSentValue, sizeof(ev)) == 0) continue;
      bool energyChanged = channel.state.primary != channel.extSentEnergy;
      if (!energyChanged &&
          nowMs_ - channel.extSentMs < kMeasurementIntervalMs) {
        continue;
      }
    }
    srpc_ds_async_channel_extendedvalue_changed(
        srpc_, static_cast<unsigned char>(i), &ev);
    Z2S_LOG_DEBUG("[%s] Channel %zu: meter sent (%.3f kWh)", name_.c_str(), i,
                  channel.state.primary);
    memcpy(channel.extSentValue, &ev, sizeof(ev));
    channel.extSentEnergy = channel.state.primary;
    channel.extSentMs = nowMs_;
    channel.extSentOnce = true;
  }
}

namespace {

bool sameGpmDefaults(const TChannelConfig_GeneralPurposeMeasurement &config,
                     const ChannelSpec &spec) {
  return config.DefaultValueDivider == 0 &&
         config.DefaultValueMultiplier == 0 && config.DefaultValueAdded == 0 &&
         config.DefaultValuePrecision == spec.precision &&
         config.DefaultUnitBeforeValue[0] == '\0' &&
         strncmp(config.DefaultUnitAfterValue, spec.unit.c_str(),
                 SUPLA_GENERAL_PURPOSE_UNIT_SIZE) == 0;
}

// Updates the device-owned part of a general purpose measurement config:
// the default unit and precision. False when nothing had to change.
bool fixGpmConfig(TChannelConfig_GeneralPurposeMeasurement *c,
                  const ChannelSpec &spec) {
  if (sameGpmDefaults(*c, spec)) return false;
  // Never configured by the device: the user settings start from the
  // defaults, like in supla-device; history is kept.
  bool fresh =
      c->DefaultUnitAfterValue[0] == '\0' && c->UnitAfterValue[0] == '\0';
  if (fresh) {
    c->ValuePrecision = static_cast<unsigned char>(spec.precision);
    copyString(c->UnitAfterValue, sizeof(c->UnitAfterValue), spec.unit);
    c->KeepHistory = 1;
  }
  c->DefaultValueDivider = 0;
  c->DefaultValueMultiplier = 0;
  c->DefaultValueAdded = 0;
  c->DefaultValuePrecision = static_cast<unsigned char>(spec.precision);
  memset(c->DefaultUnitBeforeValue, 0, sizeof(c->DefaultUnitBeforeValue));
  copyString(c->DefaultUnitAfterValue, sizeof(c->DefaultUnitAfterValue),
             spec.unit);
  return true;
}

// Updates the device-owned part of a thermostat config: a heating room
// thermostat of a ZigBee device. The device keeps its own regulation, so
// everything Supla could configure about it (algorithm, outputs, sensors,
// preset temperatures...) is hidden; the main thermometer, subfunction and
// setpoint range are read-only. User settings are left as they are. False
// when nothing had to change.
bool fixHvacConfig(TChannelConfig_HVAC *c, const ChannelSpec &spec,
                   int selfNumber, int thermometerNumber) {
  TChannelConfig_HVAC fixed = *c;
  auto self = static_cast<unsigned char>(selfNumber);
  fixed.MainThermometerChannelNo = static_cast<unsigned char>(
      thermometerNumber >= 0 ? thermometerNumber : selfNumber);
  // Pointing to the channel itself means "not set".
  fixed.AuxThermometerChannelNo = self;
  fixed.AuxThermometerType = SUPLA_HVAC_AUX_THERMOMETER_TYPE_NOT_SET;
  fixed.BinarySensorChannelNo = self;
  fixed.AntiFreezeAndOverheatProtectionEnabled = 0;
  fixed.AvailableAlgorithms = SUPLA_HVAC_ALGORITHM_ON_OFF_SETPOINT_MIDDLE;
  fixed.UsedAlgorithm = SUPLA_HVAC_ALGORITHM_ON_OFF_SETPOINT_MIDDLE;
  fixed.MinOnTimeS = 0;
  fixed.MinOffTimeS = 0;
  fixed.OutputValueOnError = 0;
  fixed.Subfunction = SUPLA_HVAC_SUBFUNCTION_HEAT;
  fixed.AuxMinMaxSetpointEnabled = 0;
  fixed.UseSeparateHeatCoolOutputs = 0;
  // Not set; the server reports the channel itself as their number.
  fixed.MasterThermostatIsSet = 0;
  fixed.MasterThermostatChannelNo = self;
  fixed.HeatOrColdSourceSwitchIsSet = 0;
  fixed.HeatOrColdSourceSwitchChannelNo = self;
  fixed.PumpSwitchIsSet = 0;
  fixed.PumpSwitchChannelNo = self;
  fixed.LocalUILockingCapabilities = 0;

  HvacParameterFlags flags = {};
  flags.MainThermometerChannelNoReadonly = 1;
  flags.AuxThermometerChannelNoHidden = 1;
  flags.BinarySensorChannelNoHidden = 1;
  flags.AuxThermometerTypeHidden = 1;
  flags.AntiFreezeAndOverheatProtectionEnabledHidden = 1;
  flags.UsedAlgorithmHidden = 1;
  flags.MinOnTimeSHidden = 1;
  flags.MinOffTimeSHidden = 1;
  flags.OutputValueOnErrorHidden = 1;
  flags.SubfunctionReadonly = 1;
  flags.TemperatureSetpointChangeSwitchesToManualModeHidden = 1;
  flags.AuxMinMaxSetpointEnabledHidden = 1;
  flags.UseSeparateHeatCoolOutputsHidden = 1;
  flags.TemperaturesFreezeProtectionHidden = 1;
  flags.TemperaturesEcoHidden = 1;
  flags.TemperaturesComfortHidden = 1;
  flags.TemperaturesBoostHidden = 1;
  flags.TemperaturesHeatProtectionHidden = 1;
  flags.TemperaturesHisteresisHidden = 1;
  flags.TemperaturesBelowAlarmHidden = 1;
  flags.TemperaturesAboveAlarmHidden = 1;
  flags.TemperaturesAuxMinSetpointHidden = 1;
  flags.TemperaturesAuxMaxSetpointHidden = 1;
  flags.MasterThermostatChannelNoHidden = 1;
  flags.HeatOrColdSourceSwitchHidden = 1;
  flags.PumpSwitchHidden = 1;
  flags.TemperaturesAuxHisteresisHidden = 1;
  fixed.ParameterFlags = flags;

  // Read-only temperatures: the setpoint range of the device.
  auto setTemperature = [&fixed](uint64_t bit, double celsius) {
    int index = 0;
    while ((1ULL << index) != bit) index++;
    if (std::isnan(celsius)) {
      fixed.Temperatures.Index &= ~static_cast<unsigned _supla_int_t>(bit);
      fixed.Temperatures.Temperature[index] = 0;
      return;
    }
    fixed.Temperatures.Index |= static_cast<unsigned _supla_int_t>(bit);
    fixed.Temperatures.Temperature[index] =
        static_cast<_supla_int16_t>(std::lround(celsius * 100.0));
  };
  setTemperature(TEMPERATURE_ROOM_MIN, spec.setpointMin);
  setTemperature(TEMPERATURE_ROOM_MAX, spec.setpointMax);

  if (memcmp(&fixed, c, sizeof(fixed)) == 0) return false;
  *c = fixed;
  return true;
}

// Default weekly schedule of a heating thermostat, as in supla-device:
// program 1 (19 °C) at night, program 2 (21 °C) from 6:00 to 21:00.
TChannelConfig_WeeklySchedule defaultWeeklySchedule() {
  TChannelConfig_WeeklySchedule schedule = {};
  const _supla_int16_t setpoints[] = {1900, 2100, 3000, 1200};
  for (int i = 0; i < 4; i++) {
    schedule.Program[i].Mode = SUPLA_HVAC_MODE_HEAT;
    schedule.Program[i].SetpointTemperatureHeat = setpoints[i];
  }
  for (int quarter = 0; quarter < SUPLA_WEEKLY_SCHEDULE_VALUES_SIZE;
       quarter++) {
    int hour = (quarter / 4) % 24;
    unsigned char program = hour >= 6 && hour < 21 ? 2 : 1;
    schedule.Quarters[quarter / 2] |= quarter % 2 ? program << 4 : program;
  }
  return schedule;
}

}  // namespace

void SuplaSession::handleChannelConfig(const TSD_ChannelConfig &config) {
  TSDS_SetChannelConfigResult result = {};
  result.ChannelNumber = config.ChannelNumber;
  result.ConfigType = config.ConfigType;
  result.Result = SUPLA_CONFIG_RESULT_TYPE_NOT_SUPPORTED;
  if (config.ChannelNumber < channels_.size() &&
      channels_[config.ChannelNumber].spec.kind == ChannelKind::Thermostat &&
      (config.ConfigType == SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE ||
       config.ConfigType == SUPLA_CONFIG_TYPE_ALT_WEEKLY_SCHEDULE)) {
    // The schedule itself is not used by the gateway (the device runs its
    // own program); the server only has to have one.
    result.Result = SUPLA_CONFIG_RESULT_TRUE;
    if (config.ConfigType == SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE &&
        config.ConfigSize > 0) {
      channels_[config.ChannelNumber].scheduleReceived = true;
    }
    srpc_ds_async_set_channel_config_result(srpc_, &result);
    return;
  }
  if (config.ChannelNumber >= channels_.size() ||
      config.ConfigType != SUPLA_CONFIG_TYPE_DEFAULT) {
    srpc_ds_async_set_channel_config_result(srpc_, &result);
    return;
  }
  auto &channel = channels_[config.ChannelNumber];
  size_t size = 0;
  if (channel.spec.kind == ChannelKind::GeneralPurposeMeasurement) {
    size = sizeof(TChannelConfig_GeneralPurposeMeasurement);
  } else if (channel.spec.kind == ChannelKind::Thermostat) {
    size = sizeof(TChannelConfig_HVAC);
  }
  if (size == 0) {
    srpc_ds_async_set_channel_config_result(srpc_, &result);
    return;
  }
  result.Result = SUPLA_CONFIG_RESULT_TRUE;
  if (config.Func == 0) {
    // Channel disabled in Cloud.
  } else if (config.ConfigSize != 0 && config.ConfigSize != size) {
    result.Result = SUPLA_CONFIG_RESULT_DATA_ERROR;
  } else {
    // An empty config (never set) is completed from zeros.
    char data[SUPLA_CHANNEL_CONFIG_MAXSIZE] = {};
    memcpy(data, config.Config, config.ConfigSize);
    bool changed;
    if (channel.spec.kind == ChannelKind::Thermostat) {
      int thermometer = -1;
      for (size_t i = 0; i < channels_.size(); i++) {
        if (channels_[i].spec.kind == ChannelKind::Thermometer &&
            channels_[i].spec.key == channel.spec.thermometerKey) {
          thermometer = static_cast<int>(i);
        }
      }
      changed = fixHvacConfig(reinterpret_cast<TChannelConfig_HVAC *>(data),
                              channel.spec, config.ChannelNumber, thermometer);
    } else {
      changed = fixGpmConfig(
          reinterpret_cast<TChannelConfig_GeneralPurposeMeasurement *>(data),
          channel.spec);
    }
    if (changed || config.ConfigSize == 0) {
      memcpy(channel.configData, data, size);
      channel.configSize = static_cast<uint16_t>(size);
      channel.configPending = true;
    }
  }
  srpc_ds_async_set_channel_config_result(srpc_, &result);
}

void SuplaSession::pushChannelConfigs() {
  for (size_t i = 0; i < channels_.size(); i++) {
    auto &channel = channels_[i];
    if (!channel.configPending || !channel.configFinished) continue;
    channel.configPending = false;
    // The server sends the config back after each change; a bounded number
    // of attempts prevents a loop if it does not keep the device's part.
    if (channel.configAttempts >= 3) continue;
    channel.configAttempts++;
    TSDS_SetChannelConfig config = {};
    config.ChannelNumber = static_cast<unsigned char>(i);
    config.Func = suplaDefaultFunction(channel.spec);
    config.ConfigType = SUPLA_CONFIG_TYPE_DEFAULT;
    config.ConfigSize = channel.configSize;
    memcpy(config.Config, channel.configData, channel.configSize);
    Z2S_LOG_DEBUG("[%s] Channel %zu: device config sent", name_.c_str(), i);
    srpc_ds_async_set_channel_config_request(srpc_, &config);
  }
  // After the channel config, like supla-device.
  for (size_t i = 0; i < channels_.size(); i++) {
    auto &channel = channels_[i];
    if (channel.schedulePending && channel.configFinished) {
      channel.schedulePending = false;
      TSDS_SetChannelConfig config = {};
      config.ChannelNumber = static_cast<unsigned char>(i);
      config.Func = suplaDefaultFunction(channel.spec);
      config.ConfigType = SUPLA_CONFIG_TYPE_WEEKLY_SCHEDULE;
      TChannelConfig_WeeklySchedule schedule = defaultWeeklySchedule();
      config.ConfigSize = sizeof(schedule);
      memcpy(config.Config, &schedule, sizeof(schedule));
      Z2S_LOG_DEBUG("[%s] Channel %zu: default weekly schedule sent",
                    name_.c_str(), i);
      srpc_ds_async_set_channel_config_request(srpc_, &config);
    }
  }
}

void SuplaSession::sendRelatedMeterConfig() {
  // A plug: one relay and the meter that measures it. The server links them
  // unless the user has already chosen a related meter for the relay.
  int relay = -1;
  int meter = -1;
  for (size_t i = 0; i < channels_.size(); i++) {
    auto kind = channels_[i].spec.kind;
    if (kind == ChannelKind::Relay) {
      if (relay != -1) return;
      relay = static_cast<int>(i);
    } else if (kind == ChannelKind::ElectricityMeter) {
      if (meter != -1) return;
      meter = static_cast<int>(i);
    }
  }
  if (relay == -1 || meter == -1) return;

  TSDS_SetChannelConfig config = {};
  config.ChannelNumber = static_cast<unsigned char>(relay);
  config.Func = suplaDefaultFunction(channels_[relay].spec);
  config.ConfigType = SUPLA_CONFIG_TYPE_DEFAULT;
  config.ConfigSize = sizeof(TChannelConfig_PowerSwitch);
  auto *ps = reinterpret_cast<TChannelConfig_PowerSwitch *>(config.Config);
  ps->DefaultRelatedMeterIsSet = 1;
  ps->DefaultRelatedMeterChannelNo = static_cast<unsigned char>(meter);
  Z2S_LOG_DEBUG("[%s] Channel %d: related meter is channel %d", name_.c_str(),
                relay, meter);
  srpc_ds_async_set_channel_config_request(srpc_, &config);
}

void SuplaSession::pushTimerStates() {
  for (size_t i = 0; i < channels_.size(); i++) {
    auto &channel = channels_[i];
    if (!channel.timerReportPending) continue;
    channel.timerReportPending = false;
    TSuplaChannelExtendedValue ev = {};
    ev.type = EV_TYPE_TIMER_STATE_V1;
    ev.size = sizeof(TTimerState_ExtendedValue);
    auto *timer = reinterpret_cast<TTimerState_ExtendedValue *>(ev.value);
    timer->RemainingTimeMs =
        channel.timerEndMs > nowMs_
            ? static_cast<unsigned _supla_int_t>(channel.timerEndMs - nowMs_)
            : 0;
    timer->TargetValue[0] = 0;  // the device turns off when the time is up
    timer->SenderID = channel.timerSenderId;
    if (timer->RemainingTimeMs == 0) channel.timerEndMs = 0;
    Z2S_LOG_DEBUG("[%s] Channel %zu: timer state sent (%u ms left)",
                  name_.c_str(), i,
                  static_cast<unsigned>(timer->RemainingTimeMs));
    srpc_ds_async_channel_extendedvalue_changed(
        srpc_, static_cast<unsigned char>(i), &ev);
  }
}

void SuplaSession::setDeviceOnline(bool online) { deviceOnline_ = online; }

void SuplaSession::setDeviceHealth(const DeviceHealth &update) {
  DeviceHealth health = mergeHealth(health_, update);
  if (health.batteryLevel != health_.batteryLevel ||
      health.batteryLow != health_.batteryLow) {
    channelStatePending_ = true;
  }
  health_ = health;
}

void SuplaSession::sendChannelState(int channelNumber, int32_t receiverId) {
  TDSC_ChannelState state = {};
  state.ReceiverID = receiverId;
  state.ChannelNumber = static_cast<unsigned char>(channelNumber);
  if (batteryPowered_ >= 0) {
    state.Fields |= SUPLA_CHANNELSTATE_FIELD_BATTERYPOWERED;
    state.BatteryPowered = static_cast<unsigned char>(batteryPowered_);
  }
  if (health_.batteryLevel >= 0) {
    state.Fields |= SUPLA_CHANNELSTATE_FIELD_BATTERYLEVEL;
    state.BatteryLevel = static_cast<unsigned char>(health_.batteryLevel);
  } else if (health_.batteryLow >= 0) {
    state.Fields |= SUPLA_CHANNELSTATE_FIELD_BATTERY_STATE;
    state.BatteryState =
        health_.batteryLow ? SUPLA_BATTERY_STATE_LOW : SUPLA_BATTERY_STATE_OK;
  }
  if (health_.linkQuality >= 0) {
    state.Fields |= SUPLA_CHANNELSTATE_FIELD_BRIDGENODESIGNALSTRENGTH;
    state.BridgeNodeSignalStrength =
        static_cast<unsigned char>(health_.linkQuality);
  }
  state.Fields |= SUPLA_CHANNELSTATE_FIELD_BRIDGENODEONLINE;
  state.BridgeNodeOnline = deviceOnline_ ? 1 : 0;
  srpc_csd_async_channel_state_result(srpc_, &state);
}

void SuplaSession::pushChannelStates() {
  if (!channelStatePending_) return;
  channelStatePending_ = false;
  for (size_t i = 0; i < channels_.size(); i++) {
    sendChannelState(static_cast<int>(i), 0);
  }
  Z2S_LOG_DEBUG("[%s] Channel state sent (battery %d %%)", name_.c_str(),
                health_.batteryLevel);
}

const ChannelState *SuplaSession::channelState(int channelNumber) const {
  if (channelNumber < 0 ||
      static_cast<size_t>(channelNumber) >= channels_.size()) {
    return nullptr;
  }
  return &channels_[channelNumber].state;
}

bool SuplaSession::isChannelOffline(const Channel &channel) const {
  if (!deviceOnline_ || !channel.present) {
    return true;
  }
  // A button has no state of its own.
  if (channel.spec.kind == ChannelKind::ActionTrigger) return false;
  // A meter without the energy counter would record a false zero in the
  // history.
  if (channel.spec.kind == ChannelKind::ElectricityMeter) {
    return std::isnan(channel.state.primary);
  }
  // A sensor without any reading yet is reported as offline.
  return isEmpty(channel.state);
}

bool SuplaSession::wantsToConnect(uint64_t nowMs) const {
  return state_ == State::Idle && !suspended_ && !channels_.empty() &&
         nowMs >= nextConnectMs_;
}

void SuplaSession::setSuspended(bool suspended) {
  if (suspended_ == suspended) return;
  suspended_ = suspended;
  if (suspended) {
    Z2S_LOG_INFO("[%s] ZigBee device is offline, disconnecting from Supla",
                 name_.c_str());
    disconnect();
  } else {
    Z2S_LOG_INFO("[%s] ZigBee device is online again, connecting to Supla",
                 name_.c_str());
    nextConnectMs_ = 0;
  }
}

void SuplaSession::disable() {
  disconnect();
  state_ = State::Disabled;
}

void SuplaSession::enable() {
  if (state_ == State::Disabled) {
    state_ = State::Idle;
    nextConnectMs_ = 0;
  }
}

int SuplaSession::fd() const { return transport_ ? transport_->fd() : -1; }

bool SuplaSession::hasPendingWrite() const {
  return transport_ && transport_->hasPendingWrite();
}

uint32_t SuplaSession::jitter(uint32_t maxMs) {
  if (maxMs == 0) return 0;
  // xorshift32
  rngState_ ^= rngState_ << 13;
  rngState_ ^= rngState_ >> 17;
  rngState_ ^= rngState_ << 5;
  return rngState_ % maxMs;
}

void SuplaSession::startConnect(uint64_t nowMs) {
  nowMs_ = nowMs;
  disconnect();
  transport_ = factory_->create();
  if (!transport_ || !transport_->connect(config_.server, config_.port)) {
    Z2S_LOG_WARNING("[%s] Cannot start connection to %s:%d", name_.c_str(),
                    config_.server.c_str(), config_.port);
    disconnect();
    scheduleReconnectAfterFailure(nowMs);
    return;
  }
  state_ = State::Connecting;
  stateSinceMs_ = nowMs;
  Z2S_LOG_DEBUG("[%s] Connecting to %s:%d", name_.c_str(),
                config_.server.c_str(), config_.port);
}

void SuplaSession::disconnect() {
  if (srpc_ != nullptr) {
    srpc_free(srpc_);
    srpc_ = nullptr;
  }
  if (transport_) {
    transport_->close();
    transport_.reset();
  }
  for (auto &channel : channels_) {
    channel.sentOnce = false;
    channel.extSentOnce = false;
  }
  writeFailed_ = false;
  closeRequested_ = false;
  if (state_ != State::Disabled) {
    state_ = State::Idle;
  }
}

void SuplaSession::scheduleReconnect(uint64_t nowMs, uint64_t delayMs) {
  nextConnectMs_ =
      nowMs + delayMs + jitter(static_cast<uint32_t>(delayMs / 4 + 1));
}

void SuplaSession::scheduleReconnectAfterFailure(uint64_t nowMs) {
  uint64_t delay = kMinReconnectDelayMs;
  for (int i = 0; i < failureCount_ && delay < kMaxReconnectDelayMs; i++) {
    delay *= 2;
  }
  delay = std::min(delay, kMaxReconnectDelayMs);
  if (failureCount_ < 16) failureCount_++;
  scheduleReconnect(nowMs, delay);
}

int SuplaSession::onDataRead(void *buf, int count) {
  if (!transport_) return 0;
  int r = transport_->read(buf, count);
  if (r > 0) {
    lastResponseMs_ = nowMs_;
  }
  return r;
}

int SuplaSession::onDataWrite(void *buf, int count) {
  if (!transport_ || writeFailed_) return -1;
  if (!transport_->write(buf, count)) {
    writeFailed_ = true;
    return -1;
  }
  lastSentMs_ = nowMs_;
  return count;
}

void SuplaSession::sendRegistration() {
  TDS_SuplaRegisterDeviceHeader header = {};
  copyString(header.Email, sizeof(header.Email), config_.email);
  memcpy(header.AuthKey, authKey_.data(), SUPLA_AUTHKEY_SIZE);
  memcpy(header.GUID, guid_.data(), SUPLA_GUID_SIZE);
  copyString(header.Name, sizeof(header.Name), name_);
  copyString(header.SoftVer, sizeof(header.SoftVer), config_.softVersion);
  copyString(header.ServerName, sizeof(header.ServerName), config_.server);
  header.Flags = 0;
  header.ManufacturerID = 0;
  header.ProductID = 0;
  header.channel_count = static_cast<unsigned char>(channels_.size());

  std::vector<TDS_SuplaDeviceChannel_E> regChannels(channels_.size());
  for (size_t i = 0; i < channels_.size(); i++) {
    auto &src = channels_[i];
    auto &dst = regChannels[i];
    memset(&dst, 0, sizeof(dst));
    dst.Number = static_cast<unsigned char>(i);
    dst.Type = suplaChannelType(src.spec.kind);
    dst.FuncList = suplaFuncList(src.spec);
    dst.Default = suplaDefaultFunction(src.spec);
    dst.Flags = static_cast<_supla_int64_t>(suplaChannelFlags(src.spec));
    bool offline = isChannelOffline(src);
    dst.Offline = offline ? SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE
                          : SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE;
    encodeChannelValue(src.spec.kind, src.state, dst.value);
    memcpy(src.sentValue, dst.value, sizeof(src.sentValue));
    src.sentOffline = offline;
    src.sentOnce = true;
  }

  Z2S_LOG_INFO("[%s] Registering (GUID %s, %zu channel(s))", name_.c_str(),
               toHex(guid_.data(), guid_.size()).c_str(), channels_.size());
  tlsRegisterChannels = &regChannels;
  _supla_int_t result = srpc_ds_async_registerdevice_in_chunks_g(
      srpc_, &header, &getRegisterChannel);
  tlsRegisterChannels = nullptr;
  if (!result) {
    Z2S_LOG_ERROR("[%s] Failed to send registration", name_.c_str());
    closeRequested_ = true;
  }
}

void SuplaSession::handleRegisterResult(int resultCode, int activityTimeout) {
  lastResultCode_ = resultCode;
  if (resultCode == SUPLA_RESULTCODE_TRUE ||
      resultCode == SUPLA_RESULTCODE_RESTART_REQUESTED ||
      resultCode == SUPLA_RESULTCODE_IDENTIFY_REQUESTED) {
    Z2S_LOG_INFO("[%s] Registered (activity timeout %d s)", name_.c_str(),
                 activityTimeout);
    state_ = State::Registered;
    stateSinceMs_ = nowMs_;
    failureCount_ = 0;
    // Initial captions; the server applies them only to channels without one.
    for (auto &channel : channels_) {
      channel.captionPending = !channel.spec.caption.empty();
      channel.configFinished = false;
      channel.configPending = false;
      channel.schedulePending = false;
      channel.scheduleReceived = false;
      channel.configAttempts = 0;
    }
    // Like supla-device: the battery state is sent after registration.
    channelStatePending_ = batteryPowered_ >= 0 || health_.batteryLevel >= 0 ||
                           health_.batteryLow >= 0;
    if (!relatedMeterSent_) {
      relatedMeterSent_ = true;
      sendRelatedMeterConfig();
    }
    activityTimeoutS_ = activityTimeout > 0 ? activityTimeout : 30;
    if (activityTimeoutS_ != config_.activityTimeoutS) {
      TDCS_SuplaSetActivityTimeout at = {};
      at.activity_timeout = static_cast<unsigned char>(
          std::clamp(config_.activityTimeoutS, 6, 255));
      srpc_dcs_async_set_activity_timeout(srpc_, &at);
    }
    return;
  }

  Z2S_LOG_ERROR("[%s] Registration failed: %s (%d)", name_.c_str(),
                resultCodeToString(resultCode), resultCode);
  closeRequested_ = true;
  uint64_t delay;
  switch (resultCode) {
    case SUPLA_RESULTCODE_REGISTRATION_DISABLED:
      // The user has to enable registration in Supla Cloud; retry often
      // enough to catch the registration window.
      delay = 60 * 1000;
      break;
    case SUPLA_RESULTCODE_TEMPORARILY_UNAVAILABLE:
      delay = 30 * 1000;
      break;
    default:
      delay = 10 * 60 * 1000;
      break;
  }
  scheduleReconnect(nowMs_, delay);
}

bool SuplaSession::handleThermostatValue(int channelNumber, const char *value,
                                         uint32_t durationMs) {
  auto &channel = channels_[channelNumber];
  const ChannelSpec &spec = channel.spec;
  const ChannelState &current = channel.state;
  THVACValue hvac;
  memcpy(&hvac, value, sizeof(hvac));
  ChannelCommand command;
  command.type = ChannelCommand::Type::SetThermostat;
  bool valid = durationMs == 0;  // no timer ("for a time") in the device
  switch (hvac.Mode) {
    case SUPLA_HVAC_MODE_NOT_SET:
      break;
    case SUPLA_HVAC_MODE_OFF:
      command.mode = 1;
      break;
    case SUPLA_HVAC_MODE_HEAT:
      command.mode = 2;
      command.manual = true;
      break;
    case SUPLA_HVAC_MODE_CMD_TURN_ON:
      command.mode = 2;
      break;
    case SUPLA_HVAC_MODE_CMD_SWITCH_TO_MANUAL:
      command.manual = true;
      break;
    case SUPLA_HVAC_MODE_CMD_WEEKLY_SCHEDULE:
      // The "program" of Supla runs the device's own schedule.
      command.program = true;
      if (current.mode == 0) command.mode = 2;
      break;
    default:
      // Cooling...: not supported.
      valid = false;
      break;
  }
  if (!command.program &&
      (hvac.Flags & SUPLA_HVAC_VALUE_FLAG_SETPOINT_TEMP_HEAT_SET)) {
    double setpoint = hvac.SetpointTemperatureHeat / 100.0;
    if (!std::isnan(spec.setpointStep) && spec.setpointStep > 0) {
      setpoint = std::round(setpoint / spec.setpointStep) * spec.setpointStep;
    }
    if (!std::isnan(spec.setpointMin)) {
      setpoint = std::max(setpoint, spec.setpointMin);
    }
    if (!std::isnan(spec.setpointMax)) {
      setpoint = std::min(setpoint, spec.setpointMax);
    }
    command.setpoint = setpoint;
  }
  if (!valid) {
    Z2S_LOG_WARNING("[%s] Channel %d: thermostat mode %d rejected",
                    name_.c_str(), channelNumber, hvac.Mode);
    return false;
  }

  // Supla sends the whole value (e.g. the setpoint with "off"); only what
  // changes is sent to the device, which handles each change slowly.
  if ((command.mode == 1 && current.mode == 0) ||
      (command.mode == 2 && current.mode == 1)) {
    command.mode = 0;
  }
  if (!std::isnan(command.setpoint) && !std::isnan(current.primary) &&
      std::fabs(command.setpoint - current.primary) < 0.01) {
    command.setpoint = NAN;
  }
  // A new setpoint must not be overridden by the device's own schedule.
  if (!std::isnan(command.setpoint)) command.manual = true;
  if (command.mode == 1 || current.program == 0) command.manual = false;
  if (command.program && current.program == 1) command.program = false;
  if (command.mode == 0 && std::isnan(command.setpoint) && !command.manual &&
      !command.program) {
    return true;  // nothing changes
  }

  Z2S_LOG_INFO("[%s] Channel %d: thermostat %s%s, setpoint %.1f requested",
               name_.c_str(), channelNumber,
               command.mode == 1   ? "off"
               : command.mode == 2 ? "heat"
                                   : "-",
               command.program  ? " (program)"
               : command.manual ? " (manual)"
                                : "",
               command.setpoint);
  if (!handler_(channelNumber, command)) return false;

  // Shown in Supla at once, like supla-device does: the device confirms the
  // change only after a while (about 25 s for some Tuya thermostats).
  ChannelState expected;
  if (command.mode != 0) expected.mode = command.mode == 2 ? 1 : 0;
  expected.primary = command.setpoint;
  if (command.program) expected.program = 1;
  if (command.manual) expected.program = 0;
  channel.state = mergeState(channel.state, expected);
  channel.expected = expected;
  channel.expectedUntilMs = nowMs_ + kThermostatConfirmMs;
  return true;
}

void SuplaSession::handleNewValue(int channelNumber, const char *value,
                                  uint32_t durationMs, int senderId,
                                  bool fromGroup) {
  bool success = false;
  if (channelNumber >= 0 &&
      static_cast<size_t>(channelNumber) < channels_.size() &&
      channels_[channelNumber].spec.kind == ChannelKind::Thermostat &&
      !isChannelOffline(channels_[channelNumber]) && handler_) {
    success = handleThermostatValue(channelNumber, value, durationMs);
  } else if (channelNumber >= 0 &&
             static_cast<size_t>(channelNumber) < channels_.size() &&
             channels_[channelNumber].spec.kind == ChannelKind::Relay &&
             !isChannelOffline(channels_[channelNumber]) && handler_) {
    const ChannelSpec &spec = channels_[channelNumber].spec;
    ChannelCommand command;
    command.type =
        value[0] ? ChannelCommand::Type::TurnOn : ChannelCommand::Type::TurnOff;
    bool valid = true;
    if (command.type == ChannelCommand::Type::TurnOn && durationMs > 0) {
      // The timer runs in the device; without one the request cannot be
      // honoured, so it is rejected rather than left on indefinitely.
      uint32_t step = spec.countdownStepMs;
      uint64_t rounded =
          step == 0 ? 0 : (uint64_t{durationMs} + step - 1) / step * step;
      if (step == 0 || rounded > spec.countdownMaxMs) {
        Z2S_LOG_WARNING(
            "[%s] Channel %d: turn-on for %u ms rejected (device timer: %s)",
            name_.c_str(), channelNumber, static_cast<unsigned>(durationMs),
            step == 0 ? "none" : "too long");
        valid = false;
      } else {
        command.durationMs = static_cast<uint32_t>(rounded);
      }
    }
    if (valid) {
      if (command.durationMs > 0) {
        Z2S_LOG_INFO("[%s] Channel %d: ON for %u s requested", name_.c_str(),
                     channelNumber,
                     static_cast<unsigned>(command.durationMs / 1000));
      } else {
        Z2S_LOG_INFO("[%s] Channel %d: %s requested", name_.c_str(),
                     channelNumber, value[0] ? "ON" : "OFF");
      }
      success = handler_(channelNumber, command);
    }
    if (success && command.durationMs > 0) {
      reportTimer(channelNumber, nowMs_ + command.durationMs, senderId);
    } else if (success && command.type == ChannelCommand::Type::TurnOff &&
               channels_[channelNumber].timerEndMs != 0) {
      reportTimer(channelNumber, 0, senderId);  // switched off before the end
    }
  } else {
    Z2S_LOG_WARNING("[%s] Channel %d: command rejected", name_.c_str(),
                    channelNumber);
  }
  if (!fromGroup) {
    srpc_ds_async_set_channel_result(srpc_,
                                     static_cast<unsigned char>(channelNumber),
                                     senderId, success ? 1 : 0);
  }
}

void SuplaSession::onRemoteCall() {
  TsrpcReceivedData rd;
  if (srpc_getdata(srpc_, &rd, 0) != SUPLA_RESULT_TRUE) {
    return;
  }
  switch (rd.call_id) {
    case SUPLA_SDC_CALL_VERSIONERROR:
      Z2S_LOG_ERROR("[%s] Protocol version error (server min %d, max %d)",
                    name_.c_str(),
                    rd.data.sdc_version_error->server_version_min,
                    rd.data.sdc_version_error->server_version);
      closeRequested_ = true;
      scheduleReconnect(nowMs_, 10 * 60 * 1000);
      break;
    case SUPLA_SD_CALL_REGISTER_DEVICE_RESULT:
      handleRegisterResult(rd.data.sd_register_device_result->result_code,
                           rd.data.sd_register_device_result->activity_timeout);
      break;
    case SUPLA_SD_CALL_REGISTER_DEVICE_RESULT_B:
      handleRegisterResult(
          rd.data.sd_register_device_result_b->result_code,
          rd.data.sd_register_device_result_b->activity_timeout);
      break;
    case SUPLA_SDC_CALL_SET_ACTIVITY_TIMEOUT_RESULT:
      activityTimeoutS_ =
          rd.data.sdc_set_activity_timeout_result->activity_timeout;
      Z2S_LOG_DEBUG("[%s] Activity timeout set to %d s", name_.c_str(),
                    activityTimeoutS_);
      break;
    case SUPLA_SDC_CALL_PING_SERVER_RESULT:
      break;
    case SUPLA_SD_CALL_CHANNEL_SET_VALUE:
      handleNewValue(rd.data.sd_channel_new_value->ChannelNumber,
                     rd.data.sd_channel_new_value->value,
                     rd.data.sd_channel_new_value->DurationMS,
                     rd.data.sd_channel_new_value->SenderID, false);
      break;
    case SUPLA_SD_CALL_CHANNELGROUP_SET_VALUE:
      handleNewValue(rd.data.sd_channelgroup_new_value->ChannelNumber,
                     rd.data.sd_channelgroup_new_value->value,
                     rd.data.sd_channelgroup_new_value->DurationMS, 0, true);
      break;
    case SUPLA_CSD_CALL_GET_CHANNEL_STATE: {
      // The user opened the channel information in the app.
      const auto *request = rd.data.csd_channel_state_request;
      if (request != nullptr &&
          request->ChannelNumber < static_cast<int>(channels_.size())) {
        sendChannelState(request->ChannelNumber, request->SenderID);
      }
      break;
    }
    case SUPLA_SD_CALL_SET_CHANNEL_CONFIG:
      if (rd.data.sds_set_channel_config_request != nullptr) {
        handleChannelConfig(*rd.data.sds_set_channel_config_request);
      }
      break;
    case SUPLA_SD_CALL_CHANNEL_CONFIG_FINISHED: {
      const auto *finished = rd.data.sd_channel_config_finished;
      if (finished != nullptr && finished->ChannelNumber < channels_.size()) {
        auto &channel = channels_[finished->ChannelNumber];
        channel.configFinished = true;
        // The server sends no weekly schedule when it has none (like
        // supla-device, the device then sends its default one).
        if (channel.spec.kind == ChannelKind::Thermostat &&
            !channel.scheduleReceived) {
          channel.schedulePending = true;
        }
      }
      break;
    }
    case SUPLA_SD_CALL_SET_CHANNEL_CONFIG_RESULT: {
      const auto *result = rd.data.sds_set_channel_config_result;
      if (result != nullptr && result->Result != SUPLA_CONFIG_RESULT_TRUE) {
        // E.g. the user has set a function without a related meter.
        Z2S_LOG_DEBUG("[%s] Channel %d: config not accepted (%d)",
                      name_.c_str(), result->ChannelNumber, result->Result);
      }
      break;
    }
    case SUPLA_SD_CALL_DEVICE_CALCFG_REQUEST: {
      TDS_DeviceCalCfgResult result = {};
      result.ReceiverID = rd.data.sd_device_calcfg_request->SenderID;
      result.ChannelNumber = rd.data.sd_device_calcfg_request->ChannelNumber;
      result.Command = rd.data.sd_device_calcfg_request->Command;
      result.Result = SUPLA_CALCFG_RESULT_NOT_SUPPORTED;
      srpc_ds_async_device_calcfg_result(srpc_, &result);
      break;
    }
    default:
      Z2S_LOG_DEBUG("[%s] Ignoring call %d from server", name_.c_str(),
                    rd.call_id);
      break;
  }
  srpc_rd_free(&rd);
}

void SuplaSession::pushChannelValues(bool force) {
  for (size_t i = 0; i < channels_.size(); i++) {
    auto &channel = channels_[i];
    char value[SUPLA_CHANNELVALUE_SIZE];
    encodeChannelValue(channel.spec.kind, channel.state, value);
    bool offline = isChannelOffline(channel);
    if (!force && channel.sentOnce && channel.sentOffline == offline &&
        memcmp(value, channel.sentValue, sizeof(value)) == 0) {
      continue;
    }
    srpc_ds_async_channel_value_changed_c(
        srpc_, static_cast<unsigned char>(i), value,
        offline ? SUPLA_CHANNEL_OFFLINE_FLAG_OFFLINE
                : SUPLA_CHANNEL_OFFLINE_FLAG_ONLINE,
        0);
    Z2S_LOG_DEBUG("[%s] Channel %zu: value sent (%s, first byte %d)",
                  name_.c_str(), i, offline ? "offline" : "online",
                  static_cast<int>(static_cast<unsigned char>(value[0])));
    memcpy(channel.sentValue, value, sizeof(value));
    channel.sentOffline = offline;
    channel.sentOnce = true;
  }
}

void SuplaSession::ping(uint64_t nowMs) {
  uint64_t timeoutMs = static_cast<uint64_t>(activityTimeoutS_) * 1000;
  if (nowMs - lastResponseMs_ >= timeoutMs + 10000) {
    Z2S_LOG_WARNING("[%s] No response from server, reconnecting",
                    name_.c_str());
    closeRequested_ = true;
    scheduleReconnectAfterFailure(nowMs);
    return;
  }
  uint64_t idleThresholdMs = timeoutMs > 5000 ? timeoutMs - 5000 : timeoutMs;
  if (nowMs - lastPingMs_ >= 5000 &&
      (nowMs - lastResponseMs_ >= idleThresholdMs ||
       nowMs - lastSentMs_ >= idleThresholdMs)) {
    lastPingMs_ = nowMs;
    srpc_dcs_async_ping_server(srpc_);
  }
}

void SuplaSession::iterate(uint64_t nowMs) {
  nowMs_ = nowMs;
  switch (state_) {
    case State::Idle:
    case State::Disabled:
      return;

    case State::Connecting: {
      TransportStatus status = transport_->status();
      if (status == TransportStatus::Connected) {
        TsrpcParams params;
        srpc_params_init(&params);
        params.data_read = &srpcDataRead;
        params.data_write = &srpcDataWrite;
        params.on_remote_call_received = &srpcRemoteCall;
        params.user_params = this;
        srpc_ = srpc_init(&params);
        srpc_set_proto_version(
            srpc_, static_cast<unsigned char>(config_.protoVersion));
        state_ = State::Registering;
        stateSinceMs_ = nowMs;
        lastResponseMs_ = nowMs;
        lastSentMs_ = nowMs;
        lastPingMs_ = nowMs;
        layoutChanged_ = false;
        sendRegistration();
      } else if (status == TransportStatus::Failed ||
                 nowMs - stateSinceMs_ > kConnectTimeoutMs) {
        Z2S_LOG_WARNING("[%s] Connection to %s failed", name_.c_str(),
                        config_.server.c_str());
        disconnect();
        scheduleReconnectAfterFailure(nowMs);
        return;
      }
      break;
    }

    case State::Registering:
    case State::Registered: {
      for (int i = 0;
           i < kMaxPacketsPerIterate && srpc_ != nullptr && !closeRequested_;
           i++) {
        if (srpc_iterate(srpc_) != SUPLA_RESULT_TRUE) {
          if (!closeRequested_) {
            Z2S_LOG_WARNING("[%s] Connection closed", name_.c_str());
            closeRequested_ = true;
            scheduleReconnectAfterFailure(nowMs);
          }
          break;
        }
        if (transport_ && transport_->status() != TransportStatus::Connected) {
          break;
        }
      }
      if (!closeRequested_ && state_ == State::Registering &&
          nowMs - stateSinceMs_ > kRegisterTimeoutMs) {
        Z2S_LOG_WARNING("[%s] No reply to registration", name_.c_str());
        closeRequested_ = true;
        scheduleReconnectAfterFailure(nowMs);
      }
      if (!closeRequested_ && state_ == State::Registered) {
        if (layoutChanged_) {
          closeRequested_ = true;
          scheduleReconnect(nowMs, 0);
        } else {
          pushChannelValues(false);
          pushElectricityMeters();
          pushChannelStates();
          pushChannelConfigs();
          pushCaptions();
          pushTimerStates();
          ping(nowMs);
        }
      }
      break;
    }
  }

  if (transport_ && !closeRequested_ && !transport_->flush()) {
    writeFailed_ = true;
  }
  if (writeFailed_ && !closeRequested_) {
    Z2S_LOG_WARNING("[%s] Write failed, reconnecting", name_.c_str());
    closeRequested_ = true;
    scheduleReconnectAfterFailure(nowMs);
  }
  if (closeRequested_) {
    if (transport_) transport_->flush();
    disconnect();
  }
}

}  // namespace z2s
