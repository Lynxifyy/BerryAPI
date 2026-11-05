# BerryAPI security model

This document states what BerryAPI trusts, what it refuses, and why. It is the
threat model for the injected library; it is not a claim that the *game* is
secure, only that this library does not widen the attack surface it is injected
into.

## Trust boundary: injection

`libBerryAPI.so` is loaded into the Minecraft Bedrock process by a host (an
injector, a JNI descriptor, or an `LD_PRELOAD`-style loader). Everything the
library does runs with the game's own privileges. The library therefore assumes:

- The host that loads it is trusted. A host that can `dlopen` the library can
  already run arbitrary code, so the library does not try to defend against its
  own loader.
- The game process's memory is trusted for *reading*. Signature scanning reads
  live code; it fails closed on a fault (a `SIGSEGV` guard) rather than
  crashing, but it cannot make a hostile in-process actor safe.

What the library refuses to do:

- It never downloads or executes code. There is no plugin fetch, no JIT of
  remote input, no `require` of a network module.
- It never relaxes TLS. `TlsSocket` is `MBEDTLS_SSL_VERIFY_REQUIRED` with the
  URL host as the expected name, and there is deliberately no "skip verify" or
  "insecure" mode. When the CA store cannot be loaded, `Connect` refuses.
- It fails closed on every budget, capability and signature gate.

## Script sandbox

Scripts run in a sandboxed Luau VM (`luaL_sandbox`, no `require`, no raw
`io`/`os` escape). The guarantees are:

- **Capability gate, fail closed.** Every binding declares the capability it
  needs. A binding that needs none must declare `Capability::NoCap()`; an
  unset requirement is *not* a grant and is *not* grantable, so "the author
  forgot to declare a capability" can never read as "no capability required".
- **Budgets.** Each invocation is bounded by a real instruction count, a
  memory cap and a wall clock. A native binding's time is charged against the
  wall clock, so a blocking call cannot hide behind a cheap counter.
- **Manifest signature.** A manifest may require a minimum script-API version
  and is Ed25519-signed over a canonical, domain-separated, length-prefixed
  payload, so no field can be spliced into another. An empty trust anchor
  (with signing required) refuses every manifest.
- **No information the client was not already sent.** Client-side modules
  (HUD, overlays, cosmetic resolve, game reads) read only state the local
  client already holds. There is no ESP, reach or aim-assist surface: the
  world/entity seam is the client's own view.

## Network surface

- **Egress policy.** A request is checked against a `UrlPolicy` (https only, no
  IP literals, no private ranges by default) *and* the script's network grant.
  A redirect is re-checked on every hop.
- **HTTP.** Chunked decoding, `Content-Length` enforcement with a truncation
  flag, CR/LF/NUL rejection in URLs and headers, duplicate client-owned header
  rejection, IPv6/userinfo/port-range authority parsing.
- **WebSocket.** Random handshake key, `Sec-WebSocket-Accept` verification,
  `Upgrade`/`Connection` required, CSPRNG masking keys under the send lock, a
  strict client frame parser (masked/non-minimal/top-bit rejected), and a
  protocol close (1002/1009) instead of a silent desync.

## LAN channels (cosmetic sync, voice)

The LAN transports move bytes only; the channel verifies before anything is
applied.

- **Voice** is end-to-end encrypted and authenticated: Ed25519 identity
  handshake, X25519 + HKDF pairwise key, ChaCha20-Poly1305 frames, and a
  sliding-window replay filter that snapshots and rolls back when the AEAD tag
  fails (a forged frame cannot push a real one out of the window).
- **Proximity is judged against the listener's own world view**, so a speaker
  the listener's world does not hold is dropped; the module never reveals a
  player the client was not already sent.
- **The relay is blind.** `VoiceRelay` never holds a key and never decrypts; it
  forwards the sealed frame untouched and cannot forge audio. A per-room secret
  gates registration, so it is not an open amplifier.
- **Room tokens fail closed.** Signature verification is required unless the
  deployment explicitly opts in to TLS-only (`allowUnsigned`); expiry is
  required; relay port/expiry are integer-checked; a loopback/private relay
  host is refused unless `allowPrivateRelayHost` is set.

## Failure and recovery

- Every exported C entry catches all exceptions, so a `std::bad_alloc` cannot
  call `std::terminate` across the ABI boundary.
- A crash handler on an alternate signal stack writes a backtrace with
  module-relative offsets and the last log line to app-private storage, then
  re-raises the original signal.
- A consecutive-startup-crash counter lets the next boot enter safe mode (hooks
  and scripts disabled) rather than crash-looping.

## Reporting a vulnerability

Open a private security advisory on the repository, or contact the maintainer
directly. Please include a minimal reproducer and the BerryAPI version
(`BERRY_Version()`).
