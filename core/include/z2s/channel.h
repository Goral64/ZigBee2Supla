// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

#include <proto.h>

namespace z2s {

// Kinds of Supla channels the gateway can expose. The numeric values are
// persisted in the identity store, so never renumber existing entries.
enum class ChannelKind : int {
  Relay = 1,
  Thermometer = 2,
  Humidity = 3,
  TempHumidity = 4,
  Pressure = 5,
  BinarySensor = 6,
  ElectricityMeter = 7,
  GeneralPurposeMeasurement = 8,
  ActionTrigger = 9,
  Thermostat = 10,
};

const char *toString(ChannelKind kind);
bool channelKindFromInt(int value, ChannelKind *out);

// Static description of a single channel, provided by a backend.
struct ChannelSpec {
  ChannelKind kind = ChannelKind::BinarySensor;
  // Backend specific, stable identifier of the channel inside its device
  // (for zigbee2mqtt: the property name, e.g. "state_l1").
  std::string key;
  // SUPLA_CHANNELFNC_* used as the initial function, 0 = server default.
  int32_t defaultFunction = 0;
  // What the channel is, e.g. "przekaźnik L1" or "kontaktron". The initial
  // channel caption in Supla is "<device name> – <caption>"; the server keeps
  // a caption given by the user. Taken from the backend on every device list
  // (the value stored in identities.json is not used).
  std::string caption;

  // Turn-on timer of the device itself (Relay only): the device switches
  // itself off after a time given with the turn-on command. The timer runs in
  // the device, not in the gateway. 0 = not supported. The duration requested
  // in Supla is rounded up to whole steps and must not exceed the maximum.
  // Runtime capability reported by the backend: not stored in the identity
  // store and not part of the channel layout.
  uint32_t countdownStepMs = 0;
  uint32_t countdownMaxMs = 0;

  // GeneralPurposeMeasurement only: default unit (e.g. "lx") and number of
  // decimal places shown in Supla, used when the user has not set their own.
  // Like the caption, taken from the backend on every device list.
  std::string unit;
  int precision = 0;

  // ActionTrigger only: SUPLA_ACTION_CAP_* the button can report. Runtime
  // capability like the countdown above.
  uint32_t actionCaps = 0;

  // Thermostat only: key of the thermometer channel of the same device that
  // measures the room (the main thermometer in Supla) and the setpoint range
  // of the device [°C]. Like the caption, taken from the backend on every
  // device list.
  std::string thermometerKey;
  double setpointMin = NAN;
  double setpointMax = NAN;
  double setpointStep = NAN;

  bool operator==(const ChannelSpec &o) const {
    return kind == o.kind && key == o.key &&
           defaultFunction == o.defaultFunction;
  }
};

// Current measurement/state of a channel. Unused fields stay NaN.
//  Relay, BinarySensor: primary = 0 or 1
//  Thermometer:         primary = temperature [°C]
//  Humidity:            primary = humidity [%]
//  TempHumidity:        primary = temperature [°C], secondary = humidity [%]
//  Pressure:            primary = pressure [hPa]
//  ElectricityMeter:    primary = forward active energy [kWh], power [W],
//                       voltage [V], current [A]
//  GeneralPurposeMeasurement: primary = measured value
//  Thermostat:          primary = setpoint [°C], mode (0 off, 1 heat),
//                       heating (0/1), valve (opening of the valve [%]),
//                       program (1 = the device runs its own schedule)
struct ChannelState {
  double primary = NAN;
  double secondary = NAN;
  double power = NAN;
  double voltage = NAN;
  double current = NAN;
  double mode = NAN;
  double heating = NAN;
  double valve = NAN;
  double program = NAN;
};

// True when no field of the state is known.
bool isEmpty(const ChannelState &state);

// Command sent from Supla to a device channel.
struct ChannelCommand {
  enum class Type { TurnOn, TurnOff, SetThermostat } type = Type::TurnOff;
  // TurnOn only: the device turns itself off after this time (a multiple of
  // ChannelSpec::countdownStepMs); 0 = stay on.
  uint32_t durationMs = 0;
  // SetThermostat only. Mode: 0 = unchanged, 1 = off, 2 = heat. Setpoint
  // [°C], NaN = unchanged. `manual`: leave the device's own schedule or
  // preset, so that it keeps the setpoint from Supla. `program`: switch to
  // the device's own schedule (the "program" mode in Supla).
  int mode = 0;
  double setpoint = NAN;
  bool manual = false;
  bool program = false;
};

// Supla wire level channel parameters (SUPLA_CHANNELTYPE_* etc.).
int32_t suplaChannelType(ChannelKind kind);
int32_t suplaFuncList(const ChannelSpec &spec);
// SUPLA_CHANNEL_FLAG_* declared at registration.
uint64_t suplaChannelFlags(const ChannelSpec &spec);
int32_t suplaDefaultFunction(const ChannelSpec &spec);

// Encodes an ElectricityMeter state into the extended value
// (EV_TYPE_ELECTRICITY_METER_MEASUREMENT_V3, one phase). False when the
// energy is not known yet.
bool encodeElectricityMeterExtendedValue(const ChannelState &state,
                                         TSuplaChannelExtendedValue *out);

// Encodes a channel state into the 8-byte Supla channel value.
void encodeChannelValue(ChannelKind kind, const ChannelState &state,
                        char out[8]);

// Merges a partial update into an existing state (NaN fields are kept).
ChannelState mergeState(const ChannelState &current,
                        const ChannelState &update);

}  // namespace z2s
