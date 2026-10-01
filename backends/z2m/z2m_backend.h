// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <deque>
#include <map>
#include <mutex>
#include <string>

#include "z2m_mapper.h"
#include "z2s/backend.h"

struct mosquitto;
struct mosquitto_message;

namespace z2s::z2m {

struct MqttConfig {
  std::string host = "localhost";
  int port = 1883;
  std::string username;
  std::string password;
  std::string clientId = "zigbee2supla";
  std::string baseTopic = "zigbee2mqtt";
};

// Backend using an existing zigbee2mqtt installation through MQTT.
class Z2mBackend : public Backend {
 public:
  explicit Z2mBackend(const MqttConfig &config);
  ~Z2mBackend() override;

  void setListener(BackendListener *listener) override { listener_ = listener; }
  bool start() override;
  void stop() override;
  void poll() override;
  bool sendCommand(const std::string &deviceId, const std::string &channelKey,
                   const ChannelCommand &command) override;
  void requestState(const std::string &deviceId) override;
  size_t deviceCount() const override { return deviceCount_; }

  // Processes one MQTT message (public for tests).
  // `retained`: the broker replayed a stored message (button actions in it
  // are old and are ignored).
  void handleMessage(const std::string &topic, const std::string &payload,
                     bool retained = false);
  void handleConnection(bool connected);

 protected:
  // Virtual so that tests can capture outgoing messages.
  virtual void publish(const std::string &topic, const std::string &payload);

 private:
  struct Event {
    enum class Type { Connected, Disconnected, Message } type;
    std::string topic;
    std::string payload;
    bool retained = false;
  };

  static void onConnect(mosquitto *mosq, void *obj, int rc);
  static void onDisconnect(mosquitto *mosq, void *obj, int rc);
  static void onMessage(mosquitto *mosq, void *obj,
                        const mosquitto_message *msg);
  void pushEvent(Event event);
  void handleDeviceList(const std::string &payload);
  // `live`: a new message, not a replay; only then button actions count.
  void handleDeviceState(const std::string &friendlyName,
                         const std::string &payload, bool live);
  void notifyBackendAvailability();

  MqttConfig config_;
  BackendListener *listener_ = nullptr;
  size_t deviceCount_ = 0;
  mosquitto *mosq_ = nullptr;

  std::mutex queueMutex_;
  std::deque<Event> queue_;

  bool mqttConnected_ = false;
  bool bridgeOnline_ = true;
  std::map<std::string, Device> devices_;                // by IEEE address
  std::map<std::string, std::string> friendlyNameToId_;  // name -> IEEE
  std::map<std::string, std::string> lastState_;         // name -> payload
  std::map<std::string, bool> lastAvailability_;         // name -> online
};

}  // namespace z2s::z2m
