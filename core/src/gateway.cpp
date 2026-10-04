// SPDX-License-Identifier: GPL-2.0-or-later

#include "z2s/gateway.h"

#include <algorithm>
#include <cctype>
#include <set>

#include "z2s/log.h"

namespace z2s {

namespace {

// Device id ("0x" + 16 lowercase hex digits) of an IEEE address written as in
// zigbee2mqtt ("0xA4C1...") or in Home Assistant ("a4:c1:..."); other entries
// (device names) are returned unchanged.
std::string normalizedIeee(const std::string &entry) {
  std::string hex = entry;
  if (hex.size() > 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
    hex.erase(0, 2);
  }
  hex.erase(std::remove(hex.begin(), hex.end(), ':'), hex.end());
  if (hex.size() != 16) return entry;
  for (char &c : hex) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) return entry;
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return "0x" + hex;
}

}  // namespace

Gateway::Gateway(const GatewayConfig &config, IdentityStore *store,
                 TransportFactory *factory, Backend *backend)
    : config_(config), store_(store), factory_(factory), backend_(backend) {}

bool Gateway::isBridged(const DeviceDescriptor &device) const {
  auto matches = [&device](const std::vector<std::string> &list) {
    return std::any_of(list.begin(), list.end(), [&](const std::string &e) {
      return e == device.name || normalizedIeee(e) == device.id;
    });
  };
  if (matches(config_.exclude)) {
    return false;
  }
  return config_.include.empty() || matches(config_.include);
}

void Gateway::onDeviceList(const std::vector<DeviceDescriptor> &devices) {
  bool storeChanged = false;
  std::set<std::string> seen;

  for (const auto &device : devices) {
    if (device.channels.empty() || !isBridged(device)) {
      continue;
    }
    bool changed = false;
    const DeviceIdentity *identity = store_->reconcile(device, &changed);
    if (identity == nullptr) {
      continue;
    }
    storeChanged |= changed;
    seen.insert(device.id);

    auto &entry = sessions_[device.id];
    if (!entry.session) {
      entry.session = std::make_unique<SuplaSession>(config_.supla, factory_);
      Z2S_LOG_INFO("New bridged device: %s (%s %s), %zu channel(s)",
                   device.name.c_str(), device.manufacturer.c_str(),
                   device.model.c_str(), identity->channels.size());
    }
    entry.session->enable();
    entry.session->configure(*identity, device);

    entry.keyToChannel.clear();
    entry.channelToKey.clear();
    for (size_t i = 0; i < identity->channels.size(); i++) {
      const auto &spec = identity->channels[i];
      auto match =
          std::find_if(device.channels.begin(), device.channels.end(),
                       [&spec](const ChannelSpec &c) {
                         return c.key == spec.key && c.kind == spec.kind;
                       });
      bool present = match != device.channels.end();
      entry.session->setChannelPresent(static_cast<int>(i), present);
      entry.session->setChannelCountdown(static_cast<int>(i),
                                         present ? match->countdownStepMs : 0,
                                         present ? match->countdownMaxMs : 0);
      entry.session->setChannelActionCaps(static_cast<int>(i),
                                          present ? match->actionCaps : 0);
      if (present) {
        entry.keyToChannel[spec.key] = static_cast<int>(i);
        entry.channelToKey[static_cast<int>(i)] = spec.key;
      }
    }

    std::string deviceId = device.id;
    entry.session->setCommandHandler(
        [this, deviceId](int channelNumber, const ChannelCommand &command) {
          auto it = sessions_.find(deviceId);
          if (it == sessions_.end()) return false;
          auto keyIt = it->second.channelToKey.find(channelNumber);
          if (keyIt == it->second.channelToKey.end()) return false;
          return backend_->sendCommand(deviceId, keyIt->second, command);
        });
    applyOnline(entry);
    backend_->requestState(device.id);
  }

  for (auto &[id, entry] : sessions_) {
    if (seen.count(id) == 0 &&
        entry.session->state() != SuplaSession::State::Disabled) {
      Z2S_LOG_INFO("Device %s is no longer bridged, disconnecting",
                   entry.session->name().c_str());
      entry.session->disable();
    }
  }

  if (storeChanged && !store_->save()) {
    Z2S_LOG_ERROR("Failed to save identity store");
  }
}

void Gateway::onChannelState(const std::string &deviceId,
                             const std::string &channelKey,
                             const ChannelState &state) {
  auto it = sessions_.find(deviceId);
  if (it == sessions_.end()) return;
  auto keyIt = it->second.keyToChannel.find(channelKey);
  if (keyIt == it->second.keyToChannel.end()) return;
  Z2S_LOG_DEBUG("[%s] Channel %d: state received (%s)",
                it->second.session->name().c_str(), keyIt->second,
                channelKey.c_str());
  it->second.session->setChannelState(keyIt->second, state);
}

void Gateway::onChannelAction(const std::string &deviceId,
                              const std::string &channelKey, uint32_t action) {
  auto it = sessions_.find(deviceId);
  if (it == sessions_.end()) return;
  auto keyIt = it->second.keyToChannel.find(channelKey);
  if (keyIt == it->second.keyToChannel.end()) return;
  it->second.session->triggerAction(keyIt->second, action);
}

void Gateway::onDeviceHealth(const std::string &deviceId,
                             const DeviceHealth &health) {
  auto it = sessions_.find(deviceId);
  if (it == sessions_.end()) return;
  it->second.session->setDeviceHealth(health);
}

void Gateway::onDeviceAvailability(const std::string &deviceId, bool online) {
  auto it = sessions_.find(deviceId);
  if (it == sessions_.end()) return;
  it->second.deviceOnline = online;
  applyOnline(it->second);
}

void Gateway::onBackendAvailability(bool online) {
  if (backendOnline_ != online) {
    Z2S_LOG_INFO("ZigBee backend is %s", online ? "online" : "offline");
  }
  backendOnline_ = online;
  for (auto &[id, entry] : sessions_) {
    applyOnline(entry);
  }
}

void Gateway::applyOnline(Entry &entry) {
  // A device reported as unreachable is disconnected from Supla, like a real
  // device that was switched off. When the whole backend is down nothing is
  // known about single devices: connections stay and channels go offline, so
  // a restart of the backend does not make every device register again.
  entry.session->setSuspended(!entry.deviceOnline);
  entry.session->setDeviceOnline(backendOnline_ && entry.deviceOnline);
}

void Gateway::iterate(uint64_t nowMs) {
  int connecting = 0;
  for (auto &[id, entry] : sessions_) {
    if (entry.session->isConnecting()) connecting++;
  }

  for (auto &[id, entry] : sessions_) {
    SuplaSession &session = *entry.session;
    if (session.wantsToConnect(nowMs) &&
        connecting < config_.maxParallelConnects &&
        (!anyConnectStarted_ ||
         nowMs - lastConnectStartMs_ >=
             static_cast<uint64_t>(config_.connectIntervalMs))) {
      session.startConnect(nowMs);
      lastConnectStartMs_ = nowMs;
      anyConnectStarted_ = true;
      if (session.isConnecting()) connecting++;
    }
    session.iterate(nowMs);
  }
}

std::vector<Gateway::PollFd> Gateway::pollFds() const {
  std::vector<PollFd> fds;
  for (const auto &[id, entry] : sessions_) {
    int fd = entry.session->fd();
    if (fd >= 0) {
      fds.push_back({fd, entry.session->hasPendingWrite()});
    }
  }
  return fds;
}

size_t Gateway::sessionCount() const {
  size_t count = 0;
  for (const auto &[id, entry] : sessions_) {
    if (entry.session->state() != SuplaSession::State::Disabled) count++;
  }
  return count;
}

size_t Gateway::offlineCount() const {
  size_t count = 0;
  for (const auto &[id, entry] : sessions_) {
    if (entry.session->state() != SuplaSession::State::Disabled &&
        entry.session->isSuspended()) {
      count++;
    }
  }
  return count;
}

size_t Gateway::registeredCount() const {
  size_t count = 0;
  for (const auto &[id, entry] : sessions_) {
    if (entry.session->isRegistered()) count++;
  }
  return count;
}

SuplaSession *Gateway::session(const std::string &deviceId) {
  auto it = sessions_.find(deviceId);
  return it == sessions_.end() ? nullptr : it->second.session.get();
}

}  // namespace z2s
