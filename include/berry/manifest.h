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
  Unknown = 0,
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
};

const char* CapKindName(CapKind k);
CapKind CapKindFromName(const std::string& name);

/// A single declared capability: a kind plus an optional scope qualifier.
/// `raw` preserves the original string for diagnostics and display.
struct BERRY_API Capability {
  CapKind kind = CapKind::Unknown;
  std::string scope;   // e.g. "api.example.com", "own-dir"; empty = default
  std::string raw;     // as written in the manifest

  /// Parses "kind" or "kind:scope" (scope may contain '=' for
  /// "network:domain=host"). Returns false for an unknown kind (fail closed).
  static bool Parse(const std::string& text, Capability* out);

  /// True when this capability (a GRANT) covers `requested`. A scope of "*"
  /// covers any scope of the same kind; otherwise scopes must match exactly.
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
};

const char* ManifestErrorName(ManifestError e);

struct BERRY_API ScriptManifest {
  std::string name;          // script id, e.g. "cps-counter"
  std::string version;       // semver-ish, e.g. "1.2.0"
  std::string entry;         // relative entry file, e.g. "main.luau"
  std::string author;        // optional
  std::string description;   // optional
  CapabilitySet capabilities;

  /// Hex SHA-256 over CanonicalPayload(). Set by Parse(); a manifest whose
  /// signature does not match its own payload is rejected.
  std::string payloadHash;
  /// Hex signature over payloadHash, produced by Sign()/an external signer.
  std::string signature;
  /// Algorithm id; only "hmac-sha256" is accepted today.
  std::string signatureAlgo;

  /// Parses a manifest document. On failure returns false and fills `error`
  /// (and `outError` when non-null). The manifest is fully validated except
  /// for signature verification, which is a separate explicit step.
  static bool Parse(const std::string& json, ScriptManifest* out,
                    ManifestError* outError = nullptr,
                    std::string* error = nullptr);

  /// Canonical signed payload: a deterministic serialization of every
  /// security-relevant field (name, version, entry, author, sorted
  /// capabilities). Deliberately excludes description and the signature
  /// itself so cosmetic edits do not invalidate a signature.
  std::string CanonicalPayload() const;

  /// Hex SHA-256 of CanonicalPayload().
  std::string ComputeHash() const;

  /// Signs the payload with `keyHex` (shared secret, HMAC-SHA256). Fills
  /// payloadHash, signature and signatureAlgo.
  bool Sign(const std::string& keyHex);

  /// Verifies signature over payloadHash with `keyHex`. Also re-derives the
  /// payload hash and rejects a mismatch (a tampered capability list fails
  /// even if the attacker could not forge a new signature).
  bool Verify(const std::string& keyHex, ManifestError* outError = nullptr) const;

  /// True when the entry path is safe: relative, no "..", no absolute path,
  /// no drive/backslash tricks. Guards against path traversal in the loader.
  static bool ValidEntryPath(const std::string& entry);
};

//----------------------------------------------------------------------------
// User approval + registry.
//----------------------------------------------------------------------------

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

  /// Registers a manifest after verifying it. Rejects a duplicate name or a
  /// manifest that fails signature verification. Fails closed.
  bool Register(const ScriptManifest& manifest, const std::string& keyHex,
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
// Crypto helpers (SHA-256 + HMAC-SHA256, dependency-free).
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

}  // namespace crypto

}  // namespace Berry
