// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace z2s {

struct HaSyncConfig {
  bool enabled = false;
  // Home Assistant WebSocket API, e.g. ws://192.168.1.10:8123/api/websocket
  // or ws://supervisor/core/websocket inside a Home Assistant add-on.
  std::string websocketUrl;
  std::string token;  // long-lived access token or SUPERVISOR_TOKEN
  std::string stateFile;
  int intervalS = 300;
  int firstDelayS = 60;
  int timeoutMs = 15000;
};

// Periodically disables, in the Home Assistant device registry, the Supla
// MQTT discovery copies of devices bridged by zigbee2supla (they duplicate
// the zigbee2mqtt devices). Every disabled device is remembered, so a device
// enabled again by the user is not disabled a second time.
class HaDuplicateDisabler {
 public:
  explicit HaDuplicateDisabler(const HaSyncConfig &config);
  ~HaDuplicateDisabler();

  // One synchronization. Returns the number of disabled devices, or -1 and
  // fills *error.
  int runOnce(std::string *error);

  void start();
  void stop();

 private:
  void loadState();
  bool saveState() const;
  void threadMain();

  HaSyncConfig config_;
  std::set<std::string> handled_;
  int nextRequestId_ = 1;

  std::thread thread_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stopRequested_ = false;
};

}  // namespace z2s
