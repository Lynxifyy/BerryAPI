#pragma once
// BerryAPI script capability manifests + signing.
//
// A script ships a manifest that DECLARES the capabilities it needs
// (`render`, `world:read`, `network:domain=api.example.com`, `fileio:own-dir`,
// …). Nothing is granted implicitly: a script runs only after the user
// approves the declared set once, and only the approved set is handed to the
// runtime. Manifests are signed so a tampered capability list fails
// verification and the script refuses to load.
//
// Design rules (mirroring the strict module system):
//   * Fail closed. An unparseable manifest, an unknown capability, an
//     unverified signature, or a capability outside the granted set disables
//     the script rather than degrading to "allow".
//   * No ambient authority. Capabilities are explicit strings; there is no
//     "trusted" flag and no wildcard default.
//   * Deterministic. The signed payload is a canonical, order-stable
//     serialization, so a byte-identical manifest always hashes the same.

#include "berry/export.h"
#include "berry/types.h"

#include <mutex>
#include <string>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Capability taxonomy.
//----------------------------------------------------------------------------

enum class CapKind : u32 {
  Unknown = 0,   // unset / absent: NOT a capability-free binding. See NoCap.
  NoCap,         // explicitly requires no capability (e.g. pure scheduling).
                 // Declared on purpose so a binding cannot be left ungated by
                 // accident: the gate denies Unknown and allows only NoCap.
  Network,   // outbound sockets; scope = allowed host/domain (or "*")
  Render,    // draw to the overlay; scope = unused
  FileIO,    // file access; scope = "own-dir" (script data dir) or a path
  World,     // read-only world/level access
  Combat,    // read-only damage observation
  Input,     // read-only local input events (keystrokes, clicks)
  Hud,       // retained-mode HUD widget tree
  Perf,      // performance governor / engine settings
  Events,    // subscribe to engine events
  Audio,     // play sounds
  Cosmetic,  // register cosmetics
  Packets,   // read-only packet metadata inspection (headers, never payloads)
};

const char* CapKindName(CapKind k);
CapKind CapKindFromName(const std::string& name);

/// True for a real, grantable capability kind. Unknown (unset) and NoCap
/// (explicitly capability-free) are both "not grantable", and the two are
/// deliberately distinct: an unset kind must never be treated as capability-
/// free, or a forgotten capability would silently become an open binding.
inline bool IsGrantableKind(CapKind k) {
  return k != CapKind::Unknown && k != CapKind::NoCap;
}

/// Normalizes a network capability scope to the bare host it names.
///
/// Manifests are written as `network:api.example.com`, but also historically
/// as `network:domain=api.example.com`. Both forms must mean the same host, or
/// a grant written one way would fail to cover a request built the other way.
/// Returns the lower-cased host, or an empty string when the scope names no
/// usable host (fail closed: an empty result covers nothing).
BERRY_API std::string NormalizeNetworkScope(const std::string& scope);

/// A single declared capability: a kind plus an optional scope qualifier.
/// `raw` preserves the original string for diagnostics and display.
struct BERRY_API Capability {
  CapKind kind = CapKind::Unknown;
  std::string scope;   // e.g. "api.example.com", "own-dir"; empty = default
  std::string raw;     // as written in the manifest

  /// Parses "kind" or "kind:scope" (scope may contain '=' for
  /// "network:domain=host"). Returns false for an unknown kind (fail closed).
  static bool Parse(const std::string& text, Capability* out);

  /// The explicit "requires no capability" requirement. A binding must name
  /// this (not an unset Capability) to be reachable without a grant; the gate
  /// treats Unknown as "deny".
  static Capability NoCap() {
    Capability c;
    c.kind = CapKind::NoCap;
    c.raw = "nocap";
    return c;
  }

  /// True when this capability (a GRANT) covers `requested`. A scope of "*"
  /// covers any scope of the same kind; otherwise scopes must match exactly.
  /// An Unknown kind covers nothing.
  bool Covers(const Capability& requested) const;

  /// Canonical text form used in the signed payload.
  std::string Canonical() const;
};

/// An ordered, de-duplicated set of capabilities.
class BERRY_API CapabilitySet {
 public:
  /// Adds a capability. Returns false when the text is unparseable or the
  /// capability is already present.
  bool Add(const std::string& text);
  bool Add(const Capability& cap);

  /// True when every capability in `needed` is covered by this set.
  bool Covers(const CapabilitySet& needed) const;

  /// True when a single capability is covered by this set.
  bool CoversCap(const Capability& cap) const;

  bool Has(CapKind kind) const;
  const std::vector<Capability>& All() const { return caps_; }
  size_t Count() const { return caps_.size(); }
  bool empty() const { return caps_.empty(); }

  /// Canonical, sorted-by-text form (stable regardless of insertion order).
  std::string Canonical() const;

  /// Collapses the set into the module capability mask (kCap*), so a manifest
  /// can be checked against a module's declared capabilities.
  u32 ToModuleMask() const;

  /// Parses a manifest capability array element list.
  static bool ParseAll(const std::vector<std::string>& texts, CapabilitySet* out,
                       std::string* error);

 private:
  std::vector<Capability> caps_;
};

//----------------------------------------------------------------------------
// Script manifest.
//----------------------------------------------------------------------------

/// Manifest validation failures. Every one of these disables the script.
enum class ManifestError : u32 {
  Ok = 0,
  MalformedJson,
  MissingField,
  BadVersion,
  UnknownCapability,
  DuplicateCapability,
  BadEntryPath,
  Unsigned,
  BadSignature,
  HashMismatch,
  UnsupportedAlgorithm,
  MissingSourceHash,
  SourceHashMismatch,
};

const char* ManifestErrorName(ManifestError e);

struct BERRY_API ScriptManifest {
  std::string name;          // script id, e.g. "cps-counter"
  std::string version;       // semver-ish, e.g. "1.2.0"
  std::string entry;         // relative entry file, e.g. "main.luau"
  std::string author;        // optional
  std::string description;   // optional
  CapabilitySet capabilities;
  /// Hex SHA-256 of the entry source. Part of the signed payload, so a
  /// signature attests to the code that will actually run and not just to the
  /// capability list: swapping the script while keeping the manifest is caught
  /// at load time.
  std::string sourceSha256;

  /// Hex SHA-256 over CanonicalPayload(). Set by Parse(); a manifest whose
  /// signature does not match its own payload is rejected.
  std::string payloadHash;
  /// Hex signature over payloadHash, produced by Sign()/an external signer.
  std::string signature;
  /// Algorithm id: "ed25519" (asymmetric, the production path) or
  /// "hmac-sha256" (shared secret, local development and self-tests).
  std::string signatureAlgo;

  /// Parses a manifest document. On failure returns false and fills `error`
  /// (and `outError` when non-null). The manifest is fully validated except
  /// for signature verification, which is a separate explicit step.
  static bool Parse(const std::string& json, ScriptManifest* out,
                    ManifestError* outError = nullptr,
                    std::string* error = nullptr);

  /// Canonical signed payload: a deterministic serialization of every
  /// security-relevant field (name, version, entry, author, sorted
  /// capabilities, source hash). Deliberately excludes description and the
  /// signature itself so cosmetic edits do not invalidate a signature.
  std::string CanonicalPayload() const;

  /// Hex SHA-256 of CanonicalPayload().
  std::string ComputeHash() const;

  /// Signs the payload with `keyHex` (shared secret, HMAC-SHA256). Fills
  /// payloadHash, signature and signatureAlgo. Local development and
  /// self-tests only: HMAC uses the same secret to sign and to verify, so
  /// every verifier can also forge.
  bool Sign(const std::string& keyHex);

  /// Verifies an HMAC-SHA256 signature over payloadHash with `keyHex`. Also
  /// re-derives the payload hash and rejects a mismatch (a tampered capability
  /// list fails even if the attacker could not forge a new signature).
  bool Verify(const std::string& keyHex, ManifestError* outError = nullptr) const;

  /// Signs the payload with a 64-byte Ed25519 secret key. This is the
  /// production path: the device is provisioned with the matching public key
  /// only, so it can verify a script but never mint one.
  bool SignEd25519(const std::string& secretKey64);

  /// Verifies an Ed25519 signature against `publicKeyHex` (32 bytes, hex).
  /// Fails closed: the algorithm must be "ed25519", the signature must be a
  /// 64-byte hex string, the re-derived payload hash must match, and — unless
  /// `requireSourceHash` is false — the payload must bind a source hash.
  bool VerifyEd25519(const std::string& publicKeyHex,
                     ManifestError* outError = nullptr,
                     bool requireSourceHash = true) const;

  /// Hex SHA-256 of `source`, the value a manifest's sourceSha256 must carry.
  static std::string SourceHash(const std::string& source);

  /// True when the entry path is safe: relative, no "..", no absolute path,
  /// no drive/backslash tricks. Guards against path traversal in the loader.
  static bool ValidEntryPath(const std::string& entry);
};

//----------------------------------------------------------------------------
// User approval + registry.
//----------------------------------------------------------------------------

/// Signature algorithm identifiers, kept as constants so the signer tool, the
/// loader and the docs agree on the spelling.
namespace signature_algo {
inline const char* Ed25519() { return "ed25519"; }
inline const char* HmacSha256() { return "hmac-sha256"; }
}  // namespace signature_algo

/// What a manifest is verified against.
///
/// Ed25519 is the production anchor: the device stores a public key, so
/// verification does not hand out the ability to sign. HMAC exists for local
/// development and the self-tests, where a single shared secret is convenient.
struct BERRY_API TrustAnchor {
  enum class Kind : u32 { Ed25519 = 0, Hmac };

  Kind kind = Kind::Ed25519;
  std::string keyHex;   // Ed25519 public key (64 hex chars) or HMAC secret

  static TrustAnchor Ed25519Key(std::string hex);
  static TrustAnchor HmacSecret(std::string secret);

  /// True when the anchor is usable: the kind must be known and the key must
  /// be well formed for it. Fails closed for anything else.
  bool Valid() const;

  /// Verifies `manifest` against this anchor. Dispatches on `kind`; an unknown
  /// kind is a failure, never an allow.
  bool Verify(const ScriptManifest& manifest, ManifestError* outError = nullptr,
              bool requireSourceHash = true) const;
};

/// A one-time user approval: the capabilities a named script may use, bound to
/// the exact version that was reviewed. A version change invalidates the
/// grant so new capabilities cannot ride an old approval.
struct BERRY_API CapabilityGrant {
  std::string name;
  std::string version;
  CapabilitySet granted;
  u64 approvedAtMs = 0;
};

/// Tracks manifests and their approvals. Thread-safe.
class BERRY_API ManifestStore {
 public:
  static ManifestStore& Instance();

  /// Registers a manifest after verifying it against `anchor`. Rejects a
  /// duplicate name or a manifest that fails signature verification. Fails
  /// closed.
  bool Register(const ScriptManifest& manifest, const TrustAnchor& anchor,
                ManifestError* outError = nullptr);

  /// Convenience overload for the shared-secret (HMAC) path used by local
  /// development and the self-tests.
  bool Register(const ScriptManifest& manifest, const std::string& hmacSecret,
                ManifestError* outError = nullptr);

  /// Records a user approval of a registered manifest's declared
  /// capabilities. The grant is capped at what the manifest declared, so an
  /// approval can never widen access beyond the reviewed list.
  bool Approve(const std::string& name, u64 nowMs);

  bool Revoke(const std::string& name);

  /// Returns the grant for a script, or nullptr when not approved. A grant
  /// from a different version than the registered manifest is ignored.
  const CapabilityGrant* Grant(const std::string& name) const;

  bool IsApproved(const std::string& name) const;

  /// The effective capabilities for a script: the intersection of the
  /// manifest's declared set and the approved grant. Empty when unapproved.
  CapabilitySet EffectiveCapabilities(const std::string& name) const;

  bool Find(const std::string& name, ScriptManifest* out) const;
  size_t Count() const;
  void Clear();

 private:
  ManifestStore() = default;
  struct Entry {
    ScriptManifest manifest;
    CapabilityGrant grant;
    bool hasGrant = false;
  };
  mutable std::mutex mutex_;
  std::vector<Entry> entries_;
};

//----------------------------------------------------------------------------
// Crypto helpers (SHA-256, HMAC-SHA256, Ed25519; dependency-free).
//----------------------------------------------------------------------------

namespace crypto {

/// Lowercase hex SHA-256 of `data`.
BERRY_API std::string Sha256Hex(const void* data, size_t len);
BERRY_API std::string Sha256Hex(const std::string& data);

/// Lowercase hex HMAC-SHA256 of `data` under `key`.
BERRY_API std::string HmacSha256Hex(const std::string& key,
                                    const std::string& data);

/// Constant-time comparison of two equal-purpose hex digests.
BERRY_API bool ConstantTimeEquals(const std::string& a, const std::string& b);

/// Validates a hex string (even length, lowercase/uppercase hex).
BERRY_API bool IsHex(const std::string& s);

/// Lowercase hex of a raw byte string.
BERRY_API std::string ToHexLower(const std::string& bytes);

/// Decodes a lowercase/uppercase hex string into raw bytes. False when the
/// input is not valid hex, or is not exactly `expectedBytes` long (pass 0 to
/// accept any length).
BERRY_API bool FromHex(const std::string& hex, std::string* bytes,
                       size_t expectedBytes = 0);

/// Decodes `hex` (64 chars) into a 32-byte Ed25519 public key. False when the
/// input is not exactly 32 hex-encoded bytes.
BERRY_API bool Ed25519PublicKeyFromHex(const std::string& hex,
                                       std::string* out32);

/// Derives the 32-byte public key and 64-byte secret key from a 32-byte seed.
/// Deterministic: the caller owns the seed, so nothing here reads
/// /dev/urandom from inside the injected library.
BERRY_API bool Ed25519KeypairFromSeed(const std::string& seed32,
                                      std::string* publicKey32,
                                      std::string* secretKey64);

/// Signs `message` with the 64-byte secret key; returns a 64-byte signature.
BERRY_API bool Ed25519Sign(const std::string& message,
                           const std::string& secretKey64,
                           std::string* signature64);

/// Verifies a 64-byte signature over `message` under a 32-byte public key.
/// Returns false for any malformed input, wrong sizes included.
BERRY_API bool Ed25519Verify(const std::string& message,
                             const std::string& signature64,
                             const std::string& publicKey32);

}  // namespace crypto

}  // namespace Berry
