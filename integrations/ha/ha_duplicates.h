// SPDX-License-Identifier: GPL-2.0-or-later
//
// Detection of duplicated devices in Home Assistant.
//
// With MQTT enabled in Supla Cloud, supla-server publishes Home Assistant
// MQTT discovery for every Supla channel, including devices created by
// zigbee2supla. Those ZigBee devices are already in Home Assistant through
// zigbee2mqtt, so their Supla copies are duplicates.
//
// A Home Assistant device comes from Supla MQTT discovery when one of its
// identifiers is ("mqtt", "supla-iodevice-<id>"); it comes from zigbee2supla
// when its sw_version (Supla device SoftVer) starts with "z2s ".
#pragma once

#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <vector>

namespace z2s::ha {

struct DuplicateDevice {
  std::string id;    // Home Assistant device registry id
  std::string name;  // for logs
};

// Prefix of the SoftVer sent by zigbee2supla (SuplaServerConfig::softVersion).
extern const char kSoftVersionPrefix[];

bool isSuplaDeviceFromGateway(const nlohmann::json &device);

// Returns devices from the `config/device_registry/list` result that should
// be disabled: Supla copies of gateway devices that are enabled and were not
// handled before (a device the user enabled again is left alone).
std::vector<DuplicateDevice> selectDevicesToDisable(
    const nlohmann::json &devices, const std::set<std::string> &handled);

}  // namespace z2s::ha
