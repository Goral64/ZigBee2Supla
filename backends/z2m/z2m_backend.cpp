// SPDX-License-Identifier: GPL-2.0-or-later

#include "z2m_backend.h"

#include <mosquitto.h>

#include "z2s/log.h"

namespace z2s::z2m {

using json = nlohmann::json;

namespace {

bool startsWith(const std::string &s, const std::string &prefix) {
  return s.compare(0, prefix.size(), prefix) == 0;
}

bool endsWith(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

}  // namespace

Z2mBackend::Z2mBackend(const MqttConfig &config) : config_(config) {}

Z2mBackend::~Z2mBackend() { stop(); }

bool Z2mBackend::start() {
  mosquitto_lib_init();
  mosq_ = mosquitto_new(config_.clientId.c_str(), true, this);
  if (mosq_ == nullptr) {
    Z2S_LOG_ERROR("mosquitto_new() failed");
    return false;
  }
  if (!config_.username.empty()) {
    mosquitto_username_pw_set(
        mosq_, config_.username.c_str(),
        config_.password.empty() ? nullptr : config_.password.c_str());
  }
  mosquitto_connect_callback_set(mosq_, &Z2mBackend::onConnect);
  mosquitto_disconnect_callback_set(mosq_, &Z2mBackend::onDisconnect);
  mosquitto_message_callback_set(mosq_, &Z2mBackend::onMessage);
  mosquitto_reconnect_delay_set(mosq_, 2, 60, true);

  int rc =
      mosquitto_connect_async(mosq_, config_.host.c_str(), config_.port, 60);
  if (rc != MOSQ_ERR_SUCCESS) {
    // The loop thread keeps retrying, so this is not fatal.
    Z2S_LOG_WARNING("MQTT: cannot connect to %s:%d yet: %s",
                    config_.host.c_str(), config_.port, mosquitto_strerror(rc));
  }
  rc = mosquitto_loop_start(mosq_);
  if (rc != MOSQ_ERR_SUCCESS) {
    Z2S_LOG_ERROR("MQTT: cannot start network loop: %s",
                  mosquitto_strerror(rc));
    return false;
  }
  Z2S_LOG_INFO("MQTT: connecting to %s:%d, base topic '%s'",
               config_.host.c_str(), config_.port, config_.baseTopic.c_str());
  return true;
}

void Z2mBackend::stop() {
  if (mosq_ != nullptr) {
    mosquitto_disconnect(mosq_);
    mosquitto_loop_stop(mosq_, true);
    mosquitto_destroy(mosq_);
    mosq_ = nullptr;
    mosquitto_lib_cleanup();
  }
}

void Z2mBackend::onConnect(mosquitto *, void *obj, int rc) {
  auto *self = static_cast<Z2mBackend *>(obj);
  if (rc != 0) {
    Z2S_LOG_WARNING("MQTT: connection refused: %s",
                    mosquitto_connack_string(rc));
    return;
  }
  // Subscribe from the network thread, so it happens on every reconnect.
  std::string topic = self->config_.baseTopic + "/#";
  mosquitto_subscribe(self->mosq_, nullptr, topic.c_str(), 0);
  self->pushEvent({Event::Type::Connected, {}, {}});
}

void Z2mBackend::onDisconnect(mosquitto *, void *obj, int) {
  static_cast<Z2mBackend *>(obj)->pushEvent(
      {Event::Type::Disconnected, {}, {}});
}

void Z2mBackend::onMessage(mosquitto *, void *obj,
                           const mosquitto_message *msg) {
  auto *self = static_cast<Z2mBackend *>(obj);
  std::string payload;
  if (msg->payload != nullptr && msg->payloadlen > 0) {
    payload.assign(static_cast<const char *>(msg->payload), msg->payloadlen);
  }
  self->pushEvent(
      {Event::Type::Message, msg->topic, std::move(payload), msg->retain});
}

void Z2mBackend::pushEvent(Event event) {
  std::lock_guard<std::mutex> lock(queueMutex_);
  queue_.push_back(std::move(event));
}

void Z2mBackend::poll() {
  std::deque<Event> events;
  {
    std::lock_guard<std::mutex> lock(queueMutex_);
    events.swap(queue_);
  }
  for (auto &event : events) {
    switch (event.type) {
      case Event::Type::Connected:
        handleConnection(true);
        break;
      case Event::Type::Disconnected:
        handleConnection(false);
        break;
      case Event::Type::Message:
        handleMessage(event.topic, event.payload, event.retained);
        break;
    }
  }
}

void Z2mBackend::handleConnection(bool connected) {
  if (connected != mqttConnected_) {
    Z2S_LOG_INFO("MQTT: %s", connected ? "connected" : "disconnected");
  }
  mqttConnected_ = connected;
  notifyBackendAvailability();
}

void Z2mBackend::notifyBackendAvailability() {
  if (listener_ != nullptr) {
    listener_->onBackendAvailability(mqttConnected_ && bridgeOnline_);
  }
}

void Z2mBackend::handleMessage(const std::string &topic,
                               const std::string &payload, bool retained) {
  const std::string prefix = config_.baseTopic + "/";
  if (!startsWith(topic, prefix)) return;
  const std::string rest = topic.substr(prefix.size());

  if (rest == "bridge/devices") {
    handleDeviceList(payload);
    return;
  }
  if (rest == "bridge/state") {
    if (auto online = parseAvailability(payload)) {
      bridgeOnline_ = *online;
      notifyBackendAvailability();
    }
    return;
  }
  if (startsWith(rest, "bridge/")) return;
  if (endsWith(rest, "/set") || endsWith(rest, "/get")) return;

  const std::string availabilitySuffix = "/availability";
  if (endsWith(rest, availabilitySuffix)) {
    std::string name = rest.substr(0, rest.size() - availabilitySuffix.size());
    if (auto online = parseAvailability(payload)) {
      lastAvailability_[name] = *online;
      auto it = friendlyNameToId_.find(name);
      if (it != friendlyNameToId_.end() && listener_ != nullptr) {
        listener_->onDeviceAvailability(it->second, *online);
      }
    }
    return;
  }

  handleDeviceState(rest, payload, !retained);
}

void Z2mBackend::handleDeviceState(const std::string &friendlyName,
                                   const std::string &payload, bool live) {
  auto idIt = friendlyNameToId_.find(friendlyName);
  if (idIt == friendlyNameToId_.end()) {
    // Device list not received yet (or topic is not a device); keep the
    // latest payload to replay it once the device list arrives.
    if (payload.size() < 4096) lastState_[friendlyName] = payload;
    return;
  }
  lastState_[friendlyName] = payload;
  json parsed = json::parse(payload, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) return;
  const Device &device = devices_.at(idIt->second);
  if (listener_ == nullptr) return;
  for (const auto &[key, state] : extractStates(device, parsed)) {
    listener_->onChannelState(device.descriptor.id, key, state);
  }
  if (auto health = extractHealth(parsed)) {
    listener_->onDeviceHealth(device.descriptor.id, *health);
  }
  if (live) {
    for (const auto &[key, action] : extractActions(device, parsed)) {
      listener_->onChannelAction(device.descriptor.id, key, action);
    }
  }
}

void Z2mBackend::handleDeviceList(const std::string &payload) {
  json parsed = json::parse(payload, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_array()) {
    Z2S_LOG_WARNING("MQTT: invalid bridge/devices payload");
    return;
  }
  std::vector<Device> devices = parseBridgeDevices(parsed);
  devices_.clear();
  friendlyNameToId_.clear();
  std::vector<DeviceDescriptor> descriptors;
  for (auto &device : devices) {
    friendlyNameToId_[device.friendlyName] = device.descriptor.id;
    descriptors.push_back(device.descriptor);
    devices_[device.descriptor.id] = std::move(device);
  }
  deviceCount_ = 0;
  for (const auto &dev : parsed) {
    if (dev.is_object() && dev.value("type", "") != "Coordinator") {
      deviceCount_++;
    }
  }
  Z2S_LOG_INFO("zigbee2mqtt: %zu of %zu device(s) can be bridged",
               devices_.size(), deviceCount_);
  // Tell the user which devices are left out because of their kind.
  std::map<std::string, std::string> skipped;  // kind -> names
  for (const auto &dev : parsed) {
    const char *kind = unsupportedDeviceKind(dev);
    if (kind == nullptr || !dev.is_object()) continue;
    if (dev.value("disabled", false)) continue;
    std::string &names = skipped[kind];
    if (!names.empty()) names += ", ";
    names += dev.value("friendly_name", "?");
  }
  for (const auto &[kind, names] : skipped) {
    Z2S_LOG_INFO("zigbee2mqtt: not bridged, %s is not supported yet: %s",
                 kind.c_str(), names.c_str());
  }

  if (listener_ == nullptr) return;
  listener_->onDeviceList(descriptors);

  for (const auto &[id, device] : devices_) {
    auto availIt = lastAvailability_.find(device.friendlyName);
    if (availIt != lastAvailability_.end()) {
      listener_->onDeviceAvailability(id, availIt->second);
    }
    auto stateIt = lastState_.find(device.friendlyName);
    if (stateIt != lastState_.end()) {
      std::string cached = stateIt->second;
      handleDeviceState(device.friendlyName, cached, false);
    }
  }
}

void Z2mBackend::requestState(const std::string &deviceId) {
  auto it = devices_.find(deviceId);
  if (it == devices_.end()) return;
  const Device &device = it->second;
  // zigbee2mqtt does not retain device states by default: ask relays for
  // their current state.
  json get = json::object();
  for (const auto &b : device.bindings) {
    if (b.spec.kind == ChannelKind::Relay) get[b.property] = "";
  }
  if (!get.empty()) {
    publish(config_.baseTopic + "/" + device.friendlyName + "/get", get.dump());
  }
}

bool Z2mBackend::sendCommand(const std::string &deviceId,
                             const std::string &channelKey,
                             const ChannelCommand &command) {
  auto it = devices_.find(deviceId);
  if (it == devices_.end()) return false;
  auto payloads = buildCommand(it->second, channelKey, command);
  if (payloads.empty()) return false;
  if (!mqttConnected_) return false;
  for (const auto &payload : payloads) {
    publish(config_.baseTopic + "/" + it->second.friendlyName + "/set",
            payload.dump());
  }
  return true;
}

void Z2mBackend::publish(const std::string &topic, const std::string &payload) {
  if (mosq_ == nullptr) return;
  int rc = mosquitto_publish(mosq_, nullptr, topic.c_str(),
                             static_cast<int>(payload.size()), payload.data(),
                             0, false);
  if (rc != MOSQ_ERR_SUCCESS) {
    Z2S_LOG_WARNING("MQTT: publish to %s failed: %s", topic.c_str(),
                    mosquitto_strerror(rc));
  } else {
    Z2S_LOG_DEBUG("MQTT: %s <- %s", topic.c_str(), payload.c_str());
  }
}

}  // namespace z2s::z2m
