// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "z2s/backend.h"
#include "z2s/identity_store.h"
#include "z2s/supla_session.h"
#include "z2s/transport.h"

namespace z2s {

struct GatewayConfig {
  SuplaServerConfig supla;
  // Max number of sessions connecting/registering at the same time. TLS
  // handshakes are expensive; with 100+ devices they must be spread in time.
  int maxParallelConnects = 4;
  // Minimum delay between starting two connections.
  int connectIntervalMs = 250;
  // Only devices whose id or name is listed are bridged (empty = all).
  std::vector<std::string> include;
  // Devices whose id or name is listed are never bridged.
  std::vector<std::string> exclude;
};

// Maps backend devices to Supla sessions (one Supla device per ZigBee device).
class Gateway : public BackendListener {
 public:
  Gateway(const GatewayConfig &config, IdentityStore *store,
          TransportFactory *factory, Backend *backend);

  // Drives all sessions. Call frequently (e.g. every 10-50 ms).
  void iterate(uint64_t nowMs);

  // Descriptors used for poll() by the event loop.
  struct PollFd {
    int fd;
    bool wantWrite;
  };
  std::vector<PollFd> pollFds() const;

  // Number of bridged devices (sessions of removed devices not counted).
  size_t sessionCount() const;
  size_t registeredCount() const;
  // Bridged devices that are disconnected from Supla because the backend
  // reports them as unreachable.
  size_t offlineCount() const;
  SuplaSession *session(const std::string &deviceId);

  // BackendListener
  void onDeviceList(const std::vector<DeviceDescriptor> &devices) override;
  void onChannelState(const std::string &deviceId,
                      const std::string &channelKey,
                      const ChannelState &state) override;
  void onChannelAction(const std::string &deviceId,
                       const std::string &channelKey, uint32_t action) override;
  void onDeviceHealth(const std::string &deviceId,
                      const DeviceHealth &health) override;
  void onDeviceAvailability(const std::string &deviceId, bool online) override;
  void onBackendAvailability(bool online) override;

 private:
  struct Entry {
    std::unique_ptr<SuplaSession> session;
    // channel key -> Supla channel number, for channels currently provided
    // by the backend.
    std::map<std::string, int> keyToChannel;
    std::map<int, std::string> channelToKey;
    bool deviceOnline = true;
  };

  bool isBridged(const DeviceDescriptor &device) const;
  void applyOnline(Entry &entry);

  GatewayConfig config_;
  IdentityStore *store_;
  TransportFactory *factory_;
  Backend *backend_;
  std::map<std::string, Entry> sessions_;
  bool backendOnline_ = false;
  uint64_t lastConnectStartMs_ = 0;
  bool anyConnectStarted_ = false;
};

}  // namespace z2s
