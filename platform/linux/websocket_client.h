// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <openssl/ssl.h>

#include <cstdint>
#include <string>
#include <vector>

namespace z2s {

// Minimal blocking WebSocket client (RFC 6455) for request/response style
// APIs such as the Home Assistant WebSocket API. Supports ws:// and wss://
// (server verified with the system CA store), text messages, fragmented
// messages and ping/pong.
class WebSocketClient {
 public:
  WebSocketClient() = default;
  ~WebSocketClient();
  WebSocketClient(const WebSocketClient &) = delete;
  WebSocketClient &operator=(const WebSocketClient &) = delete;

  bool connect(const std::string &url, int timeoutMs, std::string *error);
  bool sendText(const std::string &message, std::string *error);
  // Waits for the next complete text message.
  bool receiveText(std::string *message, int timeoutMs, std::string *error);
  void close();
  bool isConnected() const { return fd_ >= 0; }

  struct Url {
    bool secure = false;
    std::string host;
    int port = 0;
    std::string path;
  };
  static bool parseUrl(const std::string &url, Url *out);

 private:
  bool writeAll(const void *data, size_t len);
  // Reads exactly len bytes (uses bytes buffered after the HTTP handshake
  // first).
  bool readExact(void *data, size_t len, int timeoutMs);
  int readSome(void *data, size_t len, int timeoutMs);
  bool sendFrame(uint8_t opcode, const std::string &payload);

  int fd_ = -1;
  SSL_CTX *ctx_ = nullptr;
  SSL *ssl_ = nullptr;
  std::vector<char> pending_;
};

}  // namespace z2s
