// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "z2s/backend.h"
#include "zha_mapper.h"

namespace z2s {
class WebSocketClient;
}

namespace z2s::zha {

struct HaConfig {
  // Home Assistant WebSocket API, e.g. ws://192.168.1.10:8123/api/websocket
  // or ws://supervisor/core/websocket inside a Home Assistant add-on.
  std::string websocketUrl;
  std::string token;  // long-lived access token or SUPERVISOR_TOKEN
  int timeoutMs = 15000;
};

// Backend using ZHA, the ZigBee integration of Home Assistant, through the
// Home Assistant WebSocket API. A worker thread keeps the connection; events
// are handed over to poll().
class ZhaBackend : public Backend {
 public:
  explicit ZhaBackend(const HaConfig &config);
  ~ZhaBackend() override;

  void setListener(BackendListener *listener) override { listener_ = listener; }
  bool start() override;
  void stop() override;
  void poll() override;
  bool sendCommand(const std::string &deviceId, const std::string &channelKey,
                   const ChannelCommand &command) override;
  // Home Assistant keeps the last state of every entity: it comes with each
  // device list, nothing to ask for.
  void requestState(const std::string &) override {}
  size_t deviceCount() const override { return deviceCount_; }

  // Event handlers, called from poll() (public for tests).
  void handleDevices(std::vector<Device> devices, size_t deviceCount,
                     const nlohmann::json &states);
  void handleState(const nlohmann::json &state);
  void handleAction(const std::string &deviceId, const std::string &key,
                    uint32_t action);
  void handleConnection(bool connected);

 protected:
  // Hands service calls over to the worker thread. Virtual so that tests can
  // capture them.
  virtual void sendCalls(const std::vector<ServiceCall> &calls);

 private:
  struct Event {
    enum class Type { Connected, Disconnected, Devices, State, Action };
    explicit Event(Type t, nlohmann::json j = nullptr)
        : type(t), json(std::move(j)) {}
    Type type;
    nlohmann::json json;  // states (Devices) or one state (State)
    std::vector<Device> devices;
    size_t deviceCount = 0;
    std::string deviceId;
    std::string key;
    uint32_t action = 0;
  };

  // A device trigger subscription: Home Assistant reports the trigger with
  // the id of the subscription.
  struct TriggerTarget {
    std::string deviceId;
    std::string key;
    uint32_t action = 0;
  };

  void pushEvent(Event event);
  void threadMain();
  // One connection: returns when it is lost or stop() was called.
  void runSession();
  // Sends a command and waits for its result; events received meanwhile are
  // dispatched. Returns the id of the command (the subscription id of
  // subscribe_* commands), its negative when Home Assistant rejected it, or
  // 0 when the connection is lost.
  int call(WebSocketClient *ws, nlohmann::json command, nlohmann::json *result);
  bool fetchSnapshot(WebSocketClient *ws, Snapshot *snapshot);
  // Fetches the devices, hands them over to poll() and subscribes to the
  // triggers of their buttons.
  bool syncDevices(WebSocketClient *ws);
  void dispatch(const nlohmann::json &msg);
  bool waitFor(int ms);  // false when stop() was called

  HaConfig config_;
  BackendListener *listener_ = nullptr;

  // Main thread (poll()) state.
  size_t deviceCount_ = 0;
  bool connected_ = false;
  std::map<std::string, Device> devices_;              // by device id
  std::map<std::string, std::string> entityToDevice_;  // entity -> device
  std::map<std::string, std::map<std::string, bool>> entityAvailable_;
  std::map<std::string, bool> deviceAvailable_;

  // Shared between the threads.
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stopRequested_ = false;
  std::deque<Event> events_;
  std::deque<ServiceCall> calls_;

  // Worker thread state.
  std::thread thread_;
  int nextId_ = 1;
  int stateSubscription_ = 0;
  std::set<int> registrySubscriptions_;
  std::map<int, TriggerTarget> triggerSubscriptions_;
  std::set<std::string> watchedEntities_;
  bool syncing_ = false;
  std::vector<nlohmann::json> bufferedStates_;
  bool registryChanged_ = false;
  uint64_t registryChangedMs_ = 0;
};

}  // namespace z2s::zha
