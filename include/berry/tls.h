#pragma once
// TLS client socket (HTTPS/WSS).
//
// `Berry::TcpSocket` is plaintext. Everything real — presence, the package
// registry, pack downloads, cosmetic sync, JSON-RPC — is HTTPS, so a client
// that can only speak plaintext can only reach the loopback harness. This is
// the missing half: a TLS socket that verifies the server's certificate chain
// AND hostname against the system trust store before a single byte is sent, and
// fails closed on anything it cannot verify.
//
// Design:
//
//   * The transport is the existing `TcpSocket` (POSIX sockets, a deadline).
//     mbedTLS is driven through BIO callbacks over that fd, so mbedTLS never
//     opens a socket of its own and every wait still obeys our deadline.
//   * Verification is `MBEDTLS_SSL_VERIFY_REQUIRED` with the hostname set from
//     the URL, so an untrusted chain, an expired certificate, or a certificate
//     issued for another name all fail the handshake. There is no
//     "insecure"/"skip verify" mode: a downgrade would be the worst failure.
//   * The trust store is the system CA bundle, loaded once. The path is
//     overridable so a device build can point at its own store and a test can
//     point at a throwaway CA; when the store cannot be loaded, `Connect`
//     refuses rather than falling back to "trust anything".
//   * The whole operation (TCP connect + handshake + every read) is bounded by
//     one absolute `Deadline`, exactly like the plaintext path.
//
// mbedTLS is a vendored C dependency (see third_party/mbedtls/PROVENANCE.md).
// Its headers are kept out of this one behind a pimpl, so a caller of this
// header does not need mbedTLS's include path.

#include "berry/export.h"
#include "berry/net.h"
#include "berry/types.h"

#include <memory>
#include <string>

namespace Berry {

/// A TLS client connection. Move-free, single-threaded like `TcpSocket`.
class BERRY_API TlsSocket {
 public:
  TlsSocket();
  ~TlsSocket();

  TlsSocket(const TlsSocket&) = delete;
  TlsSocket& operator=(const TlsSocket&) = delete;

  void Close();
  bool valid() const;

  /// Opens a TCP connection to `host:port` and performs a verified TLS
  /// handshake. `host` is used for SNI and for certificate hostname
  /// verification. Returns false (leaving the socket closed and `LastError`
  /// set) on any failure: connect, the trust store not loading, a failed
  /// chain/hostname/expiry check, or the deadline passing.
  bool Connect(const char* host, u16 port, const Deadline& deadline);

  /// Sends the whole buffer, bounded by `deadline`. Returns the byte count, or
  /// -1 on error / -2 when the deadline passed.
  ssize_t Send(const void* data, size_t len, const Deadline& deadline);
  /// Reads up to `len` bytes, bounded by `deadline`. Returns >0 bytes, 0 on a
  /// clean peer close, -1 on error, -2 when the deadline passed.
  ssize_t Recv(void* buf, size_t len, const Deadline& deadline);

  /// True when a read would not block: either mbedTLS already holds buffered
  /// plaintext, or the underlying socket has bytes waiting within `timeoutMs`.
  /// Lets a poll loop wait on the transport without guessing.
  bool Readable(int timeoutMs);

  /// The negotiated protocol ("TLSv1.3") or empty before a handshake.
  std::string ProtocolVersion() const;
  /// The negotiated cipher suite, or empty before a handshake.
  std::string CipherSuite() const;

  const std::string& LastError() const;

  /// True when TLS is compiled into this build. A build without the vendored
  /// library reports false and `Connect` fails closed.
  static bool Available();

  /// The CA bundle path used for verification. Defaults to the system store
  /// (`/etc/ssl/certs/ca-certificates.crt` on the host). Setting it drops the
  /// cached store so the next connect reloads.
  static void SetCaBundlePath(std::string path);
  static std::string CaBundlePath();

  /// True when the trust store loaded successfully. `Connect` fails closed when
  /// it has not.
  static bool TrustStoreReady();

  /// Forces the cached trust store to be dropped and reloaded on next use.
  /// A host test calls this after pointing `SetCaBundlePath` at a throwaway CA.
  static void ReloadTrustStore();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace Berry
