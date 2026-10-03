// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "z2s/backend.h"

namespace z2s::zha {

// What Home Assistant knows about its ZHA devices, as returned by its
// WebSocket API.
struct Snapshot {
  nlohmann::json deviceRegistry = nlohmann::json::array();  // device_registry
  nlohmann::json entityRegistry = nlohmann::json::array();  // entity_registry
  nlohmann::json states = nlohmann::json::array();          // get_states
  nlohmann::json zhaDevices = nlohmann::json::array();      // zha/devices
  // device_automation/trigger/list result by HA device id.
  std::map<std::string, nlohmann::json> triggers;
};

// How one Supla channel is bound to Home Assistant entities. Channel keys
// are the zigbee2mqtt property names of the same feature, so a device keeps
// its Supla channels when it is moved between ZHA and zigbee2mqtt.
struct Binding {
  ChannelSpec spec;
  std::string entityId;           // e.g. "switch.plug"
  std::string secondaryEntityId;  // humidity of TempHumidity
  // Channel value is 1 when the entity is "off" (opening sensors: HA "on"
  // means open, Supla uses 1 for "closed").
  bool invert = false;
  // ElectricityMeter only (entityId = energy).
  std::string powerEntityId;
  std::string voltageEntityId;
  std::string currentEntityId;
  // ActionTrigger only: device trigger subtype (e.g. "button_1") and its
  // trigger types (e.g. "remote_button_short_press") -> SUPLA_ACTION_CAP_*.
  std::string triggerSubtype;
  std::map<std::string, uint32_t> triggers;
  // Thermostat only (entityId = climate entity, which also gives the room
  // temperature of the thermometer channel).
  bool canTurnOff = false;    // "off" in hvac_modes
  std::string presetManual;   // preset that keeps the setpoint ("none")
  std::string presetProgram;  // the device's own schedule ("Schedule")
};

struct Device {
  DeviceDescriptor descriptor;
  std::string haDeviceId;
  std::vector<Binding> bindings;
  std::string batteryEntityId;     // sensor, battery level [%]
  std::string batteryLowEntityId;  // binary_sensor, low battery
};

// A Home Assistant service call.
struct ServiceCall {
  std::string domain;
  std::string service;
  nlohmann::json data;  // service_data, with entity_id
};

// Converts a ZHA IEEE address ("00:12:4b:00:22:e9:5c:cf") to the
// zigbee2mqtt form used as the device id ("0x00124b0022e95ccf").
std::string deviceIdFromIeee(const std::string &ieee);

// Returns the kind of a device whose main function is not supported by the
// gateway yet ("thermostat" for climate devices that cannot heat, "cover",
// "lock", "fan"), or nullptr. Such devices are not bridged at all, as with
// zigbee2mqtt.
const char *unsupportedDeviceKind(const Snapshot &snapshot,
                                  const std::string &haDeviceId);

// Builds the devices that can be bridged: ZHA devices that are enabled,
// supported and have at least one entity or button the gateway can map.
std::vector<Device> parseDevices(const Snapshot &snapshot);

// Ids (HA device ids) of all ZHA devices except the coordinator.
std::vector<std::string> zhaDeviceIds(const Snapshot &snapshot);

// Channel updates from a Home Assistant state object ({"entity_id", "state",
// "attributes"}) of one of the device's entities.
std::vector<std::pair<std::string, ChannelState>> extractStates(
    const Device &device, const nlohmann::json &state);

// Battery from a state object of the device's battery entities.
std::optional<DeviceHealth> extractHealth(const Device &device,
                                          const nlohmann::json &state);

// Availability from a state object: false for "unavailable".
bool isAvailable(const nlohmann::json &state);

// All entities of the device the gateway listens to.
std::vector<std::string> entityIds(const Device &device);

// Service calls for a command, in order. Empty when the channel is unknown
// or the command cannot be executed by the device.
std::vector<ServiceCall> buildCommand(const Device &device,
                                      const std::string &channelKey,
                                      const ChannelCommand &command);

}  // namespace z2s::zha
