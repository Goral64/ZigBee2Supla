// SPDX-License-Identifier: GPL-2.0-or-later
// Tests of the Linux TLS transport against a real in-process OpenSSL server,
// covering all supla_security_level modes.

#include "openssl_transport.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <string>
#include <thread>

using namespace z2s;

namespace {

// Like apps/linux/main.cpp: writes to a socket closed by the peer (e.g. TLS
// alerts after a rejected certificate) must not kill the process.
const bool kSigpipeIgnored = [] {
  signal(SIGPIPE, SIG_IGN);
  return true;
}();

struct Cert {
  EVP_PKEY *key = nullptr;
  X509 *cert = nullptr;
  ~Cert() {
    X509_free(cert);
    EVP_PKEY_free(key);
  }
};

// Self-signed certificate valid from now+notBeforeDays to now+notAfterDays.
std::unique_ptr<Cert> makeCert(const char *cn, const char *sanDns,
                               long notBeforeDays, long notAfterDays) {
  auto c = std::make_unique<Cert>();
  c->key = EVP_EC_gen("P-256");
  c->cert = X509_new();
  X509_set_version(c->cert, 2);
  ASN1_INTEGER_set(X509_get_serialNumber(c->cert), 1);
  X509_gmtime_adj(X509_getm_notBefore(c->cert), notBeforeDays * 86400);
  X509_gmtime_adj(X509_getm_notAfter(c->cert), notAfterDays * 86400);
  X509_set_pubkey(c->cert, c->key);
  X509_NAME *name = X509_get_subject_name(c->cert);
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<const unsigned char *>(cn), -1,
                             -1, 0);
  X509_set_issuer_name(c->cert, name);
  if (sanDns != nullptr) {
    std::string san = std::string("DNS:") + sanDns;
    X509_EXTENSION *ext = X509V3_EXT_conf_nid(
        nullptr, nullptr, NID_subject_alt_name, san.c_str());
    X509_add_ext(c->cert, ext, -1);
    X509_EXTENSION_free(ext);
  }
  X509_sign(c->cert, c->key, EVP_sha256());
  return c;
}

class TempPem {
 public:
  explicit TempPem(X509 *cert) {
    char tmpl[] = "/tmp/z2s_cert_XXXXXX";
    int fd = mkstemp(tmpl);
    path_ = tmpl;
    FILE *f = fdopen(fd, "w");
    PEM_write_X509(f, cert);
    fclose(f);
  }
  ~TempPem() { unlink(path_.c_str()); }
  const std::string &path() const { return path_; }

 private:
  std::string path_;
};

// Accepts one TLS connection, answers "PONG" to "PING".
class TlsServer {
 public:
  explicit TlsServer(const Cert &cert) {
    ctx_ = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate(ctx_, cert.cert);
    SSL_CTX_use_PrivateKey(ctx_, cert.key);

    // Dual-stack socket, so "localhost" works for both ::1 and 127.0.0.1;
    // IPv4 only where IPv6 is not available.
    if (!listenIpv6() && !listenIpv4()) {
      ADD_FAILURE() << "cannot create listening socket";
    }

    thread_ = std::thread([this] { run(); });
  }

  ~TlsServer() {
    shutdown(listenFd_, SHUT_RDWR);
    close(listenFd_);
    thread_.join();
    SSL_CTX_free(ctx_);
  }

  bool listenIpv6() {
    listenFd_ = socket(AF_INET6, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;
    int zero = 0, one = 1;
    setsockopt(listenFd_, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in6 addr{};
    addr.sin6_family = AF_INET6;
    addr.sin6_addr = in6addr_any;
    socklen_t len = sizeof(addr);
    if (bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), len) != 0 ||
        listen(listenFd_, 1) != 0 ||
        getsockname(listenFd_, reinterpret_cast<sockaddr *>(&addr), &len) !=
            0) {
      close(listenFd_);
      listenFd_ = -1;
      return false;
    }
    port_ = ntohs(addr.sin6_port);
    return true;
  }

  bool listenIpv4() {
    listenFd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return false;
    int one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t len = sizeof(addr);
    if (bind(listenFd_, reinterpret_cast<sockaddr *>(&addr), len) != 0 ||
        listen(listenFd_, 1) != 0 ||
        getsockname(listenFd_, reinterpret_cast<sockaddr *>(&addr), &len) !=
            0) {
      close(listenFd_);
      listenFd_ = -1;
      return false;
    }
    port_ = ntohs(addr.sin_port);
    return true;
  }

  int port() const { return port_; }
  bool handshakeOk() const { return handshakeOk_; }

 private:
  void run() {
    int fd = accept(listenFd_, nullptr, nullptr);
    if (fd < 0) return;
    timeval tv{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    SSL *ssl = SSL_new(ctx_);
    SSL_set_fd(ssl, fd);
    if (SSL_accept(ssl) == 1) {
      handshakeOk_ = true;
      char buf[16];
      int n = SSL_read(ssl, buf, sizeof(buf));
      if (n == 4 && std::string(buf, 4) == "PING") {
        SSL_write(ssl, "PONG", 4);
      }
      SSL_read(ssl, buf, sizeof(buf));  // wait for client close
    }
    SSL_free(ssl);
    close(fd);
  }

  SSL_CTX *ctx_ = nullptr;
  int listenFd_ = -1;
  int port_ = 0;
  std::thread thread_;
  std::atomic<bool> handshakeOk_{false};
};

TransportStatus connectAndWait(Transport *t, const std::string &host,
                               int port) {
  if (!t->connect(host, port)) return TransportStatus::Failed;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    TransportStatus s = t->status();
    if (s != TransportStatus::Connecting) return s;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return TransportStatus::Connecting;
}

bool pingPong(Transport *t) {
  if (!t->write("PING", 4) || !t->flush()) return false;
  char buf[8];
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    int n = t->read(buf, sizeof(buf));
    if (n == 4) return std::string(buf, 4) == "PONG";
    if (n == 0) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return false;
}

TransportStatus tryConnect(const TlsConfig &config, const Cert &serverCert,
                           const std::string &host = "localhost",
                           bool *pong = nullptr) {
  TlsServer server(serverCert);
  OpenSslTransportFactory factory(config);
  EXPECT_TRUE(factory.init(host));
  auto transport = factory.create();
  TransportStatus status = connectAndWait(transport.get(), host, server.port());
  if (pong != nullptr) {
    *pong = status == TransportStatus::Connected && pingPong(transport.get());
  }
  transport->close();
  return status;
}

}  // namespace

TEST(OpenSslTransportTest, Level2AcceptsAnyCertificate) {
  auto cert = makeCert("SUPLA", nullptr, 0, 365);
  TlsConfig config;
  config.securityLevel = 2;
  bool pong = false;
  EXPECT_EQ(tryConnect(config, *cert, "localhost", &pong),
            TransportStatus::Connected);
  EXPECT_TRUE(pong);
}

TEST(OpenSslTransportTest, Level0RejectsSelfSignedCertificate) {
  auto cert = makeCert("localhost", "localhost", 0, 365);
  TlsConfig config;
  config.securityLevel = 0;
  EXPECT_EQ(tryConnect(config, *cert), TransportStatus::Failed);
}

// supla-docker: self-signed, CN=SUPLA, valid 365 days. Pinning must work
// regardless of the host name and after the certificate expired.
TEST(OpenSslTransportTest, Level3AcceptsPinnedCertificateEvenIfExpired) {
  auto cert = makeCert("SUPLA", nullptr, -400, -35);
  TempPem pem(cert->cert);
  TlsConfig config;
  config.securityLevel = 3;
  config.caFile = pem.path();
  bool pong = false;
  EXPECT_EQ(tryConnect(config, *cert, "localhost", &pong),
            TransportStatus::Connected);
  EXPECT_TRUE(pong);
}

TEST(OpenSslTransportTest, Level3RejectsDifferentCertificate) {
  auto pinned = makeCert("SUPLA", nullptr, 0, 365);
  auto other = makeCert("SUPLA", nullptr, 0, 365);
  TempPem pem(pinned->cert);
  TlsConfig config;
  config.securityLevel = 3;
  config.caFile = pem.path();
  EXPECT_EQ(tryConnect(config, *other), TransportStatus::Failed);
}

TEST(OpenSslTransportTest, Level3RequiresReadableCertificate) {
  TlsConfig config;
  config.securityLevel = 3;
  config.caFile = "/nonexistent/cert.crt";
  OpenSslTransportFactory factory(config);
  EXPECT_FALSE(factory.init("localhost"));
}

TEST(OpenSslTransportTest, Level1ChecksHostName) {
  auto supla = makeCert("SUPLA", nullptr, 0, 365);
  TempPem suplaPem(supla->cert);
  TlsConfig config;
  config.securityLevel = 1;
  config.caFile = suplaPem.path();
  EXPECT_EQ(tryConnect(config, *supla), TransportStatus::Failed);

  auto local = makeCert("localhost", "localhost", 0, 365);
  TempPem localPem(local->cert);
  config.caFile = localPem.path();
  EXPECT_EQ(tryConnect(config, *local), TransportStatus::Connected);
}
