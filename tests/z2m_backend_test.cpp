// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of zigbee2mqtt message handling without a real MQTT broker.

#include "z2m_backend.h"

#include <gtest/gtest.h>

#include "test_util.h"

using namespace z2s;
using namespace z2s::z2m;

namespace {

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
  }
  void onBackendAvailability(bool online) override { backendOnline = online; }

  struct State {
    std::string id, key;
    ChannelState state;
  };
  std::vector<DeviceDescriptor> devices;
  int deviceListCount = 0;
  std::vector<State> states;
  std::vector<std::pair<std::string, DeviceHealth>> healths;
  struct Action {
    std::string id, key;
    uint32_t action;
  };
  std::vector<Action> actions;
  std::map<std::string, bool> availability;
  bool backendOnline = false;
};

// Captures outgoing MQTT messages instead of sending them.
class CapturingBackend : public Z2mBackend {
 public:
  using Z2mBackend::Z2mBackend;
  struct Message {
    std::string topic, payload;
  };
  std::vector<Message> published;

 protected:
  void publish(const std::string &topic, const std::string &payload) override {
    published.push_back({topic, payload});
  }
};

}  // namespace

TEST(Z2mBackendTest, RoutesMessagesToListener) {
  Z2mBackend backend(MqttConfig{});
  RecordingListener listener;
  backend.setListener(&listener);

  backend.handleConnection(true);
  EXPECT_TRUE(listener.backendOnline);

  // State and availability received before the device list are replayed.
  backend.handleMessage("zigbee2mqtt/Wlacznik kuchnia",
                        R"({"state_l1":"ON","linkquality":80})");
  backend.handleMessage("zigbee2mqtt/Wlacznik kuchnia/availability",
                        R"({"state":"offline"})");
  EXPECT_TRUE(listener.states.empty());

  backend.handleMessage("zigbee2mqtt/bridge/devices",
                        loadTestJson("bridge_devices.json").dump());
  EXPECT_EQ(listener.deviceListCount, 1);
  EXPECT_EQ(listener.devices.size(), 5u);
  // All devices but the coordinator, bridged or not.
  EXPECT_EQ(backend.deviceCount(), 8u);
  ASSERT_EQ(listener.states.size(), 1u);
  EXPECT_EQ(listener.states[0].id, "0xa4c1380000000003");
  EXPECT_EQ(listener.states[0].key, "state_l1");
  EXPECT_EQ(listener.availability["0xa4c1380000000003"], false);
  ASSERT_EQ(listener.healths.size(), 1u);
  EXPECT_EQ(listener.healths[0].first, "0xa4c1380000000003");
  EXPECT_EQ(listener.healths[0].second.linkQuality, 31);  // 80 of 255
  EXPECT_EQ(listener.healths[0].second.batteryLevel, -1);

  // Friendly names may contain '/'.
  backend.handleMessage("zigbee2mqtt/Salon/czujnik",
                        R"({"temperature":22.5,"humidity":40})");
  ASSERT_EQ(listener.states.size(), 2u);
  EXPECT_EQ(listener.states[1].id, "0x00158d0001a2b3c4");
  EXPECT_EQ(listener.states[1].key, "temperature+humidity");

  // Our own /set and /get topics are ignored.
  backend.handleMessage("zigbee2mqtt/Wlacznik kuchnia/set",
                        R"({"state_l1":"OFF"})");
  EXPECT_EQ(listener.states.size(), 2u);

  backend.handleMessage("zigbee2mqtt/bridge/state", R"({"state":"offline"})");
  EXPECT_FALSE(listener.backendOnline);
  backend.handleMessage("zigbee2mqtt/bridge/state", R"({"state":"online"})");
  EXPECT_TRUE(listener.backendOnline);
  backend.handleConnection(false);
  EXPECT_FALSE(listener.backendOnline);
}

TEST(Z2mBackendTest, RequestsStateOnlyWhenAsked) {
  CapturingBackend backend(MqttConfig{});
  RecordingListener listener;
  backend.setListener(&listener);
  backend.handleConnection(true);

  // Receiving the device list alone must not query any device: the gateway
  // decides which devices are bridged.
  backend.handleMessage("zigbee2mqtt/bridge/devices",
                        loadTestJson("bridge_devices.json").dump());
  EXPECT_TRUE(backend.published.empty());

  backend.requestState("0xa4c1380000000003");
  ASSERT_EQ(backend.published.size(), 1u);
  EXPECT_EQ(backend.published[0].topic, "zigbee2mqtt/Wlacznik kuchnia/get");
  EXPECT_EQ(nlohmann::json::parse(backend.published[0].payload),
            nlohmann::json::parse(R"({"state_l1":"","state_l2":""})"));

  // Sensors have nothing to ask for; unknown devices are ignored.
  backend.requestState("0x00158d0002c3d4e5");
  backend.requestState("0xffffffffffffffff");
  EXPECT_EQ(backend.published.size(), 1u);
}

TEST(Z2mBackendTest, ButtonActionsOnlyFromLiveMessages) {
  Z2mBackend backend(MqttConfig{});
  RecordingListener listener;
  backend.setListener(&listener);
  backend.handleConnection(true);

  nlohmann::json devices = nlohmann::json::parse(R"([{
    "ieee_address": "0xa4c1380000000041",
    "friendly_name": "Przycisk",
    "type": "EndDevice",
    "interview_completed": true,
    "definition": {"vendor": "Tuya", "model": "TS0041", "exposes": [
      {"type": "enum", "name": "action", "property": "action", "access": 1,
       "values": ["single", "double", "hold"]}]}
  }])");
  // A press before the device list is known is not replayed later.
  backend.handleMessage("zigbee2mqtt/Przycisk", R"({"action":"single"})");
  backend.handleMessage("zigbee2mqtt/bridge/devices", devices.dump());
  EXPECT_TRUE(listener.actions.empty());

  backend.handleMessage("zigbee2mqtt/Przycisk", R"({"action":"double"})");
  ASSERT_EQ(listener.actions.size(), 1u);
  EXPECT_EQ(listener.actions[0].id, "0xa4c1380000000041");
  EXPECT_EQ(listener.actions[0].key, "action");
  EXPECT_EQ(listener.actions[0].action,
            uint32_t{SUPLA_ACTION_CAP_SHORT_PRESS_x2});

  // Retained by the broker: an old press.
  backend.handleMessage("zigbee2mqtt/Przycisk", R"({"action":"hold"})", true);
  // The device list is published again: the cached state is replayed.
  backend.handleMessage("zigbee2mqtt/bridge/devices", devices.dump());
  EXPECT_EQ(listener.actions.size(), 1u);
}
