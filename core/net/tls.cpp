// TLS client socket. See include/berry/tls.h.
//
// The mbedTLS types live only in this translation unit (the header is a
// pimpl), so no caller needs mbedTLS's include path. The library is driven
// through BIO callbacks over a plain `TcpSocket`, which is what lets every
// wait still obey our absolute deadline.
#include "berry/tls.h"

#include <mutex>
#include <cstring>
#include <cstdio>

#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"

namespace Berry {

namespace {

// The trust store is process-wide and loaded once: parsing a CA bundle is
// expensive, and every connection verifies against the same anchors. Guarded
// by a mutex; a failed load is remembered as a failure (never as "trust all").
std::mutex g_caMutex;
mbedtls_x509_crt g_caChain;
bool g_caLoaded = false;
bool g_caOk = false;

/// The default system store, if it exists. Kept as a small list so a device
/// build can drop in its own path without touching this code.
const char* DefaultCaBundle() {
  static const char* kCandidates[] = {
      "/etc/ssl/certs/ca-certificates.crt",   // Debian/Ubuntu/Alpine
      "/etc/pki/tls/certs/ca-bundle.crt",     // RHEL/Fedora
      "/system/etc/security/cacerts",         // Android (a directory)
      "/etc/ssl/cert.pem",                    // BSD/macOS
  };
  for (const char* p : kCandidates) {
    FILE* f = ::fopen(p, "rb");
    if (f) { ::fclose(f); return p; }
  }
  return kCandidates[0];
}

std::string g_caPath = DefaultCaBundle();

/// Loads the CA chain at `path` (a PEM bundle or a directory of certs, which
/// is what Android uses) into the process-wide chain. Returns false when the
/// store cannot be read or yields no certificates. Idempotent.
bool EnsureTrustStoreLoaded() {
  std::lock_guard<std::mutex> lk(g_caMutex);
  if (g_caLoaded) return g_caOk;
  g_caLoaded = true;
  g_caOk = false;

  mbedtls_x509_crt_init(&g_caChain);
  int ret = mbedtls_x509_crt_parse_path(&g_caChain, g_caPath.c_str());
  if (ret < 0) {
    // A directory parse also handles a bundle file poorly; try it as a file.
    ret = mbedtls_x509_crt_parse_file(&g_caChain, g_caPath.c_str());
  }
  if (ret < 0) return false;   // no usable anchors: every connect will refuse
  g_caOk = true;
  return true;
}

void ResetTrustStore() {
  std::lock_guard<std::mutex> lk(g_caMutex);
  if (g_caLoaded) {
    mbedtls_x509_crt_free(&g_caChain);
    mbedtls_x509_crt_init(&g_caChain);
  }
  g_caLoaded = false;
  g_caOk = false;
}

}  // namespace

//----------------------------------------------------------------------------
// TlsSocket::Impl
//----------------------------------------------------------------------------

struct TlsSocket::Impl {
  TcpSocket tcp;
  mbedtls_ssl_context ssl;
  mbedtls_ssl_config conf;
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  bool sslInited = false;
  bool confInited = false;
  bool entropyInited = false;
  bool drbgInited = false;
  bool handshaken = false;
  Deadline deadline = Deadline::Never();
  std::string err;

  Impl() { Init(); }
  ~Impl() { FreeAll(); }

  /// Initialises the mbedTLS contexts. Safe to call on a freed context.
  void Init() {
    if (!sslInited) { mbedtls_ssl_init(&ssl); sslInited = true; }
    if (!confInited) { mbedtls_ssl_config_init(&conf); confInited = true; }
    if (!entropyInited) { mbedtls_entropy_init(&entropy); entropyInited = true; }
    if (!drbgInited) { mbedtls_ctr_drbg_init(&drbg); drbgInited = true; }
  }

  /// Tears the contexts down. After this the Impl holds nothing and must be
  /// re-`Init`ed before use.
  void FreeAll() {
    // Free in reverse order of use. Safe to call twice.
    if (sslInited) { mbedtls_ssl_free(&ssl); sslInited = false; }
    if (confInited) { mbedtls_ssl_config_free(&conf); confInited = false; }
    if (drbgInited) { mbedtls_ctr_drbg_free(&drbg); drbgInited = false; }
    if (entropyInited) { mbedtls_entropy_free(&entropy); entropyInited = false; }
    tcp.Close();
    handshaken = false;
  }

  /// Returns the Impl to a fresh, usable state. It must leave every context
  /// INITIALISED, not freed: `Connect` calls this and then seeds the DRBG, and
  /// seeding a freed context is what made every handshake fail. (The earlier
  /// version freed without re-initialising, so the first `Connect` on a fresh
  /// socket seeded an already-freed `ctr_drbg`.)
  void Reset() {
    FreeAll();
    Init();
  }

  static int BioSend(void* ctx, const unsigned char* buf, size_t len) {
    Impl* self = (Impl*)ctx;
    const ssize_t n = self->tcp.SendAll(buf, len, self->deadline);
    if (n == -2) return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (n <= 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return (int)n;
  }
  static int BioRecv(void* ctx, unsigned char* buf, size_t len) {
    Impl* self = (Impl*)ctx;
    const ssize_t n = self->tcp.Recv(buf, len, self->deadline);
    if (n == -2) return MBEDTLS_ERR_SSL_WANT_READ;
    if (n < 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return (int)n;
  }

  /// Maps an mbedTLS error to a short, non-secret message.
  void SetErrorFrom(int ret, const char* what) {
    char buf[160];
    mbedtls_strerror(ret, buf, sizeof buf);
    err = std::string(what) + ": " + buf;
  }
};

//----------------------------------------------------------------------------
// TlsSocket
//----------------------------------------------------------------------------

TlsSocket::TlsSocket() : impl_(new Impl()) {}
TlsSocket::~TlsSocket() = default;

bool TlsSocket::Available() { return true; }

void TlsSocket::SetCaBundlePath(std::string path) {
  {
    std::lock_guard<std::mutex> lk(g_caMutex);
    if (path == g_caPath) return;
    g_caPath = std::move(path);
  }
  ResetTrustStore();
}

std::string TlsSocket::CaBundlePath() {
  std::lock_guard<std::mutex> lk(g_caMutex);
  return g_caPath;
}

bool TlsSocket::TrustStoreReady() { return EnsureTrustStoreLoaded(); }

void TlsSocket::ReloadTrustStore() { ResetTrustStore(); }

void TlsSocket::Close() {
  if (!impl_) return;
  if (impl_->handshaken) mbedtls_ssl_close_notify(&impl_->ssl);
  impl_->Reset();
}

bool TlsSocket::valid() const {
  return impl_ && impl_->handshaken;
}

bool TlsSocket::Connect(const char* host, u16 port, const Deadline& deadline) {
  if (!impl_) return false;
  impl_->Reset();
  impl_->deadline = deadline;
  impl_->err.clear();
  if (!host || !*host) {
    impl_->err = "no host";
    return false;
  }
  // Fail closed on a trust store we could not load: proceeding would mean
  // trusting whatever certificate the peer presents.
  if (!EnsureTrustStoreLoaded()) {
    impl_->err = "trust store unavailable";
    return false;
  }

  if (!impl_->tcp.Connect(host, port, deadline)) {
    impl_->err = impl_->tcp.LastError();
    return false;
  }

  const char* pers = "berry-tls";
  int ret = mbedtls_ctr_drbg_seed(&impl_->drbg, mbedtls_entropy_func,
                                  &impl_->entropy,
                                  (const unsigned char*)pers, strlen(pers));
  if (ret != 0) {
    impl_->SetErrorFrom(ret, "rng seed");
    return false;
  }

  ret = mbedtls_ssl_config_defaults(&impl_->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM,
                                    MBEDTLS_SSL_PRESET_DEFAULT);
  if (ret != 0) {
    impl_->SetErrorFrom(ret, "config");
    return false;
  }
  // Require a verified chain AND a matching hostname. Never configurable: a
  // caller cannot ask this socket to skip verification.
  mbedtls_ssl_conf_authmode(&impl_->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
  mbedtls_ssl_conf_rng(&impl_->conf, mbedtls_ctr_drbg_random, &impl_->drbg);
  // TLS 1.2 is the floor; 1.3 is offered when the peer supports it.
  mbedtls_ssl_conf_min_tls_version(&impl_->conf, MBEDTLS_SSL_VERSION_TLS1_2);
  {
    std::lock_guard<std::mutex> lk(g_caMutex);
    mbedtls_ssl_conf_ca_chain(&impl_->conf, &g_caChain, nullptr);
  }

  ret = mbedtls_ssl_setup(&impl_->ssl, &impl_->conf);
  if (ret != 0) {
    impl_->SetErrorFrom(ret, "setup");
    return false;
  }
  ret = mbedtls_ssl_set_hostname(&impl_->ssl, host);
  if (ret != 0) {
    impl_->SetErrorFrom(ret, "hostname");
    return false;
  }
  mbedtls_ssl_set_bio(&impl_->ssl, impl_.get(), Impl::BioSend, Impl::BioRecv,
                      nullptr);

  for (;;) {
    if (deadline.expired()) {
      impl_->err = "handshake deadline";
      return false;
    }
    ret = mbedtls_ssl_handshake(&impl_->ssl);
    if (ret == 0) break;
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      continue;
    }
    impl_->SetErrorFrom(ret, "handshake");
    return false;
  }

  // Belt and braces: handshake() already fails on a bad chain, but the flag
  // check makes the invariant explicit and survives a future config change.
  const u32 flags = mbedtls_ssl_get_verify_result(&impl_->ssl);
  if (flags != 0) {
    impl_->err = "certificate verification failed";
    return false;
  }
  impl_->handshaken = true;
  return true;
}

ssize_t TlsSocket::Send(const void* data, size_t len, const Deadline& deadline) {
  if (!impl_ || !impl_->handshaken) return -1;
  impl_->deadline = deadline;
  const unsigned char* p = (const unsigned char*)data;
  size_t sent = 0;
  while (sent < len) {
    if (deadline.expired()) return sent > 0 ? (ssize_t)sent : -2;
    const int ret = mbedtls_ssl_write(&impl_->ssl, p + sent, len - sent);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      continue;
    }
    if (ret < 0) {
      impl_->SetErrorFrom(ret, "send");
      return sent > 0 ? (ssize_t)sent : -1;
    }
    sent += (size_t)ret;
  }
  return (ssize_t)sent;
}

ssize_t TlsSocket::Recv(void* buf, size_t len, const Deadline& deadline) {
  if (!impl_ || !impl_->handshaken) return -1;
  impl_->deadline = deadline;
  for (;;) {
    if (deadline.expired()) return -2;
    const int ret = mbedtls_ssl_read(&impl_->ssl, (unsigned char*)buf, len);
    if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
      continue;
    }
    if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) return 0;
    if (ret < 0) {
      impl_->SetErrorFrom(ret, "recv");
      return -1;
    }
    return ret;
  }
}

bool TlsSocket::Readable(int timeoutMs) {
  if (!impl_ || !impl_->handshaken) return false;
  // mbedTLS keeps decrypted-but-unread bytes in its own buffer after a record
  // is consumed; `mbedtls_ssl_get_bytes_avail` reports them, so a poll does not
  // miss a frame that is already inside the library.
  if (mbedtls_ssl_get_bytes_avail(&impl_->ssl) > 0) return true;
  return impl_->tcp.WaitReadable(timeoutMs);
}

std::string TlsSocket::ProtocolVersion() const {
  if (!impl_ || !impl_->handshaken) return std::string();
  const char* v = mbedtls_ssl_get_version(&impl_->ssl);
  return v ? std::string(v) : std::string();
}

std::string TlsSocket::CipherSuite() const {
  if (!impl_ || !impl_->handshaken) return std::string();
  const char* c = mbedtls_ssl_get_ciphersuite(&impl_->ssl);
  return c ? std::string(c) : std::string();
}

const std::string& TlsSocket::LastError() const {
  static const std::string kEmpty;
  return impl_ ? impl_->err : kEmpty;
}

}  // namespace Berry
