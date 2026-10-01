// SPDX-License-Identifier: GPL-2.0-or-later

#include "ha_sync.h"

#include <cstdio>
#include <fstream>
#include <nlohmann/json.hpp>

#include "ha_duplicates.h"
#include "websocket_client.h"
#include "z2s/log.h"

namespace z2s {

using json = nlohmann::json;

namespace {

// Sends a command and waits for its result (other messages are skipped).
bool call(WebSocketClient *ws, int id, json command, int timeoutMs,
          json *result, std::string *error) {
  command["id"] = id;
  if (!ws->sendText(command.dump(), error)) return false;
  while (true) {
    std::string text;
    if (!ws->receiveText(&text, timeoutMs, error)) return false;
    json msg = json::parse(text, nullptr, false);
    if (msg.is_discarded() || msg.value("id", -1) != id ||
        msg.value("type", "") != "result") {
      continue;
    }
    if (!msg.value("success", false)) {
      *error = "Home Assistant rejected '" + command.value("type", "") +
               "': " + msg.value("error", json::object()).dump();
      return false;
    }
    *result = msg.value("result", json());
    return true;
  }
}

}  // namespace

HaDuplicateDisabler::HaDuplicateDisabler(const HaSyncConfig &config)
    : config_(config) {
  loadState();
}

HaDuplicateDisabler::~HaDuplicateDisabler() { stop(); }

void HaDuplicateDisabler::loadState() {
  std::ifstream in(config_.stateFile);
  if (!in.is_open()) return;
  json j = json::parse(in, nullptr, false);
  if (j.is_discarded() || !j.is_object()) {
    Z2S_LOG_WARNING("HA: ignoring invalid %s", config_.stateFile.c_str());
    return;
  }
  for (const auto &id : j.value("disabled_device_ids", json::array())) {
    if (id.is_string()) handled_.insert(id.get<std::string>());
  }
}

bool HaDuplicateDisabler::saveState() const {
  json j = {{"version", 1}, {"disabled_device_ids", handled_}};
  std::string tmp = config_.stateFile + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.is_open()) return false;
    out << j.dump(2) << "\n";
    if (!out.good()) return false;
  }
  return std::rename(tmp.c_str(), config_.stateFile.c_str()) == 0;
}

int HaDuplicateDisabler::runOnce(std::string *error) {
  WebSocketClient ws;
  if (!ws.connect(config_.websocketUrl, config_.timeoutMs, error)) return -1;

  // Authentication: auth_required -> auth -> auth_ok.
  std::string text;
  if (!ws.receiveText(&text, config_.timeoutMs, error)) return -1;
  json msg = json::parse(text, nullptr, false);
  if (msg.is_discarded() || msg.value("type", "") != "auth_required") {
    *error = "unexpected first message from Home Assistant";
    return -1;
  }
  json auth = {{"type", "auth"}, {"access_token", config_.token}};
  if (!ws.sendText(auth.dump(), error)) return -1;
  if (!ws.receiveText(&text, config_.timeoutMs, error)) return -1;
  msg = json::parse(text, nullptr, false);
  if (msg.is_discarded() || msg.value("type", "") != "auth_ok") {
    *error = "Home Assistant authentication failed (check the token)";
    return -1;
  }

  json devices;
  if (!call(&ws, nextRequestId_++, {{"type", "config/device_registry/list"}},
            config_.timeoutMs, &devices, error)) {
    return -1;
  }

  int disabled = 0;
  for (const auto &device : ha::selectDevicesToDisable(devices, handled_)) {
    json result;
    if (!call(&ws, nextRequestId_++,
              {{"type", "config/device_registry/update"},
               {"device_id", device.id},
               {"disabled_by", "user"}},
              config_.timeoutMs, &result, error)) {
      saveState();
      return -1;
    }
    handled_.insert(device.id);
    disabled++;
    Z2S_LOG_INFO("HA: disabled duplicate Supla device '%s'",
                 device.name.c_str());
  }
  if (disabled > 0 && !saveState()) {
    Z2S_LOG_WARNING("HA: cannot write %s", config_.stateFile.c_str());
  }
  return disabled;
}

void HaDuplicateDisabler::start() {
  if (!config_.enabled || thread_.joinable()) return;
  Z2S_LOG_INFO("HA: disabling Supla duplicates of bridged devices (%s)",
               config_.websocketUrl.c_str());
  thread_ = std::thread([this] { threadMain(); });
}

void HaDuplicateDisabler::stop() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    stopRequested_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void HaDuplicateDisabler::threadMain() {
  int delayS = config_.firstDelayS;
  while (true) {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (cv_.wait_for(lock, std::chrono::seconds(delayS),
                       [this] { return stopRequested_; })) {
        return;
      }
    }
    std::string error;
    if (runOnce(&error) < 0) {
      Z2S_LOG_WARNING("HA: %s", error.c_str());
    }
    delayS = config_.intervalS;
  }
}

}  // namespace z2s
