// SPDX-License-Identifier: GPL-2.0-or-later

#include "z2s/channel.h"

#include "z2s/backend.h"

#include <proto.h>
#include <srpc.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace z2s {

namespace {
constexpr double kTemperatureNotAvailable = -275.0;
constexpr double kHumidityNotAvailable = -1.0;
constexpr double kPressureNotAvailable = -1.0;

// Approximate period between electricity meter measurements [s]; zigbee2mqtt
// devices report on change, usually every few seconds.
constexpr int kElectricityMeterPeriodS = 10;

template <typename T>
T clampRound(double value, double min, double max) {
  return static_cast<T>(std::lround(std::clamp(value, min, max)));
}
}  // namespace

const char *toString(ChannelKind kind) {
  switch (kind) {
    case ChannelKind::Relay:
      return "relay";
    case ChannelKind::Thermometer:
      return "thermometer";
    case ChannelKind::Humidity:
      return "humidity";
    case ChannelKind::TempHumidity:
      return "temp_humidity";
    case ChannelKind::Pressure:
      return "pressure";
    case ChannelKind::BinarySensor:
      return "binary_sensor";
    case ChannelKind::ElectricityMeter:
      return "electricity_meter";
    case ChannelKind::GeneralPurposeMeasurement:
      return "general_purpose_measurement";
    case ChannelKind::ActionTrigger:
      return "action_trigger";
    case ChannelKind::Thermostat:
      return "thermostat";
  }
  return "unknown";
}

bool channelKindFromInt(int value, ChannelKind *out) {
  switch (value) {
    case static_cast<int>(ChannelKind::Relay):
    case static_cast<int>(ChannelKind::Thermometer):
    case static_cast<int>(ChannelKind::Humidity):
    case static_cast<int>(ChannelKind::TempHumidity):
    case static_cast<int>(ChannelKind::Pressure):
    case static_cast<int>(ChannelKind::BinarySensor):
    case static_cast<int>(ChannelKind::ElectricityMeter):
    case static_cast<int>(ChannelKind::GeneralPurposeMeasurement):
    case static_cast<int>(ChannelKind::ActionTrigger):
    case static_cast<int>(ChannelKind::Thermostat):
      *out = static_cast<ChannelKind>(value);
      return true;
  }
  return false;
}

int32_t suplaChannelType(ChannelKind kind) {
  switch (kind) {
    case ChannelKind::Relay:
      return SUPLA_CHANNELTYPE_RELAY;
    case ChannelKind::Thermometer:
      return SUPLA_CHANNELTYPE_THERMOMETER;
    case ChannelKind::Humidity:
      return SUPLA_CHANNELTYPE_HUMIDITYSENSOR;
    case ChannelKind::TempHumidity:
      return SUPLA_CHANNELTYPE_HUMIDITYANDTEMPSENSOR;
    case ChannelKind::Pressure:
      return SUPLA_CHANNELTYPE_PRESSURESENSOR;
    case ChannelKind::BinarySensor:
      return SUPLA_CHANNELTYPE_BINARYSENSOR;
    case ChannelKind::ElectricityMeter:
      return SUPLA_CHANNELTYPE_ELECTRICITY_METER;
    case ChannelKind::GeneralPurposeMeasurement:
      return SUPLA_CHANNELTYPE_GENERAL_PURPOSE_MEASUREMENT;
    case ChannelKind::ActionTrigger:
      return SUPLA_CHANNELTYPE_ACTIONTRIGGER;
    case ChannelKind::Thermostat:
      return SUPLA_CHANNELTYPE_HVAC;
  }
  return 0;
}

int32_t suplaFuncList(const ChannelSpec &spec) {
  if (spec.kind == ChannelKind::Relay) {
    int32_t list = SUPLA_BIT_FUNC_POWERSWITCH | SUPLA_BIT_FUNC_LIGHTSWITCH;
    // The staircase timer is a turn-on with a duration set in Supla Cloud.
    if (spec.countdownStepMs != 0) list |= SUPLA_BIT_FUNC_STAIRCASETIMER;
    return list;
  }
  // Same field of the registration (ActionTriggerCaps).
  if (spec.kind == ChannelKind::ActionTrigger) {
    return static_cast<int32_t>(spec.actionCaps);
  }
  if (spec.kind == ChannelKind::Thermostat) {
    return SUPLA_BIT_FUNC_HVAC_THERMOSTAT;
  }
  return 0;
}

uint64_t suplaChannelFlags(const ChannelSpec &spec) {
  // Battery and link quality of the device (SuplaSession answers
  // GET_CHANNEL_STATE).
  uint64_t flags = SUPLA_CHANNEL_FLAG_CHANNELSTATE;
  if (spec.kind == ChannelKind::Relay && spec.countdownStepMs != 0) {
    flags |= SUPLA_CHANNEL_FLAG_COUNTDOWN_TIMER_SUPPORTED;
  }
  if (spec.kind == ChannelKind::Thermostat) {
    // Supla apps need a weekly schedule to show the thermostat controls; it
    // is kept on the server, the device runs its own program.
    flags |= SUPLA_CHANNEL_FLAG_WEEKLY_SCHEDULE;
  }
  if (spec.kind == ChannelKind::GeneralPurposeMeasurement ||
      spec.kind == ChannelKind::Thermostat) {
    // The server sends the channel config after registration, so the
    // gateway can give it the default unit (SuplaSession).
    flags |= SUPLA_CHANNEL_FLAG_RUNTIME_CHANNEL_CONFIG_UPDATE;
  }
  if (spec.kind == ChannelKind::ElectricityMeter) {
    // zigbee2mqtt plugs measure a single phase.
    flags |= SUPLA_CHANNEL_FLAG_PHASE2_UNSUPPORTED |
             SUPLA_CHANNEL_FLAG_PHASE3_UNSUPPORTED;
  }
  return flags;
}

int32_t suplaDefaultFunction(const ChannelSpec &spec) {
  if (spec.defaultFunction != 0) {
    return spec.defaultFunction;
  }
  switch (spec.kind) {
    case ChannelKind::Relay:
      return SUPLA_CHANNELFNC_POWERSWITCH;
    case ChannelKind::Thermometer:
      return SUPLA_CHANNELFNC_THERMOMETER;
    case ChannelKind::Humidity:
      return SUPLA_CHANNELFNC_HUMIDITY;
    case ChannelKind::TempHumidity:
      return SUPLA_CHANNELFNC_HUMIDITYANDTEMPERATURE;
    case ChannelKind::Pressure:
      return SUPLA_CHANNELFNC_PRESSURESENSOR;
    case ChannelKind::BinarySensor:
      return SUPLA_CHANNELFNC_BINARY_SENSOR;
    case ChannelKind::ElectricityMeter:
      return SUPLA_CHANNELFNC_ELECTRICITY_METER;
    case ChannelKind::GeneralPurposeMeasurement:
      return SUPLA_CHANNELFNC_GENERAL_PURPOSE_MEASUREMENT;
    case ChannelKind::ActionTrigger:
      return SUPLA_CHANNELFNC_ACTIONTRIGGER;
    case ChannelKind::Thermostat:
      return SUPLA_CHANNELFNC_HVAC_THERMOSTAT;
  }
  return 0;
}

bool isEmpty(const ChannelState &state) {
  return std::isnan(state.primary) && std::isnan(state.secondary) &&
         std::isnan(state.power) && std::isnan(state.voltage) &&
         std::isnan(state.current) && std::isnan(state.mode) &&
         std::isnan(state.heating) && std::isnan(state.valve) &&
         std::isnan(state.program);
}

bool encodeElectricityMeterExtendedValue(const ChannelState &state,
                                         TSuplaChannelExtendedValue *out) {
  if (std::isnan(state.primary)) return false;
  TElectricityMeter_ExtendedValue_V3 em = {};
  em.total_forward_active_energy[0] = clampRound<unsigned _supla_int64_t>(
      state.primary * 100000.0, 0,
      static_cast<double>(std::numeric_limits<int64_t>::max()));
  em.measured_values = EM_VAR_FORWARD_ACTIVE_ENERGY;
  em.period = kElectricityMeterPeriodS;
  em.m_count = 1;
  TElectricityMeter_Measurement &m = em.m[0];
  if (!std::isnan(state.voltage)) {
    m.voltage[0] =
        clampRound<unsigned _supla_int16_t>(state.voltage * 100.0, 0, 65535);
    em.measured_values |= EM_VAR_VOLTAGE;
  }
  if (!std::isnan(state.current)) {
    if (state.current * 1000.0 > 65535) {
      m.current[0] =
          clampRound<unsigned _supla_int16_t>(state.current * 100.0, 0, 65535);
      em.measured_values |= EM_VAR_CURRENT_OVER_65A;
    } else {
      m.current[0] =
          clampRound<unsigned _supla_int16_t>(state.current * 1000.0, 0, 65535);
      em.measured_values |= EM_VAR_CURRENT;
    }
  }
  if (!std::isnan(state.power)) {
    constexpr double kMax = std::numeric_limits<int32_t>::max();
    double power = state.power * 100000.0;
    if (std::fabs(power) > kMax) {
      m.power_active[0] = clampRound<_supla_int_t>(power / 1000.0, -kMax, kMax);
      em.measured_values |= EM_VAR_POWER_ACTIVE | EM_VAR_POWER_ACTIVE_KW;
    } else {
      m.power_active[0] = clampRound<_supla_int_t>(power, -kMax, kMax);
      em.measured_values |= EM_VAR_POWER_ACTIVE;
    }
  }
  return srpc_evtool_v3_emextended2extended(&em, out) != 0;
}

void encodeChannelValue(ChannelKind kind, const ChannelState &state,
                        char out[8]) {
  memset(out, 0, SUPLA_CHANNELVALUE_SIZE);
  switch (kind) {
    case ChannelKind::Relay:
    case ChannelKind::BinarySensor:
      out[0] = (!std::isnan(state.primary) && state.primary > 0.5) ? 1 : 0;
      break;
    case ChannelKind::Thermometer: {
      double t =
          std::isnan(state.primary) ? kTemperatureNotAvailable : state.primary;
      memcpy(out, &t, sizeof(t));
      break;
    }
    case ChannelKind::Pressure: {
      double p =
          std::isnan(state.primary) ? kPressureNotAvailable : state.primary;
      memcpy(out, &p, sizeof(p));
      break;
    }
    case ChannelKind::Humidity:
    case ChannelKind::TempHumidity: {
      double temp = kTemperatureNotAvailable;
      double humi = kHumidityNotAvailable;
      if (kind == ChannelKind::Humidity) {
        if (!std::isnan(state.primary)) humi = state.primary;
      } else {
        if (!std::isnan(state.primary)) temp = state.primary;
        if (!std::isnan(state.secondary)) humi = state.secondary;
      }
      int32_t t = static_cast<int32_t>(std::lround(temp * 1000.0));
      int32_t h = static_cast<int32_t>(std::lround(humi * 1000.0));
      memcpy(out, &t, 4);
      memcpy(out + 4, &h, 4);
      break;
    }
    case ChannelKind::ElectricityMeter: {
      TElectricityMeter_Value em = {};
      if (!std::isnan(state.primary)) {
        em.total_forward_active_energy = clampRound<unsigned _supla_int_t>(
            state.primary * 100.0, 0, std::numeric_limits<uint32_t>::max());
      }
      if (!std::isnan(state.voltage) && state.voltage > 0) {
        em.flags |= EM_VALUE_FLAG_PHASE1_ON;
      }
      memcpy(out, &em, sizeof(em));
      break;
    }
    case ChannelKind::ActionTrigger:
      // TActionTriggerProperties: no related channel.
      break;
    case ChannelKind::Thermostat: {
      THVACValue hvac = {};
      bool on = !std::isnan(state.mode) && state.mode > 0.5;
      hvac.Mode = std::isnan(state.mode)
                      ? SUPLA_HVAC_MODE_NOT_SET
                      : (on ? SUPLA_HVAC_MODE_HEAT : SUPLA_HVAC_MODE_OFF);
      bool heating = !std::isnan(state.heating) && state.heating > 0.5;
      if (!std::isnan(state.valve)) {
        // 2..102 = output 0..100 % (valve opening of a radiator head).
        hvac.IsOn = static_cast<unsigned char>(
            2 + std::lround(std::clamp(state.valve, 0.0, 100.0)));
      } else {
        hvac.IsOn = heating ? 1 : 0;
      }
      if (heating) hvac.Flags |= SUPLA_HVAC_VALUE_FLAG_HEATING;
      if (!std::isnan(state.program) && state.program > 0.5) {
        hvac.Flags |= SUPLA_HVAC_VALUE_FLAG_WEEKLY_SCHEDULE;
      }
      if (!std::isnan(state.primary)) {
        hvac.SetpointTemperatureHeat =
            clampRound<_supla_int16_t>(state.primary * 100.0, -32768, 32767);
        hvac.Flags |= SUPLA_HVAC_VALUE_FLAG_SETPOINT_TEMP_HEAT_SET;
      }
      memcpy(out, &hvac, sizeof(hvac));
      break;
    }
    case ChannelKind::GeneralPurposeMeasurement: {
      // NaN = no reading, as in supla-device.
      double v = state.primary;
      memcpy(out, &v, sizeof(v));
      break;
    }
  }
}

DeviceHealth mergeHealth(const DeviceHealth &current,
                         const DeviceHealth &update) {
  DeviceHealth result = current;
  if (update.batteryLevel >= 0) result.batteryLevel = update.batteryLevel;
  if (update.batteryLow >= 0) result.batteryLow = update.batteryLow;
  if (update.linkQuality >= 0) result.linkQuality = update.linkQuality;
  return result;
}

ChannelState mergeState(const ChannelState &current,
                        const ChannelState &update) {
  ChannelState result = current;
  if (!std::isnan(update.primary)) result.primary = update.primary;
  if (!std::isnan(update.secondary)) result.secondary = update.secondary;
  if (!std::isnan(update.power)) result.power = update.power;
  if (!std::isnan(update.voltage)) result.voltage = update.voltage;
  if (!std::isnan(update.current)) result.current = update.current;
  if (!std::isnan(update.mode)) result.mode = update.mode;
  if (!std::isnan(update.heating)) result.heating = update.heating;
  if (!std::isnan(update.valve)) result.valve = update.valve;
  if (!std::isnan(update.program)) result.program = update.program;
  return result;
}

}  // namespace z2s
