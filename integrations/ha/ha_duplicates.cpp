// SPDX-License-Identifier: GPL-2.0-or-later

#include "ha_duplicates.h"

namespace z2s::ha {

using json = nlohmann::json;

const char kSoftVersionPrefix[] = "z2s ";

namespace {

std::string stringField(const json &obj, const char *key) {
  auto it = obj.find(key);
  return it != obj.end() && it->is_string() ? it->get<std::string>() : "";
}

}  // namespace

bool isSuplaDeviceFromGateway(const json &device) {
  if (!device.is_object()) return false;
  if (stringField(device, "sw_version").rfind(kSoftVersionPrefix, 0) != 0) {
    return false;
  }
  auto ids = device.find("identifiers");
  if (ids == device.end() || !ids->is_array()) return false;
  for (const auto &id : *ids) {
    if (id.is_array() && id.size() == 2 && id[0] == "mqtt" &&
        id[1].is_string() &&
        id[1].get<std::string>().rfind("supla-iodevice-", 0) == 0) {
      return true;
    }
  }
  return false;
}

std::vector<DuplicateDevice> selectDevicesToDisable(
    const json &devices, const std::set<std::string> &handled) {
  std::vector<DuplicateDevice> result;
  if (!devices.is_array()) return result;
  for (const auto &device : devices) {
    if (!isSuplaDeviceFromGateway(device)) continue;
    std::string id = stringField(device, "id");
    if (id.empty() || handled.count(id) > 0) continue;
    if (device.contains("disabled_by") && !device["disabled_by"].is_null()) {
      continue;
    }
    std::string name = stringField(device, "name_by_user");
    if (name.empty()) name = stringField(device, "name");
    result.push_back({id, name});
  }
  return result;
}

}  // namespace z2s::ha
