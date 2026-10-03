// SPDX-License-Identifier: GPL-2.0-or-later
//
// zigbee2supla for Linux: every ZigBee device known to zigbee2mqtt or ZHA is
// registered in Supla as a separate device.

#include <openssl/rand.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "config.h"
#include "ha_sync.h"
#include "openssl_transport.h"
#include "z2m_backend.h"
#include "z2s/gateway.h"
#include "z2s/identity_store.h"
#include "z2s/log.h"
#include "zha_backend.h"

namespace {

volatile std::sig_atomic_t gStop = 0;

void onSignal(int) { gStop = 1; }

uint64_t monotonicMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void usage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s [-c <config.yaml>]\n"
          "  -c FILE   configuration file (.yaml/.yml, or JSON otherwise);\n"
          "            default: $Z2S_CONFIG\n"
          "  -v        print version\n"
          "\n"
          "Every option can also be set with an environment variable\n"
          "Z2S_<OPTION> (overrides the file) or Z2S_<OPTION>_FILE (value\n"
          "read from a file, e.g. a Docker secret). Options:\n",
          argv0);
  for (const auto &name : z2s::configOptionNames()) {
    fprintf(stderr, "  %s\n", name.c_str());
  }
}

}  // namespace

int main(int argc, char **argv) {
  std::string configPath;
  int opt;
  while ((opt = getopt(argc, argv, "c:vh")) != -1) {
    switch (opt) {
      case 'c':
        configPath = optarg;
        break;
      case 'v':
        printf("zigbee2supla %s\n", Z2S_VERSION);
        return 0;
      default:
        usage(argv[0]);
        return opt == 'h' ? 0 : 1;
    }
  }
  if (configPath.empty()) {
    if (const char *envPath = getenv("Z2S_CONFIG")) configPath = envPath;
  }

  z2s::AppConfig config;
  std::string error;
  if (!z2s::loadConfig(configPath, &getenv, &config, &error)) {
    fprintf(stderr, "Configuration error: %s\n", error.c_str());
    return 1;
  }
  if (!z2s::setLogLevel(config.logLevel)) {
    fprintf(stderr, "Unknown log_level '%s'\n", config.logLevel.c_str());
    return 1;
  }

  signal(SIGINT, onSignal);
  signal(SIGTERM, onSignal);
  signal(SIGPIPE, SIG_IGN);

  Z2S_LOG_INFO("zigbee2supla %s starting (Supla server %s, account %s)",
               Z2S_VERSION, config.gateway.supla.server.c_str(),
               config.gateway.supla.email.c_str());

  z2s::IdentityStore store(config.stateDir + "/identities.json",
                           [](uint8_t *buf, size_t len) {
                             return RAND_bytes(buf, static_cast<int>(len)) == 1;
                           });
  if (!store.load()) {
    Z2S_LOG_ERROR(
        "Refusing to start with a broken identity store - devices would get "
        "new GUIDs and appear as new devices in Supla");
    return 1;
  }

  z2s::OpenSslTransportFactory transportFactory(config.tls);
  if (!transportFactory.init(config.gateway.supla.server)) {
    return 1;
  }

  config.ha.stateFile = config.stateDir + "/ha_disabled_devices.json";
  z2s::HaDuplicateDisabler haDuplicates(config.ha);

  std::unique_ptr<z2s::Backend> backend;
  if (config.source == "zha") {
    backend = std::make_unique<z2s::zha::ZhaBackend>(config.zha);
  } else {
    backend = std::make_unique<z2s::z2m::Z2mBackend>(config.mqtt);
  }
  z2s::Gateway gateway(config.gateway, &store, &transportFactory,
                       backend.get());
  backend->setListener(&gateway);
  if (!backend->start()) {
    return 1;
  }
  haDuplicates.start();

  uint64_t lastStatusMs = monotonicMs();
  size_t lastRegistered = static_cast<size_t>(-1);
  std::vector<pollfd> pfds;

  while (!gStop) {
    pfds.clear();
    for (const auto &p : gateway.pollFds()) {
      short events = POLLIN;
      if (p.wantWrite) events |= POLLOUT;
      pfds.push_back({p.fd, events, 0});
    }
    // Wake up at least every 20 ms to process MQTT events and timers.
    ::poll(pfds.data(), pfds.size(), 20);

    backend->poll();
    uint64_t now = monotonicMs();
    gateway.iterate(now);

    if (now - lastStatusMs >= 60000 ||
        (gateway.registeredCount() != lastRegistered &&
         now - lastStatusMs >= 5000)) {
      lastStatusMs = now;
      lastRegistered = gateway.registeredCount();
      // Bridged devices are either connected to Supla, unreachable in ZigBee
      // (not connected on purpose) or still connecting / refused by Supla.
      size_t bridged = gateway.sessionCount();
      size_t connected = gateway.registeredCount();
      size_t offline = gateway.offlineCount();
      size_t other =
          bridged > connected + offline ? bridged - connected - offline : 0;
      Z2S_LOG_INFO(
          "Status: %zu ZigBee device(s), %zu bridged: %zu connected to "
          "Supla, %zu offline in ZigBee, %zu connecting or failed",
          backend->deviceCount(), bridged, connected, offline, other);
    }
  }

  Z2S_LOG_INFO("Stopping");
  haDuplicates.stop();
  backend->stop();
  return 0;
}
