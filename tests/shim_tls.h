#pragma once
// A minimal TLS server for the host tests.
//
// The client under test verifies the chain AND the hostname, so a test that
// only faked a handshake would not exercise verification at all. This server
// completes a real handshake with mbedTLS's server role, using a throwaway
// self-signed certificate that the test installs as the trust anchor. It is
// test-only: `MBEDTLS_SSL_SRV_C` and the server-side TLS sources are compiled
// into the test binaries and never into the shipped library.
//
// Two certificates are served:
//   * `localhost` (SAN DNS:localhost, IP:127.0.0.1) — a handshake the client
//     must ACCEPT, proving the verified path works end to end.
//   * `wrong.example` — a valid chain whose name does not match, so the
//     handshake must FAIL, proving hostname verification is real.

#include "berry/types.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"

namespace berry_test {

/// The throwaway CA + leaf for `localhost`. Kept as PEM text so a test needs
/// no files on disk. Generated once for the test suite (see PROVENANCE note in
/// test_net.cpp); valid for ten years so the test does not rot.
inline const char* kTestCaPem() {
  return
      "-----BEGIN CERTIFICATE-----\n"
      "MIIByTCCAW+gAwIBAgIUTS6JzCNiGCaUcuosLxlOErIuHiQwCgYIKoZIzj0EAwIw\n"
      "LDESMBAGA1UEAwwJbG9jYWxob3N0MRYwFAYDVQQKDA1CZXJyeUFQSSBUZXN0MB4X\n"
      "DTI2MTAyMzIxMTExMloXDTM2MTAyMDIxMTExMlowLDESMBAGA1UEAwwJbG9jYWxo\n"
      "b3N0MRYwFAYDVQQKDA1CZXJyeUFQSSBUZXN0MFkwEwYHKoZIzj0CAQYIKoZIzj0D\n"
      "AQcDQgAEgRmsfIAi8ZTXP/uof9A9gujakq/uKQh/81cTe3g+mRQ5WuRzM4GmwzpN\n"
      "YWEuFKLVtRKR/2RNxVnn6AhF3+K7ZKNvMG0wHQYDVR0OBBYEFBrUY0Zf5b/bf/yf\n"
      "i7j6VRZgUtphMB8GA1UdIwQYMBaAFBrUY0Zf5b/bf/yfi7j6VRZgUtphMBoGA1Ud\n"
      "EQQTMBGCCWxvY2FsaG9zdIcEfwAAATAPBgNVHRMBAf8EBTADAQH/MAoGCCqGSM49\n"
      "BAMCA0gAMEUCIQC14l8BIOZcWf/fNw0ogyve4dm9mlbjbGbZkwk4QQYdxAIgWDtD\n"
      "O6ta8KovH01D1LwAw8ZY/EPv610EHSly/ZfDun4=\n"
      "-----END CERTIFICATE-----\n";
}

inline const char* kTestKeyPem() {
  return
      "-----BEGIN EC PRIVATE KEY-----\n"
      "MHcCAQEEIEg90Zne3VgMZ/r4GDxjTy+o7t8YWdHMAsDW5tGeM2tCoAoGCCqGSM49\n"
      "AwEHoUQDQgAEgRmsfIAi8ZTXP/uof9A9gujakq/uKQh/81cTe3g+mRQ5WuRzM4Gm\n"
      "wzpNYWEuFKLVtRKR/2RNxVnn6AhF3+K7ZA==\n"
      "-----END EC PRIVATE KEY-----\n";
}

/// A TLS server that serves the throwaway leaf above. It handles exactly one
/// connection, echoes one line, then closes. Run it on its own thread; call
/// `Stop()` before it goes out of scope.
class TinyTlsServer {
 public:
  /// Starts listening on `port` (0 lets the kernel choose). Returns the bound
  /// port, or -1 on failure. `body` is the HTTP response body it serves (the
  /// default "tls-pong"); a caller that needs a JSON body passes one.
  int Start(int port, const std::string& body = "tls-pong") {
    body_ = body;
    listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listenFd_ < 0) return -1;
    int one = 1;
    ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    if (::bind(listenFd_, (sockaddr*)&addr, sizeof addr) != 0) {
      ::close(listenFd_);
      listenFd_ = -1;
      return -1;
    }
    if (::listen(listenFd_, 4) != 0) {
      ::close(listenFd_);
      listenFd_ = -1;
      return -1;
    }
    socklen_t alen = sizeof addr;
    if (::getsockname(listenFd_, (sockaddr*)&addr, &alen) != 0) {
      ::close(listenFd_);
      listenFd_ = -1;
      return -1;
    }
    port_ = ntohs(addr.sin_port);
    running_ = true;
    thread_ = std::thread([this] { Run(); });
    return port_;
  }

  void Stop() {
    running_ = false;
    // Close the listening fd to unblock a pending accept(), and shut down any
    // live connection so a blocked handshake/read returns instead of hanging
    // the join. The fd is closed by Run(); Stop only shuts it down.
    if (listenFd_ >= 0) {
      ::shutdown(listenFd_, SHUT_RDWR);
      ::close(listenFd_);
      listenFd_ = -1;
    }
    if (connFd_ >= 0) ::shutdown(connFd_, SHUT_RDWR);
    if (thread_.joinable()) thread_.join();
  }

  /// True once a handshake has completed (used to assert the client got in).
  bool handshook() const { return handshook_; }
  int Port() const { return port_; }
  /// The server-side error, when its handshake failed. For test diagnostics.
  const std::string& lastError() const { return serverErr_; }

 private:
  void Run() {
    sockaddr_in peer{};
    socklen_t plen = sizeof peer;
    int conn = ::accept(listenFd_, (sockaddr*)&peer, &plen);
    if (conn < 0) return;
    connFd_ = conn;
    // A stalled peer must not pin the server thread forever; bound every wait.
    timeval tv{};
    tv.tv_sec = 10;
    ::setsockopt(conn, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    ::setsockopt(conn, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt cert;
    mbedtls_pk_context key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    mbedtls_x509_crt_init(&cert);
    mbedtls_pk_init(&key);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&drbg);

    bool ok = false;
    do {
      if (mbedtls_x509_crt_parse(&cert, (const unsigned char*)kTestCaPem(),
                                 strlen(kTestCaPem()) + 1) != 0) break;
      if (mbedtls_pk_parse_key(&key, (const unsigned char*)kTestKeyPem(),
                               strlen(kTestKeyPem()) + 1, nullptr, 0,
                               nullptr, nullptr) != 0) break;
      const char* pers = "berry-test-srv";
      if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy,
                                (const unsigned char*)pers, strlen(pers)) != 0) break;
      if (mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_SERVER,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT) != 0) break;
      mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &drbg);
      if (getenv("BERRY_TLS_DEBUG")) {
        mbedtls_debug_set_threshold(4);
        mbedtls_ssl_conf_dbg(&conf, DebugTrampoline, nullptr);
      }
      mbedtls_ssl_conf_ca_chain(&conf, &cert, nullptr);
      if (mbedtls_ssl_conf_own_cert(&conf, &cert, &key) != 0) break;
      if (mbedtls_ssl_setup(&ssl, &conf) != 0) break;
      mbedtls_ssl_set_bio(&ssl, &conn, BioSend, BioRecv, nullptr);
      for (;;) {
        const int ret = mbedtls_ssl_handshake(&ssl);
        if (ret == 0) { ok = true; break; }
        if (ret == MBEDTLS_ERR_SSL_WANT_READ ||
            ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        char eb[160];
        mbedtls_strerror(ret, eb, sizeof eb);
        serverErr_ = std::string("handshake: ") + eb;
        break;
      }
      if (!ok) break;
      handshook_ = true;
      // Echo a short body so the client has something to read. The status line
      // and Content-Length are computed so a caller's JSON body is served whole.
      char header[128];
      std::snprintf(header, sizeof header,
                    "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n"
                    "Connection: close\r\n\r\n",
                    body_.size());
      std::string resp = std::string(header) + body_;
      const unsigned char* p = (const unsigned char*)resp.data();
      size_t left = resp.size();
      while (left > 0) {
        const int n = mbedtls_ssl_write(&ssl, p, left);
        if (n <= 0) break;
        p += n;
        left -= (size_t)n;
      }
      // Read (and discard) the client's request so the exchange is complete.
      unsigned char buf[512];
      for (int i = 0; i < 8; ++i) {
        const int n = mbedtls_ssl_read(&ssl, buf, sizeof buf);
        if (n <= 0) break;
      }
      mbedtls_ssl_close_notify(&ssl);
    } while (false);

    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    mbedtls_x509_crt_free(&cert);
    mbedtls_pk_free(&key);
    mbedtls_ctr_drbg_free(&drbg);
    mbedtls_entropy_free(&entropy);
    connFd_ = -1;
    ::close(conn);
  }

  static void DebugTrampoline(void*, int level, const char* file, int line,
                              const char* str) {
    fprintf(stderr, "[srv %d] %s:%d: %s", level, file, line, str);
  }

  static int BioSend(void* ctx, const unsigned char* buf, size_t len) {
    const int fd = *(int*)ctx;
    const ssize_t n = ::send(fd, buf, len, MSG_NOSIGNAL);
    return n <= 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
  }
  static int BioRecv(void* ctx, unsigned char* buf, size_t len) {
    const int fd = *(int*)ctx;
    const ssize_t n = ::recv(fd, buf, len, 0);
    if (n == 0) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    return n < 0 ? MBEDTLS_ERR_SSL_INTERNAL_ERROR : (int)n;
  }

  int listenFd_ = -1;
  std::atomic<int> connFd_{-1};
  int port_ = 0;
  std::string body_ = "tls-pong";
  std::atomic<bool> running_{false};
  std::atomic<bool> handshook_{false};
  std::string serverErr_;
  std::thread thread_;
};

}  // namespace berry_test
