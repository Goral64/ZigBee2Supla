// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "ha_sync.h"
#include "openssl_transport.h"
#include "z2m_backend.h"
#include "z2s/gateway.h"
#include "zha_backend.h"

namespace z2s {

struct AppConfig {
  GatewayConfig gateway;
  TlsConfig tls;
  // Source of the devices: "z2m" (zigbee2mqtt over MQTT) or "zha" (ZHA in
  // Home Assistant, over its WebSocket API).
  std::string source = "z2m";
  z2m::MqttConfig mqtt;
  zha::HaConfig zha;
  HaSyncConfig ha;
  std::string stateDir = ".";
  std::string logLevel = "info";
};

// Returns the value of an environment variable or nullptr.
using EnvLookup = std::function<const char *(const char *name)>;

// Loads the configuration. Sources, in order of precedence:
//  1. environment variables Z2S_<OPTION> (e.g. Z2S_SUPLA_SERVER); for every
//     option Z2S_<OPTION>_FILE reads the value from a file (Docker secrets),
//  2. the file `path`: YAML when it ends with .yaml or .yml, otherwise JSON
//     (flat object, the same format as Home Assistant add-on
//     /data/options.json); may be empty when only the environment is
//     used,
//  3. defaults.
// Returns false and fills *error on failure.
bool loadConfig(const std::string &path, const EnvLookup &env,
                AppConfig *config, std::string *error);

// Names of all options (for documentation and --help).
const std::vector<std::string> &configOptionNames();

}  // namespace z2s
