// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <memory>
#include <string>

namespace z2s {

enum class TransportStatus { Idle, Connecting, Connected, Failed };

// Non-blocking, stream oriented connection to the Supla server (normally
// TCP + TLS). Implemented per platform.
class Transport {
 public:
  virtual ~Transport() = default;
  // Starts a non-blocking connection. Returns false on immediate failure.
  virtual bool connect(const std::string &host, int port) = 0;
  // Advances connection/handshake and reports the current status.
  virtual TransportStatus status() = 0;
  // Returns number of bytes read (>0), -1 when no data is available yet,
  // 0 when the connection was closed or failed.
  virtual int read(void *buf, int len) = 0;
  // Queues data for sending. Returns false when the connection is broken.
  virtual bool write(const void *buf, int len) = 0;
  // Sends as much queued data as possible. Returns false on error.
  virtual bool flush() = 0;
  virtual void close() = 0;
  // Socket descriptor for poll(), -1 when not connected.
  virtual int fd() const = 0;
  virtual bool hasPendingWrite() const = 0;
};

class TransportFactory {
 public:
  virtual ~TransportFactory() = default;
  virtual std::unique_ptr<Transport> create() = 0;
};

}  // namespace z2s
