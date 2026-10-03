// SPDX-License-Identifier: GPL-2.0-or-later

#include "zha_backend.h"

#include <chrono>

#include "websocket_client.h"
#include "z2s/log.h"

namespace z2s::zha {

using json = nlohmann::json;

namespace {

constexpr int kPingIntervalMs = 30000;
constexpr int kSilenceTimeoutMs = 90000;
// Registry changes come in bursts (a new device adds many entities): the
// devices are fetched again once it has been quiet for a while.
constexpr int kRegistrySettleMs = 3000;

uint64_t nowMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

}  // namespace

ZhaBackend::ZhaBackend(const HaConfig &config) : config_(config) {}

ZhaBackend::~ZhaBackend() { stop(); }

bool ZhaBackend::start() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopRequested_ = false;
  }
  thread_ = std::thread([this] { threadMain(); });
  Z2S_LOG_INFO("ZHA: connecting to Home Assistant at %s",
               config_.websocketUrl.c_str());
  return true;
}

void ZhaBackend::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopRequested_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void ZhaBackend::pushEvent(Event event) {
  std::lock_guard<std::mutex> lock(mutex_);
  events_.push_back(std::move(event));
}

bool ZhaBackend::waitFor(int ms) {
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait_for(lock, std::chrono::milliseconds(ms),
               [this] { return stopRequested_; });
  return !stopRequested_;
}

// --- main thread -------------------------------------------------------------

void ZhaBackend::poll() {
  std::deque<Event> events;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    events.swap(events_);
  }
  for (auto &event : events) {
    switch (event.type) {
      case Event::Type::Connected:
        handleConnection(true);
        break;
      case Event::Type::Disconnected:
        handleConnection(false);
        break;
      case Event::Type::Devices:
        handleDevices(std::move(event.devices), event.deviceCount, event.json);
        break;
      case Event::Type::State:
        handleState(event.json);
        break;
      case Event::Type::Action:
        handleAction(event.deviceId, event.key, event.action);
        break;
    }
  }
}

void ZhaBackend::handleConnection(bool connected) {
  if (connected != connected_) {
    Z2S_LOG_INFO("ZHA: %s Home Assistant",
                 connected ? "connected to" : "disconnected from");
  }
  connected_ = connected;
  if (listener_ != nullptr) listener_->onBackendAvailability(connected);
}

void ZhaBackend::handleDevices(std::vector<Device> devices, size_t deviceCount,
                               const json &states) {
  deviceCount_ = deviceCount;
  devices_.clear();
  entityToDevice_.clear();
  entityAvailable_.clear();
  deviceAvailable_.clear();
  std::vector<DeviceDescriptor> descriptors;
  for (auto &device : devices) {
    for (const auto &entityId : entityIds(device)) {
      entityToDevice_[entityId] = device.descriptor.id;
    }
    descriptors.push_back(device.descriptor);
    std::string id = device.descriptor.id;
    devices_[id] = std::move(device);
  }
  if (listener_ == nullptr) return;
  listener_->onDeviceList(descriptors);
  if (states.is_array()) {
    for (const auto &state : states) handleState(state);
  }
}

void ZhaBackend::handleState(const json &state) {
  if (!state.is_object() || !state.contains("entity_id") ||
      !state["entity_id"].is_string()) {
    return;
  }
  const std::string entityId = state["entity_id"];
  auto idIt = entityToDevice_.find(entityId);
  if (idIt == entityToDevice_.end()) return;
  const std::string &deviceId = idIt->second;
  const Device &device = devices_.at(deviceId);
  if (listener_ == nullptr) return;

  // ZHA marks all entities of an unreachable device unavailable.
  auto &entities = entityAvailable_[deviceId];
  entities[entityId] = isAvailable(state);
  bool online = false;
  for (const auto &[id, available] : entities) online = online || available;
  auto availIt = deviceAvailable_.find(deviceId);
  if (availIt == deviceAvailable_.end() || availIt->second != online) {
    deviceAvailable_[deviceId] = online;
    listener_->onDeviceAvailability(deviceId, online);
  }

  for (const auto &[key, channelState] : extractStates(device, state)) {
    listener_->onChannelState(deviceId, key, channelState);
  }
  if (auto health = extractHealth(device, state)) {
    listener_->onDeviceHealth(deviceId, *health);
  }
}

void ZhaBackend::handleAction(const std::string &deviceId,
                              const std::string &key, uint32_t action) {
  if (listener_ == nullptr || devices_.find(deviceId) == devices_.end()) {
    return;
  }
  listener_->onChannelAction(deviceId, key, action);
}

bool ZhaBackend::sendCommand(const std::string &deviceId,
                             const std::string &channelKey,
                             const ChannelCommand &command) {
  auto it = devices_.find(deviceId);
  if (it == devices_.end()) return false;
  auto calls = buildCommand(it->second, channelKey, command);
  if (calls.empty() || !connected_) return false;
  sendCalls(calls);
  return true;
}

void ZhaBackend::sendCalls(const std::vector<ServiceCall> &calls) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &call : calls) calls_.push_back(call);
  }
  cv_.notify_all();
}

// --- worker thread -----------------------------------------------------------

void ZhaBackend::threadMain() {
  int delayMs = 5000;
  while (true) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopRequested_) return;
    }
    uint64_t started = nowMs();
    runSession();
    pushEvent(Event(Event::Type::Disconnected));
    // Reconnect quickly after a long session (e.g. a Home Assistant
    // restart), slower when connecting keeps failing.
    if (nowMs() - started > 60000) delayMs = 5000;
    if (!waitFor(delayMs)) return;
    delayMs = std::min(delayMs * 2, 60000);
  }
}

int ZhaBackend::call(WebSocketClient *ws, json command, json *result) {
  int id = nextId_++;
  command["id"] = id;
  std::string error;
  if (!ws->sendText(command.dump(), &error)) {
    Z2S_LOG_WARNING("ZHA: %s", error.c_str());
    return 0;
  }
  while (true) {
    std::string text;
    if (!ws->receiveText(&text, config_.timeoutMs, &error)) {
      Z2S_LOG_WARNING("ZHA: %s", error.c_str());
      return 0;
    }
    json msg = json::parse(text, nullptr, false);
    if (msg.is_discarded() || !msg.is_object()) continue;
    if (msg.value("id", -1) != id || msg.value("type", "") != "result") {
      dispatch(msg);
      continue;
    }
    if (!msg.value("success", false)) {
      Z2S_LOG_WARNING("ZHA: Home Assistant rejected '%s': %s",
                      command.value("type", "").c_str(),
                      msg.value("error", json::object()).dump().c_str());
      *result = json();
      return -id;
    }
    *result = msg.value("result", json());
    return id;
  }
}

void ZhaBackend::runSession() {
  WebSocketClient ws;
  std::string error;
  if (!ws.connect(config_.websocketUrl, config_.timeoutMs, &error)) {
    Z2S_LOG_WARNING("ZHA: %s", error.c_str());
    return;
  }
  // Authentication: auth_required -> auth -> auth_ok.
  std::string text;
  if (!ws.receiveText(&text, config_.timeoutMs, &error)) {
    Z2S_LOG_WARNING("ZHA: %s", error.c_str());
    return;
  }
  json auth = {{"type", "auth"}, {"access_token", config_.token}};
  if (!ws.sendText(auth.dump(), &error) ||
      !ws.receiveText(&text, config_.timeoutMs, &error)) {
    Z2S_LOG_WARNING("ZHA: %s", error.c_str());
    return;
  }
  json msg = json::parse(text, nullptr, false);
  if (msg.is_discarded() || msg.value("type", "") != "auth_ok") {
    Z2S_LOG_ERROR("ZHA: Home Assistant refused the access token");
    return;
  }

  // Right after a Home Assistant start ZHA is not ready yet and its entities
  // are unavailable: they must not disconnect the devices from Supla.
  json entries;
  if (call(&ws, {{"type", "config_entries/get"}, {"domain", "zha"}},
           &entries) <= 0) {
    return;
  }
  bool loaded = false;
  if (entries.is_array()) {
    for (const auto &e : entries) {
      loaded = loaded || e.value("state", "") == "loaded";
    }
  }
  if (!loaded) {
    Z2S_LOG_WARNING(
        "ZHA: the ZHA integration is not loaded in Home "
        "Assistant (yet)");
    return;
  }

  stateSubscription_ = 0;
  syncing_ = false;
  registrySubscriptions_.clear();
  triggerSubscriptions_.clear();
  watchedEntities_.clear();
  registryChanged_ = false;
  json ignored;
  int id =
      call(&ws, {{"type", "subscribe_events"}, {"event_type", "state_changed"}},
           &ignored);
  if (id <= 0) return;
  stateSubscription_ = id;
  for (const char *event :
       {"device_registry_updated", "entity_registry_updated"}) {
    id = call(&ws, {{"type", "subscribe_events"}, {"event_type", event}},
              &ignored);
    if (id <= 0) return;
    registrySubscriptions_.insert(id);
  }
  if (!syncDevices(&ws)) return;
  pushEvent(Event(Event::Type::Connected));

  uint64_t lastReceived = nowMs();
  uint64_t lastPing = lastReceived;
  while (true) {
    std::deque<ServiceCall> calls;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stopRequested_) return;
      calls.swap(calls_);
    }
    for (const auto &c : calls) {
      json command = {{"id", nextId_++},
                      {"type", "call_service"},
                      {"domain", c.domain},
                      {"service", c.service},
                      {"service_data", c.data}};
      if (!ws.sendText(command.dump(), &error)) {
        Z2S_LOG_WARNING("ZHA: %s", error.c_str());
        return;
      }
      Z2S_LOG_DEBUG("ZHA: %s.%s %s", c.domain.c_str(), c.service.c_str(),
                    c.data.dump().c_str());
    }

    uint64_t now = nowMs();
    if (registryChanged_ && now - registryChangedMs_ >= kRegistrySettleMs) {
      registryChanged_ = false;
      Z2S_LOG_INFO("ZHA: devices changed in Home Assistant, reloading");
      if (!syncDevices(&ws)) return;
    }
    if (now - lastPing >= kPingIntervalMs) {
      lastPing = now;
      if (!ws.sendText(json({{"id", nextId_++}, {"type", "ping"}}).dump(),
                       &error)) {
        Z2S_LOG_WARNING("ZHA: %s", error.c_str());
        return;
      }
    }
    if (!ws.waitReadable(50)) {
      if (now - lastReceived > kSilenceTimeoutMs) {
        Z2S_LOG_WARNING("ZHA: no response from Home Assistant");
        return;
      }
      continue;
    }
    if (!ws.receiveText(&text, config_.timeoutMs, &error)) {
      Z2S_LOG_WARNING("ZHA: %s", error.c_str());
      return;
    }
    lastReceived = nowMs();
    msg = json::parse(text, nullptr, false);
    if (!msg.is_discarded() && msg.is_object()) dispatch(msg);
  }
}

bool ZhaBackend::fetchSnapshot(WebSocketClient *ws, Snapshot *snapshot) {
  if (call(ws, {{"type", "config/device_registry/list"}},
           &snapshot->deviceRegistry) <= 0 ||
      call(ws, {{"type", "config/entity_registry/list"}},
           &snapshot->entityRegistry) <= 0 ||
      call(ws, {{"type", "get_states"}}, &snapshot->states) <= 0) {
    return false;
  }
  // Only for the coordinator and the power source; not essential.
  if (call(ws, {{"type", "zha/devices"}}, &snapshot->zhaDevices) == 0) {
    return false;
  }
  for (const auto &deviceId : zhaDeviceIds(*snapshot)) {
    json triggers;
    int id = call(
        ws,
        {{"type", "device_automation/trigger/list"}, {"device_id", deviceId}},
        &triggers);
    if (id == 0) return false;
    if (id > 0) snapshot->triggers[deviceId] = triggers;
  }
  return true;
}

bool ZhaBackend::syncDevices(WebSocketClient *ws) {
  // State changes received while fetching are older than the fetched states
  // only when they come before them; they are kept and replayed after the
  // device list, so that the newest state wins.
  syncing_ = true;
  bufferedStates_.clear();
  Snapshot snapshot;
  if (!fetchSnapshot(ws, &snapshot)) return false;

  std::vector<Device> devices = parseDevices(snapshot);
  std::vector<std::string> zhaIds = zhaDeviceIds(snapshot);
  Z2S_LOG_INFO("ZHA: %zu of %zu device(s) can be bridged", devices.size(),
               zhaIds.size());
  std::map<std::string, std::string> skipped;  // kind -> names
  for (const auto &haId : zhaIds) {
    const char *kind = unsupportedDeviceKind(snapshot, haId);
    if (kind == nullptr) continue;
    std::string name = "?";
    for (const auto &dev : snapshot.deviceRegistry) {
      if (dev.value("id", "") != haId) continue;
      if (!dev.value("disabled_by", json()).is_null())
        name.clear();
      else if (dev.value("name_by_user", json()).is_string()) {
        name = dev["name_by_user"];
      } else if (dev.value("name", json()).is_string()) {
        name = dev["name"];
      }
    }
    if (name.empty()) continue;
    std::string &names = skipped[kind];
    if (!names.empty()) names += ", ";
    names += name;
  }
  for (const auto &[kind, names] : skipped) {
    Z2S_LOG_INFO("ZHA: not bridged, %s is not supported yet: %s", kind.c_str(),
                 names.c_str());
  }

  watchedEntities_.clear();
  for (const auto &device : devices) {
    for (const auto &entityId : entityIds(device)) {
      watchedEntities_.insert(entityId);
    }
  }
  Event event(Event::Type::Devices);
  event.deviceCount = zhaIds.size();
  event.json = json::array();
  for (const auto &state : snapshot.states) {
    if (watchedEntities_.count(state.value("entity_id", "")) != 0) {
      event.json.push_back(state);
    }
  }

  // Device triggers of the buttons, replacing the previous subscriptions.
  std::map<int, TriggerTarget> previous;
  previous.swap(triggerSubscriptions_);
  json ignored;
  for (const auto &[id, target] : previous) {
    if (call(ws, {{"type", "unsubscribe_events"}, {"subscription", id}},
             &ignored) == 0) {
      return false;
    }
  }
  std::map<int, TriggerTarget> subscriptions;
  for (const auto &device : devices) {
    for (const auto &b : device.bindings) {
      if (b.spec.kind != ChannelKind::ActionTrigger) continue;
      for (const auto &[type, action] : b.triggers) {
        json trigger = {{"platform", "device"},
                        {"domain", "zha"},
                        {"device_id", device.haDeviceId},
                        {"type", type},
                        {"subtype", b.triggerSubtype}};
        int id = call(ws, {{"type", "subscribe_trigger"}, {"trigger", trigger}},
                      &ignored);
        if (id == 0) return false;
        if (id > 0) {
          subscriptions[id] = {device.descriptor.id, b.spec.key, action};
        }
      }
    }
  }

  event.devices = std::move(devices);
  pushEvent(std::move(event));
  syncing_ = false;
  for (auto &state : bufferedStates_) {
    pushEvent(Event(Event::Type::State, std::move(state)));
  }
  bufferedStates_.clear();
  // Only now: trigger events must not reach poll() before the device list.
  triggerSubscriptions_ = std::move(subscriptions);
  return true;
}

void ZhaBackend::dispatch(const json &msg) {
  if (msg.value("type", "") != "event") {
    if (msg.value("type", "") == "result" && !msg.value("success", true)) {
      Z2S_LOG_WARNING("ZHA: Home Assistant rejected a command: %s",
                      msg.value("error", json::object()).dump().c_str());
    }
    return;
  }
  int id = msg.value("id", 0);
  const json &event = msg.contains("event") ? msg["event"] : json();
  if (id == stateSubscription_ && stateSubscription_ != 0) {
    if (!event.is_object() || !event.contains("data")) return;
    const json &data = event["data"];
    if (!data.is_object() || !data.contains("new_state") ||
        !data["new_state"].is_object()) {
      return;
    }
    const json &state = data["new_state"];
    if (syncing_) {
      bufferedStates_.push_back(state);
      return;
    }
    if (watchedEntities_.count(state.value("entity_id", "")) == 0) return;
    pushEvent(Event(Event::Type::State, state));
    return;
  }
  if (registrySubscriptions_.count(id) != 0) {
    registryChanged_ = true;
    registryChangedMs_ = nowMs();
    return;
  }
  auto it = triggerSubscriptions_.find(id);
  if (it != triggerSubscriptions_.end()) {
    Event e(Event::Type::Action);
    e.deviceId = it->second.deviceId;
    e.key = it->second.key;
    e.action = it->second.action;
    pushEvent(std::move(e));
  }
}

}  // namespace z2s::zha
