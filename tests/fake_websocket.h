// SPDX-License-Identifier: GPL-2.0-or-later
// Server side of the WebSocket protocol for in-process fake servers
// (Home Assistant WebSocket API) in tests.
#pragma once

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <sys/socket.h>

#include <cstdint>
#include <string>

namespace z2s::test {

inline bool readN(int fd, void *buf, size_t n) {
  char *p = static_cast<char *>(buf);
  while (n > 0) {
    ssize_t r = recv(fd, p, n, 0);
    if (r <= 0) return false;
    p += r;
    n -= static_cast<size_t>(r);
  }
  return true;
}

inline void sendFrame(int fd, uint8_t opcode, const std::string &payload,
                      bool fin = true) {
  std::string f;
  f.push_back(static_cast<char>((fin ? 0x80 : 0) | opcode));
  if (payload.size() < 126) {
    f.push_back(static_cast<char>(payload.size()));
  } else if (payload.size() < 65536) {
    f.push_back(126);
    f.push_back(static_cast<char>(payload.size() >> 8));
    f.push_back(static_cast<char>(payload.size() & 0xFF));
  } else {
    f.push_back(127);
    for (int i = 7; i >= 0; i--) {
      f.push_back(static_cast<char>(
          (static_cast<uint64_t>(payload.size()) >> (8 * i)) & 0xFF));
    }
  }
  f += payload;
  send(fd, f.data(), f.size(), MSG_NOSIGNAL);
}

// Reads one client (masked) frame; returns opcode or -1.
inline int readFrame(int fd, std::string *payload) {
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

// Reads the HTTP upgrade request for `path` and accepts it.
inline bool acceptHandshake(int fd, const std::string &path) {
  std::string req;
  char c;
  while (req.find("\r\n\r\n") == std::string::npos && recv(fd, &c, 1, 0) == 1) {
    req.push_back(c);
  }
  std::string keyHdr = "Sec-WebSocket-Key: ";
  size_t k = req.find(keyHdr);
  if (k == std::string::npos || req.find("GET " + path) != 0) return false;
  std::string key =
      req.substr(k + keyHdr.size(), req.find("\r\n", k) - k - keyHdr.size());
  std::string acc = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
  unsigned char digest[SHA_DIGEST_LENGTH];
  SHA1(reinterpret_cast<const unsigned char *>(acc.data()), acc.size(), digest);
  unsigned char b64[64];
  EVP_EncodeBlock(b64, digest, SHA_DIGEST_LENGTH);
  std::string resp =
      "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
      "Connection: Upgrade\r\nSec-WebSocket-Accept: " +
      std::string(reinterpret_cast<char *>(b64)) + "\r\n\r\n";
  send(fd, resp.data(), resp.size(), MSG_NOSIGNAL);
  return true;
}

}  // namespace z2s::test
