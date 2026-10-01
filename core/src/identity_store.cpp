// SPDX-License-Identifier: GPL-2.0-or-later

#include "z2s/identity_store.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

#include "z2s/log.h"

namespace z2s {

using json = nlohmann::json;

std::string toHex(const uint8_t *data, size_t len) {
  static const char digits[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(len * 2);
  for (size_t i = 0; i < len; i++) {
    out.push_back(digits[data[i] >> 4]);
    out.push_back(digits[data[i] & 0x0F]);
  }
  return out;
}

bool fromHex(const std::string &hex, uint8_t *out, size_t len) {
  if (hex.size() != len * 2) {
    return false;
  }
  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
  };
  for (size_t i = 0; i < len; i++) {
    int hi = nibble(hex[i * 2]);
    int lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i] = static_cast<uint8_t>((hi << 4) | lo);
  }
  return true;
}

IdentityStore::IdentityStore(std::string path, RandomBytesFn randomBytes)
    : path_(std::move(path)), randomBytes_(std::move(randomBytes)) {}

bool IdentityStore::load() {
  identities_.clear();
  std::ifstream in(path_);
  if (!in.is_open()) {
    Z2S_LOG_INFO("Identity store %s not found, starting empty", path_.c_str());
    return true;
  }

  json root;
  try {
    in >> root;
  } catch (const std::exception &e) {
    Z2S_LOG_ERROR("Identity store %s is corrupted: %s", path_.c_str(),
                  e.what());
    return false;
  }

  if (!root.is_object() || !root.contains("devices") ||
      !root["devices"].is_object()) {
    Z2S_LOG_ERROR("Identity store %s has invalid format", path_.c_str());
    return false;
  }

  for (auto &[id, entry] : root["devices"].items()) {
    DeviceIdentity identity;
    identity.id = id;
    if (!fromHex(entry.value("guid", ""), identity.guid.data(),
                 identity.guid.size()) ||
        !fromHex(entry.value("authkey", ""), identity.authKey.data(),
                 identity.authKey.size())) {
      Z2S_LOG_ERROR("Identity store: invalid GUID/AuthKey for %s", id.c_str());
      return false;
    }
    for (const auto &ch : entry.value("channels", json::array())) {
      ChannelSpec spec;
      if (!channelKindFromInt(ch.value("kind", 0), &spec.kind)) {
        Z2S_LOG_ERROR("Identity store: unknown channel kind for %s",
                      id.c_str());
        return false;
      }
      spec.key = ch.value("key", "");
      spec.defaultFunction = ch.value("function", 0);
      spec.caption = ch.value("caption", "");
      identity.channels.push_back(spec);
    }
    identities_[id] = std::move(identity);
  }
  Z2S_LOG_INFO("Identity store loaded: %zu device(s)", identities_.size());
  return true;
}

bool IdentityStore::save() const {
  json devices = json::object();
  for (const auto &[id, identity] : identities_) {
    json channels = json::array();
    for (const auto &spec : identity.channels) {
      channels.push_back({{"kind", static_cast<int>(spec.kind)},
                          {"key", spec.key},
                          {"function", spec.defaultFunction},
                          {"caption", spec.caption}});
    }
    devices[id] = {
        {"guid", toHex(identity.guid.data(), identity.guid.size())},
        {"authkey", toHex(identity.authKey.data(), identity.authKey.size())},
        {"channels", channels}};
  }
  json root = {{"version", 1}, {"devices", devices}};
  std::string data = root.dump(2) + "\n";

  std::string tmpPath = path_ + ".tmp";
  // The file contains AuthKeys: create it readable by the owner only.
  int fd = ::open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) {
    Z2S_LOG_ERROR("Cannot write identity store %s", tmpPath.c_str());
    return false;
  }
  const char *ptr = data.data();
  size_t left = data.size();
  while (left > 0) {
    ssize_t written = ::write(fd, ptr, left);
    if (written <= 0) {
      ::close(fd);
      Z2S_LOG_ERROR("Write to %s failed", tmpPath.c_str());
      return false;
    }
    ptr += written;
    left -= static_cast<size_t>(written);
  }
  ::fsync(fd);
  ::close(fd);
  if (std::rename(tmpPath.c_str(), path_.c_str()) != 0) {
    Z2S_LOG_ERROR("Cannot rename %s to %s", tmpPath.c_str(), path_.c_str());
    return false;
  }
  return true;
}

const DeviceIdentity *IdentityStore::reconcile(const DeviceDescriptor &device,
                                               bool *changed) {
  *changed = false;
  auto it = identities_.find(device.id);
  if (it == identities_.end()) {
    DeviceIdentity identity;
    identity.id = device.id;
    if (!randomBytes_(identity.guid.data(), identity.guid.size()) ||
        !randomBytes_(identity.authKey.data(), identity.authKey.size())) {
      Z2S_LOG_ERROR("Failed to generate GUID/AuthKey for %s",
                    device.id.c_str());
      return nullptr;
    }
    it = identities_.emplace(device.id, std::move(identity)).first;
    *changed = true;
  }

  DeviceIdentity &identity = it->second;
  for (const auto &spec : device.channels) {
    bool known = false;
    for (auto &existing : identity.channels) {
      if (existing.key == spec.key && existing.kind == spec.kind) {
        known = true;
        break;
      }
    }
    if (known) {
      continue;
    }
    if (identity.channels.size() >= kMaxChannels) {
      Z2S_LOG_WARNING("Device %s: channel limit reached, skipping %s",
                      device.id.c_str(), spec.key.c_str());
      continue;
    }
    identity.channels.push_back(spec);
    *changed = true;
  }
  return &identity;
}

const DeviceIdentity *IdentityStore::find(const std::string &id) const {
  auto it = identities_.find(id);
  return it == identities_.end() ? nullptr : &it->second;
}

}  // namespace z2s
