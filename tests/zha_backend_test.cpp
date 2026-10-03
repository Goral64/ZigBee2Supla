// SPDX-License-Identifier: GPL-2.0-or-later
// ZhaBackend: event handling, and the whole backend against an in-process
// fake Home Assistant WebSocket API with the devices of zha_snapshot.json.

#include "zha_backend.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <poll.h>
#include <proto.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>

#include "fake_websocket.h"
#include "test_util.h"

using namespace z2s;
using namespace z2s::zha;
using namespace z2s::test;
using json = nlohmann::json;

namespace {

const bool kSigpipeIgnored = [] {
  signal(SIGPIPE, SIG_IGN);
  return true;
}();

class RecordingListener : public BackendListener {
 public:
  void onDeviceList(const std::vector<DeviceDescriptor> &d) override {
    devices = d;
    deviceListCount++;
  }
  void onChannelState(const std::string &id, const std::string &key,
                      const ChannelState &state) override {
    states.push_back({id, key, state});
  }
  void onChannelAction(const std::string &id, const std::string &key,
                       uint32_t action) override {
    actions.push_back({id, key, action});
  }
  void onDeviceHealth(const std::string &id,
                      const DeviceHealth &health) override {
    healths.push_back({id, health});
  }
  void onDeviceAvailability(const std::string &id, bool online) override {
    availability[id] = online;
    availabilityCount++;
  }
  void onBackendAvailability(bool online) override { backendOnline = online; }

  struct State {
    std::string id, key;
    ChannelState state;
  };
  struct Action {
    std::string id, key;
    uint32_t action;
  };
  std::vector<DeviceDescriptor> devices;
  int deviceListCount = 0;
  std::vector<State> states;
  std::vector<Action> actions;
  std::vector<std::pair<std::string, DeviceHealth>> healths;
  std::map<std::string, bool> availability;
  int availabilityCount = 0;
  bool backendOnline = false;
};

// Captures service calls instead of sending them.
class CapturingBackend : public ZhaBackend {
 public:
  using ZhaBackend::ZhaBackend;
  std::vector<ServiceCall> calls;

 protected:
  void sendCalls(const std::vector<ServiceCall> &c) override {
    calls.insert(calls.end(), c.begin(), c.end());
  }
};

json state(const std::string &entityId, const std::string &value,
           json attributes = json::object()) {
  return {{"entity_id", entityId},
          {"state", value},
          {"attributes", std::move(attributes)}};
}

Snapshot loadSnapshot() {
  json j = loadTestJson("zha_snapshot.json");
  Snapshot s;
  s.deviceRegistry = j["device_registry"];
  s.entityRegistry = j["entity_registry"];
  s.states = j["states"];
  s.zhaDevices = j["zha_devices"];
  for (const auto &[id, triggers] : j["triggers"].items()) {
    s.triggers[id] = triggers;
  }
  return s;
}

constexpr const char *kPlug = "0xa4c13897bc9d1029";
constexpr const char *kContact = "0x00124b0022ea2033";

}  // namespace

TEST(ZhaBackendTest, DeviceListStatesAndAvailability) {
  CapturingBackend backend(HaConfig{});
  RecordingListener listener;
  backend.setListener(&listener);
  Snapshot s = loadSnapshot();
  backend.handleDevices(parseDevices(s), zhaDeviceIds(s).size(), s.states);

  EXPECT_EQ(listener.deviceListCount, 1);
  EXPECT_EQ(listener.devices.size(), 7u);
  EXPECT_EQ(backend.deviceCount(), 10u);
  // Every device with states gets its availability once.
  EXPECT_EQ(listener.availability[kContact], true);
  EXPECT_EQ(listener.availability[kPlug], true);
  bool contactState = false;
  for (const auto &st : listener.states) {
    if (st.id == kContact && st.key == "contact") {
      contactState = true;
      EXPECT_DOUBLE_EQ(st.state.primary, 0);  // "on" = open
    }
  }
  EXPECT_TRUE(contactState);
  ASSERT_FALSE(listener.healths.empty());

  // ZHA makes all entities of an unreachable device unavailable; the device
  // is back with the first entity that is available again.
  int count = listener.availabilityCount;
  backend.handleState(state("binary_sensor.ewelink_ds01", "unavailable"));
  EXPECT_EQ(listener.availabilityCount, count);  // battery still available
  backend.handleState(state("sensor.ewelink_ds01_battery", "unavailable"));
  EXPECT_EQ(listener.availability[kContact], false);
  backend.handleState(state("binary_sensor.ewelink_ds01", "off"));
  EXPECT_EQ(listener.availability[kContact], true);
  EXPECT_EQ(listener.states.back().key, "contact");
  EXPECT_DOUBLE_EQ(listener.states.back().state.primary, 1);

  // Entities of other devices are ignored.
  size_t states = listener.states.size();
  backend.handleState(state("sensor.ewelink_ms01_battery", "50"));
  backend.handleState(state("light.kitchen", "on"));
  EXPECT_EQ(listener.states.size(), states);
}

TEST(ZhaBackendTest, CommandsOnlyWhenConnected) {
  CapturingBackend backend(HaConfig{});
  RecordingListener listener;
  backend.setListener(&listener);
  Snapshot s = loadSnapshot();
  backend.handleDevices(parseDevices(s), 0, json::array());

  ChannelCommand on;
  on.type = ChannelCommand::Type::TurnOn;
  EXPECT_FALSE(backend.sendCommand(kPlug, "state", on));
  backend.handleConnection(true);
  EXPECT_TRUE(listener.backendOnline);
  EXPECT_TRUE(backend.sendCommand(kPlug, "state", on));
  ASSERT_EQ(backend.calls.size(), 1u);
  EXPECT_EQ(backend.calls[0].domain, "switch");
  EXPECT_EQ(backend.calls[0].service, "turn_on");
  EXPECT_FALSE(backend.sendCommand(kPlug, "energy", on));
  EXPECT_FALSE(backend.sendCommand("0x0000000000000000", "state", on));

  backend.handleAction(kPlug, "state", SUPLA_ACTION_CAP_HOLD);
  backend.handleAction("0x0000000000000000", "action", SUPLA_ACTION_CAP_HOLD);
  EXPECT_EQ(listener.actions.size(), 1u);

  backend.handleConnection(false);
  EXPECT_FALSE(listener.backendOnline);
  EXPECT_FALSE(backend.sendCommand(kPlug, "state", on));
}

namespace {

// Fake Home Assistant with ZHA: answers the commands used by ZhaBackend with
// the content of zha_snapshot.json and can send events.
class FakeZha {
 public:
  FakeZha() : snapshot_(loadTestJson("zha_snapshot.json")) {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    listen(listenFd_, 4);
    socklen_t len = sizeof(addr);
    getsockname(listenFd_, reinterpret_cast<sockaddr *>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { run(); });
  }
  ~FakeZha() {
    stop_ = true;
    shutdown(listenFd_, SHUT_RDWR);
    close(listenFd_);
    thread_.join();
  }

  std::string url() const {
    return "ws://127.0.0.1:" + std::to_string(port_) + "/api/websocket";
  }

  // Sends a state_changed event.
  void changeState(json newState) {
    send([newState](FakeZha *self) {
      return json({{"id", self->stateSubscription_},
                   {"type", "event"},
                   {"event",
                    {{"event_type", "state_changed"},
                     {"data", {{"new_state", newState}}}}}});
    });
  }

  // Fires the device trigger of the given type (all subscriptions of it).
  void fireTrigger(const std::string &type) {
    send([type](FakeZha *self) {
      json events = json::array();
      for (const auto &[id, t] : self->triggerSubscriptions_) {
        if (t != type) continue;
        events.push_back({{"id", id},
                          {"type", "event"},
                          {"event", {{"variables", {{"trigger", {}}}}}}});
      }
      return events;
    });
  }

  std::vector<json> serviceCalls() {
    std::lock_guard<std::mutex> lock(mutex_);
    return serviceCalls_;
  }
  size_t triggerSubscriptionCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    return triggerSubscriptions_.size();
  }

 private:
  using Message = std::function<json(FakeZha *)>;

  void send(Message message) {
    std::lock_guard<std::mutex> lock(mutex_);
    outgoing_.push_back(std::move(message));
  }

  void run() {
    while (!stop_) {
      int fd = accept(listenFd_, nullptr, nullptr);
      if (fd < 0) return;
      handle(fd);
      close(fd);
    }
  }

  void reply(int fd, int id, json result) {
    sendFrame(fd, 0x1,
              json({{"id", id},
                    {"type", "result"},
                    {"success", true},
                    {"result", std::move(result)}})
                  .dump());
  }

  void handle(int fd) {
    if (!acceptHandshake(fd, "/api/websocket")) return;
    sendFrame(fd, 0x1, R"({"type":"auth_required"})");
    std::string payload;
    if (readFrame(fd, &payload) != 0x1) return;
    if (json::parse(payload).value("access_token", "") != "token") {
      sendFrame(fd, 0x1, R"({"type":"auth_invalid"})");
      return;
    }
    sendFrame(fd, 0x1, R"({"type":"auth_ok"})");

    while (!stop_) {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!outgoing_.empty()) {
          json msg = outgoing_.front()(this);
          outgoing_.pop_front();
          if (msg.is_array()) {
            for (const auto &m : msg) sendFrame(fd, 0x1, m.dump());
          } else {
            sendFrame(fd, 0x1, msg.dump());
          }
        }
      }
      pollfd pfd{fd, POLLIN, 0};
      if (::poll(&pfd, 1, 10) <= 0) continue;
      int op = readFrame(fd, &payload);
      if (op == 0xA) continue;
      if (op != 0x1) return;
      json msg = json::parse(payload);
      int id = msg["id"];
      std::string type = msg["type"];
      std::lock_guard<std::mutex> lock(mutex_);
      if (type == "config_entries/get") {
        reply(fd, id, json::array({{{"domain", "zha"}, {"state", "loaded"}}}));
      } else if (type == "subscribe_events") {
        if (msg["event_type"] == "state_changed") stateSubscription_ = id;
        reply(fd, id, nullptr);
      } else if (type == "config/device_registry/list") {
        reply(fd, id, snapshot_["device_registry"]);
      } else if (type == "config/entity_registry/list") {
        reply(fd, id, snapshot_["entity_registry"]);
      } else if (type == "get_states") {
        reply(fd, id, snapshot_["states"]);
      } else if (type == "zha/devices") {
        reply(fd, id, snapshot_["zha_devices"]);
      } else if (type == "device_automation/trigger/list") {
        reply(fd, id,
              snapshot_["triggers"].value(msg["device_id"].get<std::string>(),
                                          json::array()));
      } else if (type == "subscribe_trigger") {
        triggerSubscriptions_[id] = msg["trigger"]["type"];
        reply(fd, id, nullptr);
      } else if (type == "call_service") {
        serviceCalls_.push_back(msg);
        reply(fd, id, json::object());
      } else if (type == "ping") {
        sendFrame(fd, 0x1, json({{"id", id}, {"type", "pong"}}).dump());
      } else {
        sendFrame(fd, 0x1,
                  json({{"id", id},
                        {"type", "result"},
                        {"success", false},
                        {"error", {{"code", "unknown_command"}}}})
                      .dump());
      }
    }
  }

  json snapshot_;
  int listenFd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::deque<Message> outgoing_;
  int stateSubscription_ = 0;
  std::map<int, std::string> triggerSubscriptions_;
  std::vector<json> serviceCalls_;
};

// Polls the backend until the condition holds (at most 5 s).
bool pollUntil(ZhaBackend *backend, const std::function<bool()> &condition) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    backend->poll();
    if (condition()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

}  // namespace

TEST(ZhaBackendTest, AgainstFakeHomeAssistant) {
  FakeZha ha;
  HaConfig config;
  config.websocketUrl = ha.url();
  config.token = "token";
  config.timeoutMs = 3000;
  ZhaBackend backend(config);
  RecordingListener listener;
  backend.setListener(&listener);
  ASSERT_TRUE(backend.start());

  ASSERT_TRUE(pollUntil(&backend, [&] { return listener.backendOnline; }));
  EXPECT_EQ(listener.deviceListCount, 1);
  EXPECT_EQ(listener.devices.size(), 7u);
  EXPECT_EQ(backend.deviceCount(), 10u);
  EXPECT_FALSE(listener.states.empty());
  // Button: short press, double press and hold.
  EXPECT_EQ(ha.triggerSubscriptionCount(), 3u);

  ha.changeState(state("switch.tz3210_2putqrmw_ts011f", "on"));
  ASSERT_TRUE(pollUntil(&backend, [&] {
    return listener.states.back().id == kPlug &&
           listener.states.back().key == "state";
  }));
  EXPECT_DOUBLE_EQ(listener.states.back().state.primary, 1);

  ha.fireTrigger("remote_button_double_press");
  ASSERT_TRUE(pollUntil(&backend, [&] { return !listener.actions.empty(); }));
  EXPECT_EQ(listener.actions[0].id, "0xa4c1385d607115bd");
  EXPECT_EQ(listener.actions[0].key, "action");
  EXPECT_EQ(listener.actions[0].action,
            static_cast<uint32_t>(SUPLA_ACTION_CAP_SHORT_PRESS_x2));

  ChannelCommand off;
  off.type = ChannelCommand::Type::TurnOff;
  EXPECT_TRUE(backend.sendCommand(kPlug, "state", off));
  ASSERT_TRUE(pollUntil(&backend, [&] { return !ha.serviceCalls().empty(); }));
  json call = ha.serviceCalls()[0];
  EXPECT_EQ(call["domain"], "switch");
  EXPECT_EQ(call["service"], "turn_off");
  EXPECT_EQ(call["service_data"]["entity_id"], "switch.tz3210_2putqrmw_ts011f");

  backend.stop();
}
