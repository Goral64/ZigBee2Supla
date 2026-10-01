// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <netinet/in.h>
#include <openssl/ssl.h>
#include <sys/socket.h>

#include <cstdint>
#include <memory>
#include <string>

#include "z2s/transport.h"

namespace z2s {

struct TlsConfig {
  // 0 - verify server certificate with Supla CA (default),
  // 1 - verify with CA from caFile (chain, validity and host name),
  // 2 - no certificate verification (insecure, e.g. self-signed private
  //     server in a trusted LAN),
  // 3 - pinned certificate: the server must present exactly the certificate
  //     from caFile; host name and validity dates are not checked. Meant for
  //     self-signed certificates of local servers (supla-docker generates one
  //     with CN=SUPLA, valid for 365 days).
  int securityLevel = 0;
  std::string caFile;
  // Plain TCP without TLS (only for tests with a local fake server).
  bool disableTls = false;
};

// Creates non-blocking TCP+TLS connections. Shares one SSL_CTX, the resolved
// server address and the last TLS session (for session resumption) between
// all connections, which matters with 100+ devices.
// The process must ignore SIGPIPE (OpenSSL writes TLS alerts and
// close_notify with plain write()).
class OpenSslTransportFactory : public TransportFactory {
 public:
  explicit OpenSslTransportFactory(const TlsConfig &config);
  ~OpenSslTransportFactory() override;

  bool init(const std::string &serverHost);
  std::unique_ptr<Transport> create() override;

  // Internal, used by transports.
  SSL_CTX *ctx() const { return ctx_; }
  const TlsConfig &config() const { return config_; }
  bool resolve(const std::string &host, int port, sockaddr_storage *addr,
               socklen_t *addrLen);
  void storeSession(SSL_SESSION *session);
  SSL_SESSION *session() const { return lastSession_; }
  X509 *pinnedCert() const { return pinnedCert_; }

 private:
  TlsConfig config_;
  SSL_CTX *ctx_ = nullptr;
  SSL_SESSION *lastSession_ = nullptr;
  X509 *pinnedCert_ = nullptr;
  std::string cachedHost_;
  int cachedPort_ = 0;
  sockaddr_storage cachedAddr_{};
  socklen_t cachedAddrLen_ = 0;
  uint64_t cachedAtMs_ = 0;
};

}  // namespace z2s
