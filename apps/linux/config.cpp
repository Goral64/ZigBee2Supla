// SPDX-License-Identifier: GPL-2.0-or-later

#include "config.h"

#include <proto.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <cctype>
#include <climits>
#include <fstream>
#include <iterator>
#include <nlohmann/json.hpp>

#include "websocket_client.h"
#include "z2s/protocol_info.h"

namespace z2s {

using json = nlohmann::json;

namespace {

enum class OptionType { String, Int, Bool, List };

struct Option {
  const char *name;
  OptionType type;
};

const std::vector<Option> &options() {
  static const std::vector<Option> kOptions = {
      {"source", OptionType::String},
      {"supla_server", OptionType::String},
      {"supla_email", OptionType::String},
      {"supla_port", OptionType::Int},
      {"supla_proto_version", OptionType::Int},
      {"supla_activity_timeout", OptionType::Int},
      {"supla_security_level", OptionType::Int},
      {"supla_ca_file", OptionType::String},
      {"disable_tls", OptionType::Bool},
      {"mqtt_host", OptionType::String},
      {"mqtt_port", OptionType::Int},
      {"mqtt_username", OptionType::String},
      {"mqtt_password", OptionType::String},
      {"mqtt_client_id", OptionType::String},
      {"z2m_base_topic", OptionType::String},
      {"max_parallel_connects", OptionType::Int},
      {"connect_interval_ms", OptionType::Int},
      {"include", OptionType::List},
      {"exclude", OptionType::List},
      {"state_dir", OptionType::String},
      {"log_level", OptionType::String},
      {"ha_disable_supla_duplicates", OptionType::Bool},
      {"ha_websocket_url", OptionType::String},
      {"ha_token", OptionType::String},
  };
  return kOptions;
}

std::string envName(const std::string &option) {
  std::string name = "Z2S_";
  for (char c : option) {
    name.push_back(
        static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
  }
  return name;
}

std::string trim(const std::string &s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Converts the text form of an option (environment variable, YAML scalar) to
// its JSON value. `label` names the source in error messages.
bool convert(const Option &option, const std::string &raw,
             const std::string &label, json *out, std::string *error) {
  const std::string value = trim(raw);
  switch (option.type) {
    case OptionType::String:
      *out = value;
      return true;
    case OptionType::Int: {
      size_t pos = 0;
      try {
        long v = std::stol(value, &pos);
        if (pos == value.size()) {
          *out = v;
          return true;
        }
      } catch (const std::exception &) {
      }
      *error = label + ": not an integer: '" + value + "'";
      return false;
    }
    case OptionType::Bool: {
      std::string v;
      for (char c : value) v.push_back(static_cast<char>(std::tolower(c)));
      if (v == "1" || v == "true" || v == "yes" || v == "on") {
        *out = true;
      } else if (v == "0" || v == "false" || v == "no" || v == "off") {
        *out = false;
      } else {
        *error = label + ": not a boolean: '" + value + "'";
        return false;
      }
      return true;
    }
    case OptionType::List: {
      json list = json::array();
      size_t start = 0;
      while (start <= value.size()) {
        size_t comma = value.find(',', start);
        if (comma == std::string::npos) comma = value.size();
        std::string item = trim(value.substr(start, comma - start));
        if (!item.empty()) list.push_back(item);
        start = comma + 1;
      }
      *out = list;
      return true;
    }
  }
  return false;
}

bool readFile(const std::string &path, std::string *out) {
  std::ifstream in(path);
  if (!in.is_open()) return false;
  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  // Secret files usually end with a newline.
  while (!content.empty() &&
         (content.back() == '\n' || content.back() == '\r')) {
    content.pop_back();
  }
  *out = content;
  return true;
}

// Applies Z2S_* environment variables on top of the options read from a file.
bool applyEnvironment(const EnvLookup &env, json *j, std::string *error) {
  for (const auto &option : options()) {
    const std::string name = envName(option.name);
    std::string raw;
    if (const char *fileVar = env((name + "_FILE").c_str())) {
      if (!readFile(fileVar, &raw)) {
        *error = name + "_FILE: cannot read " + std::string(fileVar);
        return false;
      }
    } else if (const char *value = env(name.c_str())) {
      raw = value;
    } else {
      continue;
    }
    json converted;
    if (!convert(option, raw, name, &converted, error)) return false;
    (*j)[option.name] = converted;
  }
  return true;
}

// Default MQTT client id. The broker disconnects a client when another one
// connects with the same id, so two instances (e.g. a container and a Home
// Assistant add-on) must differ: the host name tells them apart.
std::string defaultMqttClientId() {
  char host[HOST_NAME_MAX + 1] = {};
  if (gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0') {
    return "zigbee2supla";
  }
  return std::string("zigbee2supla-") + host;
}

bool hasSuffix(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool isYamlPath(const std::string &path) {
  return hasSuffix(path, ".yaml") || hasSuffix(path, ".yml");
}

// Reads a YAML file into the same flat JSON object that a JSON config file
// produces. YAML scalars are untyped, so the declared option types decide how
// they are read. Unknown keys are ignored, like in JSON files.
bool loadYaml(const std::string &path, json *j, std::string *error) {
  YAML::Node loaded;
  try {
    loaded = YAML::LoadFile(path);
  } catch (const YAML::BadFile &) {
    *error = "cannot open " + path;
    return false;
  } catch (const YAML::Exception &e) {
    *error = std::string("invalid YAML: ") + e.what();
    return false;
  }
  if (loaded.IsNull()) return true;  // empty file
  if (!loaded.IsMap()) {
    *error = "config must be a YAML mapping";
    return false;
  }
  // Indexing a const node does not create missing keys.
  const YAML::Node &root = loaded;
  for (const auto &option : options()) {
    const YAML::Node node = root[option.name];
    if (!node.IsDefined() || node.IsNull()) continue;
    const std::string name = option.name;
    if (option.type == OptionType::List && node.IsSequence()) {
      json list = json::array();
      for (const auto &item : node) {
        if (!item.IsScalar()) {
          *error = name + ": list items must be plain values";
          return false;
        }
        list.push_back(item.as<std::string>());
      }
      (*j)[name] = list;
      continue;
    }
    if (!node.IsScalar()) {
      *error = name + ": unexpected nested value";
      return false;
    }
    if (option.type == OptionType::String) {
      (*j)[name] = node.as<std::string>();
      continue;
    }
    json converted;
    if (!convert(option, node.as<std::string>(), name, &converted, error)) {
      return false;
    }
    (*j)[name] = converted;
  }
  return true;
}

}  // namespace

const std::vector<std::string> &configOptionNames() {
  static const std::vector<std::string> kNames = [] {
    std::vector<std::string> names;
    for (const auto &option : options()) names.push_back(option.name);
    return names;
  }();
  return kNames;
}

bool loadConfig(const std::string &path, const EnvLookup &env,
                AppConfig *config, std::string *error) {
  json j = json::object();
  if (isYamlPath(path)) {
    if (!loadYaml(path, &j, error)) return false;
  } else if (!path.empty()) {
    std::ifstream in(path);
    if (!in.is_open()) {
      *error = "cannot open " + path;
      return false;
    }
    try {
      in >> j;
    } catch (const std::exception &e) {
      *error = std::string("invalid JSON: ") + e.what();
      return false;
    }
    if (!j.is_object()) {
      *error = "config must be a JSON object";
      return false;
    }
  }
  if (!applyEnvironment(env, &j, error)) {
    return false;
  }

  try {
    auto &supla = config->gateway.supla;
    supla.server = j.value("supla_server", "");
    supla.email = j.value("supla_email", "");
    supla.port = j.value("supla_port", 2016);
    supla.protoVersion = j.value("supla_proto_version", kDefaultProtoVersion);
    supla.activityTimeoutS = j.value("supla_activity_timeout", 120);

    config->tls.securityLevel = j.value("supla_security_level", 0);
    config->tls.caFile = j.value("supla_ca_file", "");
    config->tls.disableTls = j.value("disable_tls", false);

    config->mqtt.host = j.value("mqtt_host", "localhost");
    config->mqtt.port = j.value("mqtt_port", 1883);
    config->mqtt.username = j.value("mqtt_username", "");
    config->mqtt.password = j.value("mqtt_password", "");
    config->mqtt.clientId = j.value("mqtt_client_id", defaultMqttClientId());
    config->mqtt.baseTopic = j.value("z2m_base_topic", "zigbee2mqtt");

    config->gateway.maxParallelConnects = j.value("max_parallel_connects", 4);
    config->gateway.connectIntervalMs = j.value("connect_interval_ms", 250);
    config->gateway.include = j.value("include", std::vector<std::string>{});
    config->gateway.exclude = j.value("exclude", std::vector<std::string>{});

    config->stateDir = j.value("state_dir", ".");
    config->logLevel = j.value("log_level", "info");

    config->ha.enabled = j.value("ha_disable_supla_duplicates", false);
    config->ha.websocketUrl = j.value("ha_websocket_url", "");
    config->ha.token = j.value("ha_token", "");

    config->source = j.value("source", "z2m");
    config->zha.websocketUrl = config->ha.websocketUrl;
    config->zha.token = config->ha.token;
  } catch (const std::exception &e) {
    *error = std::string("invalid option type: ") + e.what();
    return false;
  }

  if (config->gateway.supla.server.empty()) {
    *error = "supla_server is required";
    return false;
  }
  if (config->gateway.supla.email.empty()) {
    *error = "supla_email is required";
    return false;
  }
  if (config->gateway.supla.protoVersion < kMinProtoVersion ||
      config->gateway.supla.protoVersion > SUPLA_PROTO_VERSION) {
    *error = "supla_proto_version must be between " +
             std::to_string(kMinProtoVersion) + " and " +
             std::to_string(SUPLA_PROTO_VERSION);
    return false;
  }
  if (config->tls.securityLevel < 0 || config->tls.securityLevel > 3) {
    *error = "supla_security_level must be 0, 1, 2 or 3";
    return false;
  }
  if ((config->tls.securityLevel == 1 || config->tls.securityLevel == 3) &&
      config->tls.caFile.empty()) {
    *error = "supla_ca_file is required for supla_security_level 1 and 3";
    return false;
  }
  if (config->source != "z2m" && config->source != "zha") {
    *error = "source must be z2m or zha";
    return false;
  }
  if (config->source == "zha") {
    WebSocketClient::Url url;
    if (!WebSocketClient::parseUrl(config->ha.websocketUrl, &url)) {
      *error =
          "source zha needs ha_websocket_url, a ws:// or wss:// URL, e.g. "
          "ws://192.168.1.10:8123/api/websocket";
      return false;
    }
    if (config->ha.token.empty()) {
      *error = "ha_token is required for source zha";
      return false;
    }
  }
  if (config->ha.enabled) {
    WebSocketClient::Url url;
    if (!WebSocketClient::parseUrl(config->ha.websocketUrl, &url)) {
      *error =
          "ha_websocket_url must be a ws:// or wss:// URL, e.g. "
          "ws://192.168.1.10:8123/api/websocket";
      return false;
    }
    if (config->ha.token.empty()) {
      *error = "ha_token is required when ha_disable_supla_duplicates is on";
      return false;
    }
  }
  if (config->gateway.maxParallelConnects < 1) {
    *error = "max_parallel_connects must be >= 1";
    return false;
  }
  return true;
}

}  // namespace z2s
