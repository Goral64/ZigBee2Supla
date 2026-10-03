// SPDX-License-Identifier: GPL-2.0-or-later

#include "websocket_client.h"

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace z2s {

namespace {

constexpr char kWebSocketGuid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr size_t kMaxMessageSize = 64 * 1024 * 1024;

std::string base64(const unsigned char *data, size_t len) {
  std::string out(4 * ((len + 2) / 3), '\0');
  int n = EVP_EncodeBlock(reinterpret_cast<unsigned char *>(&out[0]), data,
                          static_cast<int>(len));
  out.resize(n);
  return out;
}

std::string toLower(std::string s) {
  for (auto &c : s) c = static_cast<char>(std::tolower(c));
  return s;
}

std::string headerValue(const std::string &headers, const std::string &name) {
  std::string lower = toLower(headers);
  std::string key = "\r\n" + toLower(name) + ":";
  size_t pos = lower.find(key);
  if (pos == std::string::npos) return "";
  size_t start = pos + key.size();
  size_t end = headers.find("\r\n", start);
  std::string value = headers.substr(start, end - start);
  size_t b = value.find_first_not_of(" \t");
  size_t e = value.find_last_not_of(" \t");
  return b == std::string::npos ? "" : value.substr(b, e - b + 1);
}

}  // namespace

WebSocketClient::~WebSocketClient() { close(); }

bool WebSocketClient::parseUrl(const std::string &url, Url *out) {
  std::string rest;
  if (url.rfind("ws://", 0) == 0) {
    out->secure = false;
    out->port = 80;
    rest = url.substr(5);
  } else if (url.rfind("wss://", 0) == 0) {
    out->secure = true;
    out->port = 443;
    rest = url.substr(6);
  } else {
    return false;
  }
  size_t slash = rest.find('/');
  std::string hostPort = rest.substr(0, slash);
  out->path = slash == std::string::npos ? "/" : rest.substr(slash);
  size_t colon = hostPort.rfind(':');
  if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
    try {
      out->port = std::stoi(hostPort.substr(colon + 1));
    } catch (const std::exception &) {
      return false;
    }
    hostPort = hostPort.substr(0, colon);
  }
  out->host = hostPort;
  return !out->host.empty() && out->port > 0 && out->port < 65536;
}

bool WebSocketClient::connect(const std::string &url, int timeoutMs,
                              std::string *error) {
  close();
  Url u;
  if (!parseUrl(url, &u)) {
    *error = "invalid WebSocket URL: " + url;
    return false;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *result = nullptr;
  std::string port = std::to_string(u.port);
  int r = getaddrinfo(u.host.c_str(), port.c_str(), &hints, &result);
  if (r != 0 || result == nullptr) {
    *error = "cannot resolve " + u.host + ": " + gai_strerror(r);
    return false;
  }
  for (addrinfo *ai = result; ai != nullptr && fd_ < 0; ai = ai->ai_next) {
    int fd = ::socket(ai->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) continue;
    timeval tv{timeoutMs / 1000, (timeoutMs % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
      fd_ = fd;
    } else {
      ::close(fd);
    }
  }
  freeaddrinfo(result);
  if (fd_ < 0) {
    *error = "cannot connect to " + u.host + ":" + port;
    return false;
  }
  int one = 1;
  setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

  if (u.secure) {
    ctx_ = SSL_CTX_new(TLS_client_method());
    SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
    SSL_CTX_set_default_verify_paths(ctx_);
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    ssl_ = SSL_new(ctx_);
    SSL_set_fd(ssl_, fd_);
    SSL_set_tlsext_host_name(ssl_, u.host.c_str());
    SSL_set1_host(ssl_, u.host.c_str());
    if (SSL_connect(ssl_) != 1) {
      long verify = SSL_get_verify_result(ssl_);
      *error = "TLS handshake with " + u.host + " failed" +
               (verify != X509_V_OK
                    ? std::string(": ") + X509_verify_cert_error_string(verify)
                    : std::string());
      ERR_clear_error();
      close();
      return false;
    }
  }

  unsigned char keyBytes[16];
  RAND_bytes(keyBytes, sizeof(keyBytes));
  std::string key = base64(keyBytes, sizeof(keyBytes));
  std::string hostHeader = u.host;
  if ((u.secure && u.port != 443) || (!u.secure && u.port != 80)) {
    hostHeader += ":" + port;
  }
  std::string request = "GET " + u.path +
                        " HTTP/1.1\r\n"
                        "Host: " +
                        hostHeader +
                        "\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Key: " +
                        key +
                        "\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n";
  if (!writeAll(request.data(), request.size())) {
    *error = "cannot send WebSocket handshake";
    close();
    return false;
  }

  std::string response;
  char buf[1024];
  while (response.find("\r\n\r\n") == std::string::npos) {
    int n = readSome(buf, sizeof(buf), timeoutMs);
    if (n <= 0 || response.size() > 16384) {
      *error = "no valid WebSocket handshake response";
      close();
      return false;
    }
    response.append(buf, n);
  }
  size_t headerEnd = response.find("\r\n\r\n") + 4;
  pending_.assign(response.begin() + headerEnd, response.end());
  std::string headers = response.substr(0, headerEnd);

  if (headers.rfind("HTTP/1.1 101", 0) != 0) {
    *error =
        "WebSocket upgrade refused: " + headers.substr(0, headers.find("\r\n"));
    close();
    return false;
  }
  unsigned char digest[SHA_DIGEST_LENGTH];
  std::string accept = key + kWebSocketGuid;
  SHA1(reinterpret_cast<const unsigned char *>(accept.data()), accept.size(),
       digest);
  if (headerValue(headers, "Sec-WebSocket-Accept") !=
      base64(digest, sizeof(digest))) {
    *error = "invalid Sec-WebSocket-Accept";
    close();
    return false;
  }
  return true;
}

void WebSocketClient::close() {
  if (ssl_ != nullptr) {
    SSL_free(ssl_);
    ssl_ = nullptr;
  }
  if (ctx_ != nullptr) {
    SSL_CTX_free(ctx_);
    ctx_ = nullptr;
  }
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  pending_.clear();
}

bool WebSocketClient::writeAll(const void *data, size_t len) {
  const char *p = static_cast<const char *>(data);
  while (len > 0) {
    int n;
    if (ssl_ != nullptr) {
      n = SSL_write(ssl_, p, static_cast<int>(len));
    } else {
      n = static_cast<int>(::send(fd_, p, len, MSG_NOSIGNAL));
    }
    if (n <= 0) {
      if (ssl_ == nullptr && n < 0 && errno == EINTR) continue;
      return false;
    }
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

int WebSocketClient::readSome(void *data, size_t len, int timeoutMs) {
  if (fd_ < 0) return -1;
  if (ssl_ == nullptr || SSL_pending(ssl_) == 0) {
    pollfd pfd{fd_, POLLIN, 0};
    int r = ::poll(&pfd, 1, timeoutMs);
    if (r <= 0) return -1;
  }
  if (ssl_ != nullptr) {
    return SSL_read(ssl_, data, static_cast<int>(len));
  }
  return static_cast<int>(::recv(fd_, data, len, 0));
}

bool WebSocketClient::waitReadable(int timeoutMs) {
  if (fd_ < 0) return false;
  if (!pending_.empty()) return true;
  if (ssl_ != nullptr && SSL_pending(ssl_) > 0) return true;
  pollfd pfd{fd_, POLLIN, 0};
  return ::poll(&pfd, 1, timeoutMs) > 0;
}

bool WebSocketClient::readExact(void *data, size_t len, int timeoutMs) {
  char *p = static_cast<char *>(data);
  size_t fromPending = std::min(len, pending_.size());
  if (fromPending > 0) {
    memcpy(p, pending_.data(), fromPending);
    pending_.erase(pending_.begin(), pending_.begin() + fromPending);
    p += fromPending;
    len -= fromPending;
  }
  while (len > 0) {
    int n = readSome(p, len, timeoutMs);
    if (n <= 0) return false;
    p += n;
    len -= static_cast<size_t>(n);
  }
  return true;
}

bool WebSocketClient::sendFrame(uint8_t opcode, const std::string &payload) {
  std::string frame;
  frame.push_back(static_cast<char>(0x80 | opcode));  // FIN + opcode
  size_t len = payload.size();
  if (len < 126) {
    frame.push_back(static_cast<char>(0x80 | len));  // masked
  } else if (len <= 0xFFFF) {
    frame.push_back(static_cast<char>(0x80 | 126));
    frame.push_back(static_cast<char>((len >> 8) & 0xFF));
    frame.push_back(static_cast<char>(len & 0xFF));
  } else {
    frame.push_back(static_cast<char>(0x80 | 127));
    for (int i = 7; i >= 0; i--) {
      frame.push_back(
          static_cast<char>((static_cast<uint64_t>(len) >> (8 * i)) & 0xFF));
    }
  }
  unsigned char mask[4];
  RAND_bytes(mask, sizeof(mask));
  frame.append(reinterpret_cast<char *>(mask), 4);
  for (size_t i = 0; i < len; i++) {
    frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
  }
  return writeAll(frame.data(), frame.size());
}

bool WebSocketClient::sendText(const std::string &message, std::string *error) {
  if (fd_ < 0 || !sendFrame(0x1, message)) {
    *error = "WebSocket send failed";
    close();
    return false;
  }
  return true;
}

bool WebSocketClient::receiveText(std::string *message, int timeoutMs,
                                  std::string *error) {
  message->clear();
  bool inMessage = false;
  while (fd_ >= 0) {
    unsigned char header[2];
    if (!readExact(header, 2, timeoutMs)) {
      *error = "WebSocket receive timeout or connection closed";
      close();
      return false;
    }
    bool fin = header[0] & 0x80;
    uint8_t opcode = header[0] & 0x0F;
    bool masked = header[1] & 0x80;
    uint64_t len = header[1] & 0x7F;
    if (len == 126) {
      unsigned char ext[2];
      if (!readExact(ext, 2, timeoutMs)) break;
      len = (ext[0] << 8) | ext[1];
    } else if (len == 127) {
      unsigned char ext[8];
      if (!readExact(ext, 8, timeoutMs)) break;
      len = 0;
      for (int i = 0; i < 8; i++) len = (len << 8) | ext[i];
    }
    if (len > kMaxMessageSize || message->size() + len > kMaxMessageSize) {
      *error = "WebSocket message too large";
      close();
      return false;
    }
    unsigned char mask[4] = {0, 0, 0, 0};
    if (masked && !readExact(mask, 4, timeoutMs)) break;
    std::string payload(static_cast<size_t>(len), '\0');
    if (len > 0 && !readExact(&payload[0], payload.size(), timeoutMs)) break;
    if (masked) {
      for (size_t i = 0; i < payload.size(); i++) payload[i] ^= mask[i % 4];
    }

    switch (opcode) {
      case 0x9:  // ping
        if (!sendFrame(0xA, payload)) {
          *error = "WebSocket send failed";
          close();
          return false;
        }
        continue;
      case 0xA:  // pong
        continue;
      case 0x8:  // close
        *error = "WebSocket closed by server";
        close();
        return false;
      case 0x1:  // text
      case 0x2:  // binary
        if (inMessage) {
          *error = "WebSocket protocol error";
          close();
          return false;
        }
        inMessage = true;
        message->append(payload);
        break;
      case 0x0:  // continuation
        if (!inMessage) {
          *error = "WebSocket protocol error";
          close();
          return false;
        }
        message->append(payload);
        break;
      default:
        *error = "WebSocket protocol error";
        close();
        return false;
    }
    if (inMessage && fin) return true;
  }
  if (error->empty()) *error = "WebSocket connection lost";
  close();
  return false;
}

}  // namespace z2s
