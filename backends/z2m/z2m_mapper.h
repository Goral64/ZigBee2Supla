// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "z2s/backend.h"

namespace z2s::z2m {

// How one Supla channel is bound to zigbee2mqtt properties.
struct Binding {
  ChannelSpec spec;
  std::string property;           // e.g. "state_l1", "temperature"
  std::string secondaryProperty;  // e.g. "humidity" for TempHumidity
  nlohmann::json valueOn = "ON";
  nlohmann::json valueOff = "OFF";
  // Channel value is 1 when the property equals valueOff (e.g. "contact":
  // zigbee2mqtt reports contact=true for a closed door, while its expose
  // defines value_on=false; Supla opening sensors use 1 for "closed").
  bool invert = false;
  // BinarySensor from an enum property: 1 when the value contains any of
  // these words, 0 for any other value. Empty: valueOn/valueOff are used.
  std::vector<std::string> enumOnWords;
  // Relay only: the device's own turn-on timer (ChannelSpec::countdown*).
  std::string timerProperty;  // e.g. "countdown", "countdown_l1", "timer"
  uint32_t timerUnitMs = 0;   // 1000 when set in seconds, 60000 in minutes
  // true: the timer is an auto-off duration set before turning on ("timer"
  // of watering timers); false: a countdown started after turning on
  // ("countdown" of plugs and switches).
  bool timerBeforeOn = false;
  // ElectricityMeter only (property = "energy"); empty when not exposed.
  std::string powerProperty;
  std::string voltageProperty;
  std::string currentProperty;
  // ActionTrigger only: zigbee2mqtt "action" value -> SUPLA_ACTION_CAP_*.
  std::map<std::string, uint32_t> actions;
  // Thermostat only (property = "current_heating_setpoint").
  std::string modeProperty;     // "system_mode"
  std::string modeOff;          // its value for "off" (empty: none)
  std::string modeHeat;         // its value for heating
  std::string presetProperty;   // "preset", empty when not settable
  std::string presetManual;     // "manual" or "hold": keeps the setpoint
  std::string presetProgram;    // "schedule" or "program": own schedule
  std::string runningProperty;  // "running_state"
  std::string valveProperty;    // "position" of radiator heads
};

struct Device {
  DeviceDescriptor descriptor;
  std::string friendlyName;
  std::vector<Binding> bindings;
};

// Returns the kind of a device whose main function is not supported by the
// gateway yet ("thermostat" without a settable heating setpoint, "cover",
// "lock", "fan"), or nullptr. Such devices
// are not bridged at all: their secondary switches and sensors alone would
// create a channel layout that cannot be changed once the main function gets
// supported. `device` is one element of the "<base>/bridge/devices" payload.
const char *unsupportedDeviceKind(const nlohmann::json &device);

// Parses the retained "<base>/bridge/devices" payload. Skips the coordinator,
// disabled, unsupported and not yet interviewed devices, devices of a kind
// that is not supported yet (see unsupportedDeviceKind()) and devices without
// any exposed feature supported by the gateway.
std::vector<Device> parseBridgeDevices(const nlohmann::json &devices);

// Extracts channel updates from a "<base>/<friendly_name>" state payload.
std::vector<std::pair<std::string, ChannelState>> extractStates(
    const Device &device, const nlohmann::json &payload);

// Extracts the device health ("battery", "battery_low", "linkquality") from
// a state payload. Empty when the payload has none of them.
std::optional<DeviceHealth> extractHealth(const nlohmann::json &payload);

// Extracts button actions ("action" property) from a state payload as
// (channel key, SUPLA_ACTION_CAP_*) pairs.
std::vector<std::pair<std::string, uint32_t>> extractActions(
    const Device &device, const nlohmann::json &payload);

// Builds the payloads for "<base>/<friendly_name>/set", to be published in
// order (a turn-on with a timer needs two messages). Empty when the channel
// is unknown or cannot be controlled.
std::vector<nlohmann::json> buildCommand(const Device &device,
                                         const std::string &channelKey,
                                         const ChannelCommand &command);

// Parses "<base>/<friendly_name>/availability" and "<base>/bridge/state"
// payloads: {"state":"online"} or legacy plain "online"/"offline".
std::optional<bool> parseAvailability(const std::string &payload);

}  // namespace z2s::z2m
