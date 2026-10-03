// SPDX-License-Identifier: GPL-2.0-or-later
// HaDuplicateDisabler + WebSocketClient against an in-process fake Home
// Assistant WebSocket API.

#include "ha_sync.h"
#include "fake_websocket.h"
#include "websocket_client.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace z2s;
using namespace z2s::test;
using json = nlohmann::json;

namespace {

const bool kSigpipeIgnored = [] {
  signal(SIGPIPE, SIG_IGN);
  return true;
}();

class FakeHomeAssistant {
 public:
  explicit FakeHomeAssistant(json devices) : devices_(std::move(devices)) {
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
  ~FakeHomeAssistant() {
    stop_ = true;
    shutdown(listenFd_, SHUT_RDWR);
    close(listenFd_);
    thread_.join();
  }

  std::string url() const {
    return "ws://127.0.0.1:" + std::to_string(port_) + "/api/websocket";
  }

  std::string token = "secret-token";
  std::vector<std::string> updated;  // device ids disabled via update
  std::atomic<int> connections{0};

 private:
  void run() {
    while (!stop_) {
      int fd = accept(listenFd_, nullptr, nullptr);
      if (fd < 0) return;
      connections++;
      handle(fd);
      close(fd);
    }
  }

  void handle(int fd) {
    if (!acceptHandshake(fd, "/api/websocket")) return;

    sendFrame(fd, 0x1, R"({"type":"auth_required","ha_version":"2026.9.0"})");
    std::string payload;
    if (readFrame(fd, &payload) != 0x1) return;
    json auth = json::parse(payload);
    if (auth.value("access_token", "") != token) {
      sendFrame(fd, 0x1, R"({"type":"auth_invalid","message":"Invalid"})");
      return;
    }
    sendFrame(fd, 0x1, R"({"type":"auth_ok","ha_version":"2026.9.0"})");

    while (true) {
      int op = readFrame(fd, &payload);
      if (op == 0xA) continue;  // pong
      if (op != 0x1) return;
      json msg = json::parse(payload);
      int id = msg["id"];
      std::string type = msg["type"];
      if (type == "config/device_registry/list") {
        // A ping and a fragmented result exercise the client framing.
        sendFrame(fd, 0x9, "hb");
        std::string result = json({{"id", id},
                                   {"type", "result"},
                                   {"success", true},
                                   {"result", devices_}})
                                 .dump();
        size_t half = result.size() / 2;
        sendFrame(fd, 0x1, result.substr(0, half), false);
        sendFrame(fd, 0x0, result.substr(half), true);
      } else if (type == "config/device_registry/update") {
        std::string deviceId = msg["device_id"];
        updated.push_back(deviceId);
        for (auto &d : devices_) {
          if (d["id"] == deviceId) d["disabled_by"] = msg["disabled_by"];
        }
        sendFrame(fd, 0x1,
                  json({{"id", id},
                        {"type", "result"},
                        {"success", true},
                        {"result", json::object()}})
                      .dump());
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

 public:
  json devices_;

 private:
  int listenFd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
};

json sampleDevices() {
  return json::parse(R"([
    {"id": "dup1", "name": "Salon/czujnik", "sw_version": "z2s 0.1.0",
     "disabled_by": null, "identifiers": [["mqtt", "supla-iodevice-101"]]},
    {"id": "z2m1", "name": "Salon/czujnik", "sw_version": "3000-0001",
     "disabled_by": null,
     "identifiers": [["mqtt", "zigbee2mqtt_0x00158d0001a2b3c4"]]},
    {"id": "dup2", "name": "Lampa", "sw_version": "z2s 0.1.0",
     "disabled_by": null, "identifiers": [["mqtt", "supla-iodevice-102"]]},
    {"id": "supla", "name": "ROW-01", "sw_version": "2.8.53",
     "disabled_by": null, "identifiers": [["mqtt", "supla-iodevice-7"]]}
  ])");
}

class HaSyncTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char tmpl[] = "/tmp/z2s_ha_XXXXXX";
    dir_ = mkdtemp(tmpl);
  }
  void TearDown() override {
    unlink((dir_ + "/ha.json").c_str());
    rmdir(dir_.c_str());
  }
  HaSyncConfig config(const FakeHomeAssistant &ha) {
    HaSyncConfig c;
    c.enabled = true;
    c.websocketUrl = ha.url();
    c.token = "secret-token";
    c.stateFile = dir_ + "/ha.json";
    c.timeoutMs = 3000;
    return c;
  }
  std::string dir_;
};

}  // namespace

TEST_F(HaSyncTest, DisablesSuplaDuplicatesOnce) {
  FakeHomeAssistant ha(sampleDevices());
  std::string error;
  {
    HaDuplicateDisabler disabler(config(ha));
    EXPECT_EQ(disabler.runOnce(&error), 2) << error;
  }
  EXPECT_EQ(ha.updated, (std::vector<std::string>{"dup1", "dup2"}));

  // The user enables "dup1" again; a new instance (restart) must respect it.
  ha.devices_[0]["disabled_by"] = nullptr;
  HaDuplicateDisabler disabler(config(ha));
  EXPECT_EQ(disabler.runOnce(&error), 0) << error;
  EXPECT_EQ(ha.updated.size(), 2u);
}

TEST_F(HaSyncTest, InvalidTokenIsReported) {
  FakeHomeAssistant ha(sampleDevices());
  auto c = config(ha);
  c.token = "wrong";
  HaDuplicateDisabler disabler(c);
  std::string error;
  EXPECT_EQ(disabler.runOnce(&error), -1);
  EXPECT_NE(error.find("authentication"), std::string::npos);
  EXPECT_TRUE(ha.updated.empty());
}

TEST_F(HaSyncTest, ConnectionErrorIsReported) {
  HaSyncConfig c;
  c.enabled = true;
  c.websocketUrl = "ws://127.0.0.1:1/api/websocket";
  c.token = "x";
  c.stateFile = dir_ + "/ha.json";
  c.timeoutMs = 1000;
  HaDuplicateDisabler disabler(c);
  std::string error;
  EXPECT_EQ(disabler.runOnce(&error), -1);
  EXPECT_FALSE(error.empty());
}

TEST(WebSocketUrlTest, Parse) {
  WebSocketClient::Url u;
  ASSERT_TRUE(WebSocketClient::parseUrl("ws://supervisor/core/websocket", &u));
  EXPECT_FALSE(u.secure);
  EXPECT_EQ(u.host, "supervisor");
  EXPECT_EQ(u.port, 80);
  EXPECT_EQ(u.path, "/core/websocket");
  ASSERT_TRUE(
      WebSocketClient::parseUrl("wss://ha.local:8123/api/websocket", &u));
  EXPECT_TRUE(u.secure);
  EXPECT_EQ(u.port, 8123);
  EXPECT_FALSE(WebSocketClient::parseUrl("http://ha.local:8123", &u));
  EXPECT_FALSE(WebSocketClient::parseUrl("ws://:80/x", &u));
}
