// SPDX-License-Identifier: GPL-2.0-or-later

#include "openssl_transport.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstring>
#include <vector>

#include "z2s/log.h"
#include "z2s/supla_ca_cert.h"

namespace z2s {

namespace {

constexpr uint64_t kDnsCacheMs = 5 * 60 * 1000;

uint64_t monotonicMs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

bool endsWith(const std::string &s, const std::string &suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Called by OpenSSL whenever the server issues a session (with TLS 1.3 this
// happens after the handshake). Remembering it lets the remaining 100+
// connections resume the session instead of a full handshake.
int onNewSession(SSL *ssl, SSL_SESSION *session) {
  auto *factory = static_cast<OpenSslTransportFactory *>(
      SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
  if (factory != nullptr) {
    factory->storeSession(session);
  }
  return 0;  // we took our own reference in storeSession()
}

// Accepts the peer only when its certificate is identical to the pinned one
// (security level 3). Every other verification error is ignored on purpose.
int verifyPinned(int, X509_STORE_CTX *storeCtx) {
  if (X509_STORE_CTX_get_error_depth(storeCtx) > 0) {
    return 1;  // the decision is made on the peer (depth 0) certificate
  }
  auto *ssl = static_cast<SSL *>(X509_STORE_CTX_get_ex_data(
      storeCtx, SSL_get_ex_data_X509_STORE_CTX_idx()));
  auto *factory = static_cast<OpenSslTransportFactory *>(
      SSL_CTX_get_app_data(SSL_get_SSL_CTX(ssl)));
  X509 *peer = X509_STORE_CTX_get_current_cert(storeCtx);
  if (factory == nullptr || factory->pinnedCert() == nullptr ||
      peer == nullptr || X509_cmp(peer, factory->pinnedCert()) != 0) {
    X509_STORE_CTX_set_error(storeCtx, X509_V_ERR_CERT_REJECTED);
    return 0;
  }
  X509_STORE_CTX_set_error(storeCtx, X509_V_OK);
  return 1;
}

bool addPemToStore(X509_STORE *store, const char *pem) {
  BIO *bio = BIO_new_mem_buf(pem, -1);
  if (bio == nullptr) return false;
  int added = 0;
  while (X509 *cert = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr)) {
    if (X509_STORE_add_cert(store, cert) == 1) added++;
    X509_free(cert);
  }
  ERR_clear_error();
  BIO_free(bio);
  return added > 0;
}

class OpenSslTransport : public Transport {
 public:
  explicit OpenSslTransport(OpenSslTransportFactory *factory)
      : factory_(factory) {}
  ~OpenSslTransport() override { close(); }

  bool connect(const std::string &host, int port) override {
    close();
    host_ = host;
    sockaddr_storage addr;
    socklen_t addrLen = 0;
    if (!factory_->resolve(host, port, &addr, &addrLen)) {
      state_ = TransportStatus::Failed;
      return false;
    }
    fd_ =
        ::socket(addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0) {
      Z2S_LOG_ERROR("socket() failed: %s", strerror(errno));
      state_ = TransportStatus::Failed;
      return false;
    }
    int one = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd_, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    int r = ::connect(fd_, reinterpret_cast<sockaddr *>(&addr), addrLen);
    if (r != 0 && errno != EINPROGRESS) {
      Z2S_LOG_WARNING("connect() to %s failed: %s", host.c_str(),
                      strerror(errno));
      close();
      state_ = TransportStatus::Failed;
      return false;
    }
    phase_ = Phase::TcpConnecting;
    state_ = TransportStatus::Connecting;
    return true;
  }

  TransportStatus status() override {
    if (state_ != TransportStatus::Connecting) {
      return state_;
    }
    if (phase_ == Phase::TcpConnecting) {
      pollfd pfd{fd_, POLLOUT, 0};
      int r = ::poll(&pfd, 1, 0);
      if (r == 0) return state_;
      int err = 0;
      socklen_t len = sizeof(err);
      if (r < 0 || getsockopt(fd_, SOL_SOCKET, SO_ERROR, &err, &len) != 0 ||
          err != 0) {
        Z2S_LOG_WARNING("TCP connection to %s failed: %s", host_.c_str(),
                        strerror(err ? err : errno));
        fail();
        return state_;
      }
      if (factory_->config().disableTls) {
        phase_ = Phase::Ready;
        state_ = TransportStatus::Connected;
        return state_;
      }
      if (!startTls()) {
        fail();
        return state_;
      }
      phase_ = Phase::TlsHandshake;
    }
    if (phase_ == Phase::TlsHandshake) {
      int r = SSL_connect(ssl_);
      if (r == 1) {
        phase_ = Phase::Ready;
        state_ = TransportStatus::Connected;
        return state_;
      }
      int err = SSL_get_error(ssl_, r);
      if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return state_;
      }
      long verify = SSL_get_verify_result(ssl_);
      if (verify != X509_V_OK) {
        Z2S_LOG_ERROR("TLS certificate verification failed for %s: %s",
                      host_.c_str(), X509_verify_cert_error_string(verify));
      } else {
        char buf[256];
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        Z2S_LOG_WARNING("TLS handshake with %s failed: %s", host_.c_str(), buf);
      }
      ERR_clear_error();
      fail();
    }
    return state_;
  }

  int read(void *buf, int len) override {
    if (state_ != TransportStatus::Connected) return 0;
    if (ssl_ == nullptr) {
      ssize_t r = ::recv(fd_, buf, len, 0);
      if (r > 0) return static_cast<int>(r);
      if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return -1;
      fail();
      return 0;
    }
    int r = SSL_read(ssl_, buf, len);
    if (r > 0) return r;
    int err = SSL_get_error(ssl_, r);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
      return -1;
    }
    ERR_clear_error();
    fail();
    return 0;
  }

  bool write(const void *buf, int len) override {
    if (state_ != TransportStatus::Connected) return false;
    const char *p = static_cast<const char *>(buf);
    outBuffer_.insert(outBuffer_.end(), p, p + len);
    return true;
  }

  bool flush() override {
    if (state_ != TransportStatus::Connected) {
      return state_ == TransportStatus::Connecting;
    }
    while (!outBuffer_.empty()) {
      int r;
      if (ssl_ == nullptr) {
        ssize_t s =
            ::send(fd_, outBuffer_.data(), outBuffer_.size(), MSG_NOSIGNAL);
        if (s < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (s <= 0) {
          fail();
          return false;
        }
        r = static_cast<int>(s);
      } else {
        r = SSL_write(ssl_, outBuffer_.data(),
                      static_cast<int>(outBuffer_.size()));
        if (r <= 0) {
          int err = SSL_get_error(ssl_, r);
          if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
            return true;
          }
          ERR_clear_error();
          fail();
          return false;
        }
      }
      outBuffer_.erase(outBuffer_.begin(), outBuffer_.begin() + r);
    }
    return true;
  }

  void close() override {
    if (ssl_ != nullptr) {
      if (state_ == TransportStatus::Connected) {
        SSL_shutdown(ssl_);
      }
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    outBuffer_.clear();
    state_ = TransportStatus::Idle;
    phase_ = Phase::None;
  }

  int fd() const override { return fd_; }

  bool hasPendingWrite() const override {
    return !outBuffer_.empty() || phase_ == Phase::TcpConnecting;
  }

 private:
  enum class Phase { None, TcpConnecting, TlsHandshake, Ready };

  bool startTls() {
    ssl_ = SSL_new(factory_->ctx());
    if (ssl_ == nullptr) {
      Z2S_LOG_ERROR("SSL_new() failed");
      return false;
    }
    SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE |
                           SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
    SSL_set_fd(ssl_, fd_);
    SSL_set_tlsext_host_name(ssl_, host_.c_str());
    int level = factory_->config().securityLevel;
    if (level == 0 || level == 1) {
      SSL_set1_host(ssl_, host_.c_str());
    }
    if (SSL_SESSION *session = factory_->session()) {
      SSL_set_session(ssl_, session);
    }
    return true;
  }

  void fail() {
    if (ssl_ != nullptr) {
      SSL_free(ssl_);
      ssl_ = nullptr;
    }
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
    outBuffer_.clear();
    state_ = TransportStatus::Failed;
    phase_ = Phase::None;
  }

  OpenSslTransportFactory *factory_;
  std::string host_;
  int fd_ = -1;
  SSL *ssl_ = nullptr;
  Phase phase_ = Phase::None;
  TransportStatus state_ = TransportStatus::Idle;
  std::vector<char> outBuffer_;
};

}  // namespace

OpenSslTransportFactory::OpenSslTransportFactory(const TlsConfig &config)
    : config_(config) {}

OpenSslTransportFactory::~OpenSslTransportFactory() {
  if (lastSession_ != nullptr) SSL_SESSION_free(lastSession_);
  if (pinnedCert_ != nullptr) X509_free(pinnedCert_);
  if (ctx_ != nullptr) SSL_CTX_free(ctx_);
}

bool OpenSslTransportFactory::init(const std::string &serverHost) {
  if (config_.disableTls) {
    Z2S_LOG_WARNING("TLS disabled - use only with a local test server");
    return true;
  }
  ctx_ = SSL_CTX_new(TLS_client_method());
  if (ctx_ == nullptr) {
    Z2S_LOG_ERROR("SSL_CTX_new() failed");
    return false;
  }
  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
  SSL_CTX_set_session_cache_mode(
      ctx_, SSL_SESS_CACHE_CLIENT | SSL_SESS_CACHE_NO_INTERNAL_STORE);
  SSL_CTX_set_app_data(ctx_, this);
  SSL_CTX_sess_set_new_cb(ctx_, &onNewSession);

  switch (config_.securityLevel) {
    case 0: {
      bool publicServer = endsWith(serverHost, ".supla.org");
      const char *pem = publicServer ? kSuplaCACert : kSupla3rdPartyCACert;
      if (!addPemToStore(SSL_CTX_get_cert_store(ctx_), pem)) {
        Z2S_LOG_ERROR("Cannot load Supla CA certificate");
        return false;
      }
      SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
      Z2S_LOG_INFO("TLS: verifying server with Supla %s CA",
                   publicServer ? "public cloud" : "private cloud");
      break;
    }
    case 1:
      if (SSL_CTX_load_verify_locations(ctx_, config_.caFile.c_str(),
                                        nullptr) != 1) {
        Z2S_LOG_ERROR("Cannot load CA file %s", config_.caFile.c_str());
        return false;
      }
      SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
      Z2S_LOG_INFO("TLS: verifying server with CA from %s",
                   config_.caFile.c_str());
      break;
    case 3: {
      FILE *f = fopen(config_.caFile.c_str(), "r");
      if (f != nullptr) {
        pinnedCert_ = PEM_read_X509(f, nullptr, nullptr, nullptr);
        fclose(f);
      }
      if (pinnedCert_ == nullptr) {
        Z2S_LOG_ERROR("Cannot load pinned certificate %s",
                      config_.caFile.c_str());
        return false;
      }
      SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, &verifyPinned);
      Z2S_LOG_INFO("TLS: server certificate pinned to %s",
                   config_.caFile.c_str());
      break;
    }
    case 2:
      SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
      Z2S_LOG_WARNING("TLS: server certificate is NOT verified");
      break;
    default:
      Z2S_LOG_ERROR("Invalid security level %d", config_.securityLevel);
      return false;
  }
  return true;
}

std::unique_ptr<Transport> OpenSslTransportFactory::create() {
  return std::make_unique<OpenSslTransport>(this);
}

bool OpenSslTransportFactory::resolve(const std::string &host, int port,
                                      sockaddr_storage *addr,
                                      socklen_t *addrLen) {
  uint64_t now = monotonicMs();
  if (cachedAddrLen_ > 0 && cachedHost_ == host && cachedPort_ == port &&
      now - cachedAtMs_ < kDnsCacheMs) {
    *addr = cachedAddr_;
    *addrLen = cachedAddrLen_;
    return true;
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo *result = nullptr;
  std::string portStr = std::to_string(port);
  int r = getaddrinfo(host.c_str(), portStr.c_str(), &hints, &result);
  if (r != 0 || result == nullptr) {
    Z2S_LOG_WARNING("Cannot resolve %s: %s", host.c_str(), gai_strerror(r));
    // Keep using a stale address rather than failing all devices.
    if (cachedAddrLen_ > 0 && cachedHost_ == host && cachedPort_ == port) {
      *addr = cachedAddr_;
      *addrLen = cachedAddrLen_;
      return true;
    }
    return false;
  }
  memcpy(&cachedAddr_, result->ai_addr, result->ai_addrlen);
  cachedAddrLen_ = result->ai_addrlen;
  cachedHost_ = host;
  cachedPort_ = port;
  cachedAtMs_ = now;
  freeaddrinfo(result);
  *addr = cachedAddr_;
  *addrLen = cachedAddrLen_;
  return true;
}

void OpenSslTransportFactory::storeSession(SSL_SESSION *session) {
  if (session == lastSession_) return;
  if (!SSL_SESSION_is_resumable(session)) return;
  SSL_SESSION_up_ref(session);
  if (lastSession_ != nullptr) SSL_SESSION_free(lastSession_);
  lastSession_ = session;
}

}  // namespace z2s
