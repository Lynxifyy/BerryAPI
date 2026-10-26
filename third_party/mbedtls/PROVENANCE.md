# mbedTLS (vendored)

Source: https://github.com/Mbed-TLS/mbedtls
Release: v3.6.7 (tarball `v3.6.7.tar.bz2`, sha256
`a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6`)
License: Apache-2.0 OR GPL-2.0-or-later (see LICENSE.txt in the upstream
release; the Apache-2.0 option is the one used here).

3.6 is the long-term-support line (supported through at least March 2027).
The pinned release is the latest 3.6 patch, chosen over the older v3.6.2 this
tree started on because 3.6.3 fixes a **TLS handshake authentication bypass**
(`mbedtls-security-advisory-2025-03-2`) — the exact class of failure a verified
TLS client exists to stop — and 3.6.4–3.6.7 fix further advisories (client
impersonation while resuming a TLS 1.3 session, PSA RNG cloning, compiler-
induced constant-time violations, RSA PKCS#1 v1.5 decryption timing). See
`docs/BUILD.md` for the `make check-mbedtls` guard that flags a stale pin.

Only the client-side TLS 1.2/1.3 stack needed by `Berry::TlsSocket` is
vendored:

- `include/mbedtls`, `include/psa` — the public headers, unmodified.
- `library/*.c` and `library/*.h` — the library sources, minus the units that
  compile to an empty object under the trimmed configuration (they are
  `#if`-ed out entirely): `camellia memory_buffer_alloc mps_trace net_sockets
  ripemd160 ssl_cookie ssl_ticket x509_create x509_crl x509_csr x509write
  x509write_crt x509write_csr`.
- `LICENSE`.

`include/mbedtls/mbedtls_config.h` is the one file that differs from upstream.
It was produced with upstream's own `scripts/config.py` by turning OFF the
parts a client that only needs HTTPS/WSS does not use:

- the server role (`MBEDTLS_SSL_SRV_C`, `MBEDTLS_SSL_COOKIE_C`),
- DTLS (`MBEDTLS_SSL_PROTO_DTLS` and the DTLS helpers),
- TLS 1.0/1.1 (`MBEDTLS_SSL_PROTO_TLS1`, `MBEDTLS_SSL_PROTO_TLS1_1`),
- the PSK key exchanges (`MBEDTLS_PSK_MODES` and the `*_PSK_ENABLED` set),
- legacy ciphers/hashes we never negotiate (DES, Camellia, ARIA, Blowfish,
  XTEA, RC4, MD5, RIPEMD-160, SHA-3, NIST-KW),
- ECJPAKE and LMS,
- the certificate/CSR *writing* side (`MBEDTLS_X509_CREATE_C`,
  `MBEDTLS_X509_CRT_WRITE_C`, `MBEDTLS_X509_CSR_WRITE_C`, `MBEDTLS_PK_WRITE_C`,
  `MBEDTLS_PEM_WRITE_C`, `MBEDTLS_PKCS7_C`, `MBEDTLS_PKCS12_C`),
- `MBEDTLS_NET_C` and `MBEDTLS_TIMING_C`: the library is driven through our own
  POSIX-socket BIO callbacks (`Berry::TlsSocket`), so mbedTLS never opens a
  socket of its own and never reads the clock through its own wrapper,
- the debug/tracing and memory-buffer-allocator units, and the x86 AES-NI /
  Padlock / SSE2 fast paths (they are selected per build, not vendored).

TLS 1.3 is kept (`MBEDTLS_SSL_PROTO_TLS1_3`), as is `MBEDTLS_PSA_CRYPTO_C`
which TLS 1.3 requires. Certificate verification
(`MBEDTLS_X509_CRT_PARSE_C`, `MBEDTLS_X509_USE_C`) and the trusted-CA store
loader (`MBEDTLS_FS_IO`, `MBEDTLS_PLATFORM_C` for the file primitives) are kept:
a client that could not verify a chain would be worse than no TLS at all.

The SHA-256 implementation is mbedTLS's; `core/script/ed25519.cpp` remains the
separate, vendored Ed25519 used for manifests and signature packs.
