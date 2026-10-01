// SPDX-License-Identifier: GPL-2.0-or-later
// HaDuplicateDisabler + WebSocketClient against an in-process fake Home
// Assistant WebSocket API.

#include "ha_sync.h"
#include "websocket_client.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <csignal>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

using namespace z2s;
using json = nlohmann::json;

namespace {

const bool kSigpipeIgnored = [] {
  signal(SIGPIPE, SIG_IGN);
  return true;
}();

bool readN(int fd, void *buf, size_t n) {
  char *p = static_cast<char *>(buf);
  while (n > 0) {
    ssize_t r = recv(fd, p, n, 0);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

void sendFrame(int fd, uint8_t opcode, const std::string &payload,
               bool fin = true) {
  std::string f;
  f.push_back(static_cast<char>((fin ? 0x80 : 0) | opcode));
  if (payload.size() < 126) {
    f.push_back(static_cast<char>(payload.size()));
  } else {
    f.push_back(126);
    f.push_back(static_cast<char>(payload.size() >> 8));
    f.push_back(static_cast<char>(payload.size() & 0xFF));
  }
  f += payload;
  send(fd, f.data(), f.size(), MSG_NOSIGNAL);
}

// Reads one client (masked) frame; returns opcode or -1.
int readFrame(int fd, std::string *payload) {
  unsigned char h[2];
  if (!readN(fd, h, 2)) return -1;
  uint64_t len = h[1] & 0x7F;
  if (len == 126) {
    unsigned char e[2];
    if (!readN(fd, e, 2)) return -1;
    len = (e[0] << 8) | e[1];
  } else if (len == 127) {
    unsigned char e[8];
    if (!readN(fd, e, 8)) return -1;
    len = 0;
    for (int i = 0; i < 8; i++) len = (len << 8) | e[i];
  }
  unsigned char mask[4];
  if (!(h[1] & 0x80) || !readN(fd, mask, 4)) return -1;  // must be masked
  payload->assign(len, '\0');
  if (len > 0 && !readN(fd, &(*payload)[0], len)) return -1;
  for (size_t i = 0; i < len; i++) (*payload)[i] ^= mask[i % 4];
  return h[0] & 0x0F;
}

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
    // HTTP upgrade
    std::string req;
    char c;
    while (req.find("\r\n\r\n") == std::string::npos &&
           recv(fd, &c, 1, 0) == 1) {
      req.push_back(c);
    }
    std::string keyHdr = "Sec-WebSocket-Key: ";
    size_t k = req.find(keyHdr);
    if (k == std::string::npos || req.find("GET /api/websocket") != 0) return;
    std::string key =
        req.substr(k + keyHdr.size(), req.find("\r\n", k) - k - keyHdr.size());
    std::string acc = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char digest[SHA_DIGEST_LENGTH];
    SHA1(reinterpret_cast<const unsigned char *>(acc.data()), acc.size(),
         digest);
    unsigned char b64[64];
    EVP_EncodeBlock(b64, digest, SHA_DIGEST_LENGTH);
    std::string resp =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
        std::string(reinterpret_cast<char *>(b64)) + "\r\n\r\n";
    send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);

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
