// Capability manifests + signing. See include/berry/manifest.h for the rules.
#include "berry/manifest.h"
#include "berry/logger.h"
#include "berry/module.h"   // kCap* mask bits for CapabilitySet::ToModuleMask
#include "json_min.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace Berry {

//----------------------------------------------------------------------------
// Capability taxonomy
//----------------------------------------------------------------------------

namespace {
struct KindRow {
  const char* name;
  CapKind kind;
};
const KindRow kKinds[] = {
    {"network", CapKind::Network},   {"render", CapKind::Render},
    {"fileio", CapKind::FileIO},     {"world", CapKind::World},
    {"combat", CapKind::Combat},     {"input", CapKind::Input},
    {"hud", CapKind::Hud},           {"perf", CapKind::Perf},
    {"events", CapKind::Events},     {"audio", CapKind::Audio},
    {"cosmetic", CapKind::Cosmetic},
};
// "nocap" is deliberately NOT in the table above: it is a binding REQUIREMENT,
// never a manifest grant, so a manifest must not be able to write it and a
// script must never be granted it. It is still nameable for diagnostics.
const char* kNoCapName = "nocap";
}  // namespace

const char* CapKindName(CapKind k) {
  if (k == CapKind::NoCap) return kNoCapName;
  for (const auto& row : kKinds) {
    if (row.kind == k) return row.name;
  }
  return "unknown";
}

CapKind CapKindFromName(const std::string& name) {
  for (const auto& row : kKinds) {
    if (name == row.name) return row.kind;
  }
  return CapKind::Unknown;
}

std::string NormalizeNetworkScope(const std::string& scope) {
  std::string s = scope;
  // Tolerate the historical "domain=host" spelling so a grant written either
  // way covers a request built from the other.
  size_t eq = s.find('=');
  if (eq != std::string::npos) {
    std::string key = s.substr(0, eq);
    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    if (key == "domain" || key == "host") {
      s = s.substr(eq + 1);
    } else {
      return std::string();   // unknown qualifier: covers nothing
    }
  }
  // A scope is a host, never a URL: reject anything that tries to smuggle a
  // scheme, path, port or userinfo into the comparison.
  if (s.empty()) return std::string();
  if (s.find("://") != std::string::npos) return std::string();
  if (s.find('/') != std::string::npos) return std::string();
  if (s.find('@') != std::string::npos) return std::string();
  if (s.find('*') != std::string::npos) return std::string();   // "*" is not a host
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  while (!s.empty() && s.back() == '.') s.pop_back();   // "host." == "host"
  if (s.empty()) return std::string();
  if (s.front() == '[' && s.back() == ']') s = s.substr(1, s.size() - 2);
  return s;
}

bool Capability::Parse(const std::string& text, Capability* out) {
  if (!out) return false;
  out->scope.clear();
  out->raw = text;
  out->kind = CapKind::Unknown;
  if (text.empty()) return false;

  std::string kindPart = text;
  size_t colon = text.find(':');
  if (colon != std::string::npos) {
    kindPart = text.substr(0, colon);
    out->scope = text.substr(colon + 1);
    if (out->scope.empty()) return false;
  }
  CapKind kind = CapKindFromName(kindPart);
  if (kind == CapKind::Unknown) return false;
  out->kind = kind;
  return true;
}

bool Capability::Covers(const Capability& requested) const {
  if (kind != requested.kind) return false;
  // Neither unset (Unknown) nor capability-free (NoCap) is a grantable
  // capability, so neither may ever act as a grant: a capability-free binding
  // is allowed by the gate's explicit NoCap check, never by "covering" it.
  if (!IsGrantableKind(kind)) return false;
  // A wildcard grant covers any scope of the same kind.
  if (scope == "*") return true;
  // A wildcard REQUEST is a kind-level requirement: "this binding needs some
  // capability of this kind, and it narrows the check to the concrete host
  // itself" (see HttpGetBlocking). Any grant of the same kind satisfies it.
  // Note this must come before the empty-scope rule below, so that even a
  // legacy unscoped grant reaches the binding and gets a precise
  // "host not granted" refusal instead of a bare gate denial.
  if (requested.scope == "*") return true;
  // An unscoped grant is unscoped: it must not silently cover a scoped request
  // (that is how a bare `network` grant ended up reaching every host).
  if (scope.empty()) return requested.scope.empty();
  // Network scopes name a host, and both sides may be spelled with the legacy
  // "domain=" qualifier, so compare the normalized host.
  if (kind == CapKind::Network) {
    const std::string have = NormalizeNetworkScope(scope);
    const std::string want = NormalizeNetworkScope(requested.scope);
    return !have.empty() && have == want;
  }
  return scope == requested.scope;
}

std::string Capability::Canonical() const {
  std::string s = CapKindName(kind);
  if (!scope.empty()) {
    s += ":";
    s += scope;
  }
  return s;
}

//----------------------------------------------------------------------------
// CapabilitySet
//----------------------------------------------------------------------------

bool CapabilitySet::Add(const std::string& text) {
  Capability cap;
  if (!Capability::Parse(text, &cap)) return false;
  return Add(cap);
}

bool CapabilitySet::Add(const Capability& cap) {
  // Only real, grantable kinds may enter a grant set: an unset kind (Unknown)
  // would be meaningless and NoCap is a binding requirement, never a grant.
  if (!IsGrantableKind(cap.kind)) return false;
  const std::string canon = cap.Canonical();
  for (const auto& existing : caps_) {
    if (existing.Canonical() == canon) return false;  // duplicate
  }
  caps_.push_back(cap);
  return true;
}

bool CapabilitySet::Covers(const CapabilitySet& needed) const {
  for (const auto& want : needed.caps_) {
    bool covered = false;
    for (const auto& have : caps_) {
      if (have.Covers(want)) {
        covered = true;
        break;
      }
    }
    if (!covered) return false;
  }
  return true;
}

bool CapabilitySet::CoversCap(const Capability& cap) const {
  for (const auto& have : caps_) {
    if (have.Covers(cap)) return true;
  }
  return false;
}

bool CapabilitySet::Has(CapKind kind) const {
  for (const auto& c : caps_) {
    if (c.kind == kind) return true;
  }
  return false;
}

std::string CapabilitySet::Canonical() const {
  std::vector<std::string> parts;
  parts.reserve(caps_.size());
  for (const auto& c : caps_) parts.push_back(c.Canonical());
  std::sort(parts.begin(), parts.end());
  parts.erase(std::unique(parts.begin(), parts.end()), parts.end());
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i) out += ",";
    out += parts[i];
  }
  return out;
}

u32 CapabilitySet::ToModuleMask() const {
  u32 mask = kCapNone;
  for (const auto& c : caps_) {
    switch (c.kind) {
      case CapKind::Combat: mask |= kCapCombat; break;
      case CapKind::Render:
      case CapKind::Hud:
      case CapKind::Cosmetic: mask |= kCapRender; break;
      case CapKind::World: mask |= kCapWorld; break;
      case CapKind::Network: mask |= kCapNetwork; break;
      case CapKind::FileIO: mask |= kCapFileIO; break;
      default: break;
    }
  }
  return mask;
}

bool CapabilitySet::ParseAll(const std::vector<std::string>& texts,
                             CapabilitySet* out, std::string* error) {
  if (!out) return false;
  CapabilitySet set;
  for (const auto& text : texts) {
    if (!set.Add(text)) {
      if (error) *error = "invalid or duplicate capability: " + text;
      return false;
    }
  }
  *out = set;
  return true;
}

//----------------------------------------------------------------------------
// ScriptManifest
//----------------------------------------------------------------------------

const char* ManifestErrorName(ManifestError e) {
  switch (e) {
    case ManifestError::Ok: return "ok";
    case ManifestError::MalformedJson: return "malformed-json";
    case ManifestError::MissingField: return "missing-field";
    case ManifestError::BadVersion: return "bad-version";
    case ManifestError::UnknownCapability: return "unknown-capability";
    case ManifestError::DuplicateCapability: return "duplicate-capability";
    case ManifestError::BadEntryPath: return "bad-entry-path";
    case ManifestError::Unsigned: return "unsigned";
    case ManifestError::BadSignature: return "bad-signature";
    case ManifestError::HashMismatch: return "hash-mismatch";
    case ManifestError::UnsupportedAlgorithm: return "unsupported-algorithm";
    case ManifestError::MissingSourceHash: return "missing-source-hash";
    case ManifestError::SourceHashMismatch: return "source-hash-mismatch";
  }
  return "unknown";
}

bool ScriptManifest::ValidEntryPath(const std::string& entry) {
  if (entry.empty()) return false;
  if (entry.size() > 255) return false;
  // Reject absolute paths and Windows-style separators/drive letters.
  if (entry[0] == '/' || entry[0] == '\\') return false;
  if (entry.find('\\') != std::string::npos) return false;
  if (entry.size() >= 2 && entry[1] == ':') return false;
  // Reject traversal and empty segments ("a//b", "./x", "a/./b").
  size_t start = 0;
  while (start <= entry.size()) {
    size_t slash = entry.find('/', start);
    std::string seg = entry.substr(
        start, slash == std::string::npos ? std::string::npos : slash - start);
    if (seg.empty() || seg == "." || seg == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

namespace {

bool JsonToStringList(const detail::json::Value& v, std::vector<std::string>* out,
                      std::string* error) {
  if (!v.IsArray()) {
    if (error) *error = "capabilities must be an array";
    return false;
  }
  for (const auto& item : v.array) {
    if (!item.IsString()) {
      if (error) *error = "capabilities entries must be strings";
      return false;
    }
    out->push_back(item.str);
  }
  return true;
}

}  // namespace

bool ScriptManifest::Parse(const std::string& json, ScriptManifest* out,
                           ManifestError* outError, std::string* error) {
  auto fail = [&](ManifestError code, const std::string& msg) {
    if (outError) *outError = code;
    if (error) *error = msg;
    return false;
  };

  detail::json::Value root;
  std::string jerr;
  if (!detail::json::Parse(json, &root, &jerr)) {
    return fail(ManifestError::MalformedJson, jerr);
  }
  if (!root.IsObject()) {
    return fail(ManifestError::MalformedJson, "manifest root must be an object");
  }

  ScriptManifest m;
  const detail::json::Value* name = root.Get("name");
  if (!name || !name->IsString() || name->str.empty()) {
    return fail(ManifestError::MissingField, "missing or empty 'name'");
  }
  m.name = name->str;

  const detail::json::Value* version = root.Get("version");
  if (!version || !version->IsString() || version->str.empty()) {
    return fail(ManifestError::MissingField, "missing or empty 'version'");
  }
  m.version = version->str;

  const detail::json::Value* entry = root.Get("entry");
  if (!entry || !entry->IsString() || entry->str.empty()) {
    return fail(ManifestError::MissingField, "missing or empty 'entry'");
  }
  m.entry = entry->str;
  if (!ValidEntryPath(m.entry)) {
    return fail(ManifestError::BadEntryPath, "unsafe entry path: " + m.entry);
  }

  if (const detail::json::Value* author = root.Get("author")) {
    if (author->IsString()) m.author = author->str;
  }
  if (const detail::json::Value* desc = root.Get("description")) {
    if (desc->IsString()) m.description = desc->str;
  }

  const detail::json::Value* caps = root.Get("capabilities");
  if (!caps) {
    return fail(ManifestError::MissingField, "missing 'capabilities'");
  }
  std::vector<std::string> capTexts;
  std::string cerr;
  if (!JsonToStringList(*caps, &capTexts, &cerr)) {
    return fail(ManifestError::MalformedJson, cerr);
  }
  CapabilitySet set;
  for (const auto& text : capTexts) {
    Capability cap;
    if (!Capability::Parse(text, &cap)) {
      return fail(ManifestError::UnknownCapability,
                  "unknown or malformed capability: " + text);
    }
    if (!set.Add(cap)) {
      return fail(ManifestError::DuplicateCapability,
                  "duplicate capability: " + cap.Canonical());
    }
  }
  m.capabilities = set;

  if (const detail::json::Value* sig = root.Get("signature")) {
    if (sig->IsString()) m.signature = sig->str;
  }
  if (const detail::json::Value* algo = root.Get("signature_algo")) {
    if (algo->IsString()) m.signatureAlgo = algo->str;
  }
  if (const detail::json::Value* hash = root.Get("payload_hash")) {
    if (hash->IsString()) m.payloadHash = hash->str;
  }
  if (const detail::json::Value* src = root.Get("source_sha256")) {
    if (src->IsString()) m.sourceSha256 = src->str;
  }

  *out = m;
  if (outError) *outError = ManifestError::Ok;
  return true;
}

std::string ScriptManifest::CanonicalPayload() const {
  // Deterministic, order-stable serialization. Only security-relevant fields
  // are included; description and the signature are intentionally excluded.
  std::string s;
  s += "berry-manifest-v1\n";
  s += "name=";
  s += name;
  s += "\nversion=";
  s += version;
  s += "\nentry=";
  s += entry;
  s += "\nauthor=";
  s += author;
  s += "\ncapabilities=";
  s += capabilities.Canonical();
  s += "\nsource_sha256=";
  s += sourceSha256;
  s += "\n";
  return s;
}

std::string ScriptManifest::SourceHash(const std::string& source) {
  return crypto::Sha256Hex(source);
}

std::string ScriptManifest::ComputeHash() const {
  return crypto::Sha256Hex(CanonicalPayload());
}

bool ScriptManifest::Sign(const std::string& keyHex) {
  payloadHash = ComputeHash();
  signature = crypto::HmacSha256Hex(keyHex, payloadHash);
  signatureAlgo = signature_algo::HmacSha256();
  return true;
}

bool ScriptManifest::SignEd25519(const std::string& secretKey64) {
  payloadHash = ComputeHash();
  // The signature covers the raw 32-byte digest, not its hex spelling, so the
  // same payload hashes and signs identically here and in the Python signer.
  std::string digest;
  if (!crypto::FromHex(payloadHash, &digest, 32)) return false;
  std::string sig;
  if (!crypto::Ed25519Sign(digest, secretKey64, &sig)) return false;
  signature = crypto::ToHexLower(sig);
  signatureAlgo = signature_algo::Ed25519();
  return true;
}

bool ScriptManifest::VerifyEd25519(const std::string& publicKeyHex,
                                   ManifestError* outError,
                                   bool requireSourceHash) const {
  auto fail = [&](ManifestError code) {
    if (outError) *outError = code;
    return false;
  };
  if (signature.empty() || signatureAlgo.empty()) {
    return fail(ManifestError::Unsigned);
  }
  if (signatureAlgo != signature_algo::Ed25519()) {
    return fail(ManifestError::UnsupportedAlgorithm);
  }
  // A signed payload that does not bind the code is only a capability claim,
  // so the source hash is mandatory on this path.
  if (requireSourceHash && sourceSha256.empty()) {
    return fail(ManifestError::MissingSourceHash);
  }
  std::string sig, pk;
  if (!crypto::FromHex(signature, &sig, 64)) {
    return fail(ManifestError::BadSignature);
  }
  if (!crypto::Ed25519PublicKeyFromHex(publicKeyHex, &pk)) {
    return fail(ManifestError::BadSignature);
  }
  // Re-derive the payload hash: a tampered capability list or a swapped source
  // hash fails here even before the signature is checked.
  const std::string expectHash = ComputeHash();
  if (!payloadHash.empty() && !crypto::ConstantTimeEquals(payloadHash, expectHash)) {
    return fail(ManifestError::HashMismatch);
  }
  std::string digest;
  if (!crypto::FromHex(expectHash, &digest, 32)) {
    return fail(ManifestError::BadSignature);
  }
  if (!crypto::Ed25519Verify(digest, sig, pk)) {
    return fail(ManifestError::BadSignature);
  }
  if (outError) *outError = ManifestError::Ok;
  return true;
}

bool ScriptManifest::Verify(const std::string& keyHex,
                            ManifestError* outError) const {
  auto fail = [&](ManifestError code) {
    if (outError) *outError = code;
    return false;
  };
  if (signature.empty() || signatureAlgo.empty()) {
    return fail(ManifestError::Unsigned);
  }
  if (signatureAlgo != signature_algo::HmacSha256()) {
    return fail(ManifestError::UnsupportedAlgorithm);
  }
  if (!crypto::IsHex(signature) || signature.size() != 64) {
    return fail(ManifestError::BadSignature);
  }
  // Re-derive the payload hash: a tampered capability list fails here even if
  // the attacker copied the old signature verbatim.
  const std::string expectHash = ComputeHash();
  if (!payloadHash.empty() && !crypto::ConstantTimeEquals(payloadHash, expectHash)) {
    return fail(ManifestError::HashMismatch);
  }
  const std::string expectSig = crypto::HmacSha256Hex(keyHex, expectHash);
  if (!crypto::ConstantTimeEquals(signature, expectSig)) {
    return fail(ManifestError::BadSignature);
  }
  if (outError) *outError = ManifestError::Ok;
  return true;
}

//----------------------------------------------------------------------------
// TrustAnchor
//----------------------------------------------------------------------------

TrustAnchor TrustAnchor::Ed25519Key(std::string hex) {
  TrustAnchor a;
  a.kind = Kind::Ed25519;
  a.keyHex = std::move(hex);
  return a;
}

TrustAnchor TrustAnchor::HmacSecret(std::string secret) {
  TrustAnchor a;
  a.kind = Kind::Hmac;
  a.keyHex = std::move(secret);
  return a;
}

bool TrustAnchor::Valid() const {
  switch (kind) {
    case Kind::Ed25519: {
      std::string pk;
      return crypto::Ed25519PublicKeyFromHex(keyHex, &pk);
    }
    case Kind::Hmac:
      return !keyHex.empty();
  }
  return false;
}

bool TrustAnchor::Verify(const ScriptManifest& manifest,
                         ManifestError* outError, bool requireSourceHash) const {
  if (!Valid()) {
    if (outError) *outError = ManifestError::BadSignature;
    return false;
  }
  switch (kind) {
    case Kind::Ed25519:
      return manifest.VerifyEd25519(keyHex, outError, requireSourceHash);
    case Kind::Hmac:
      return manifest.Verify(keyHex, outError);
  }
  if (outError) *outError = ManifestError::BadSignature;
  return false;
}

//----------------------------------------------------------------------------
// ManifestStore
//----------------------------------------------------------------------------

ManifestStore& ManifestStore::Instance() {
  static ManifestStore store;
  return store;
}

bool ManifestStore::Register(const ScriptManifest& manifest,
                             const std::string& hmacSecret,
                             ManifestError* outError) {
  return Register(manifest, TrustAnchor::HmacSecret(hmacSecret), outError);
}

bool ManifestStore::Register(const ScriptManifest& manifest,
                             const TrustAnchor& anchor,
                             ManifestError* outError) {
  ManifestError verr = ManifestError::Ok;
  if (!anchor.Verify(manifest, &verr)) {
    Log::Error("ManifestStore: rejecting '%s': %s", manifest.name.c_str(),
               ManifestErrorName(verr));
    if (outError) *outError = verr;
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& e : entries_) {
    if (e.manifest.name == manifest.name) {
      Log::Error("ManifestStore: duplicate script '%s'", manifest.name.c_str());
      if (outError) *outError = ManifestError::BadVersion;
      return false;
    }
  }
  Entry entry;
  entry.manifest = manifest;
  entries_.push_back(std::move(entry));
  Log::Info("ManifestStore: registered '%s' v%s (%zu caps)",
            manifest.name.c_str(), manifest.version.c_str(),
            manifest.capabilities.Count());
  if (outError) *outError = ManifestError::Ok;
  return true;
}

bool ManifestStore::Approve(const std::string& name, u64 nowMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& e : entries_) {
    if (e.manifest.name != name) continue;
    e.grant.name = name;
    e.grant.version = e.manifest.version;
    // The grant is capped at the declared set: approval cannot widen access.
    e.grant.granted = e.manifest.capabilities;
    e.grant.approvedAtMs = nowMs;
    e.hasGrant = true;
    Log::Info("ManifestStore: approved '%s' v%s", name.c_str(),
              e.manifest.version.c_str());
    return true;
  }
  Log::Warn("ManifestStore: cannot approve unknown script '%s'", name.c_str());
  return false;
}

bool ManifestStore::Revoke(const std::string& name) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& e : entries_) {
    if (e.manifest.name != name) continue;
    e.hasGrant = false;
    e.grant = CapabilityGrant{};
    return true;
  }
  return false;
}

const CapabilityGrant* ManifestStore::Grant(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& e : entries_) {
    if (e.manifest.name != name) continue;
    if (!e.hasGrant) return nullptr;
    // A grant bound to a different version no longer applies.
    if (e.grant.version != e.manifest.version) return nullptr;
    return &e.grant;
  }
  return nullptr;
}

bool ManifestStore::IsApproved(const std::string& name) const {
  return Grant(name) != nullptr;
}

CapabilitySet ManifestStore::EffectiveCapabilities(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  CapabilitySet out;
  for (const auto& e : entries_) {
    if (e.manifest.name != name) continue;
    if (!e.hasGrant || e.grant.version != e.manifest.version) return out;
    // Intersection: only capabilities present in BOTH the declared manifest
    // and the approval survive.
    for (const auto& cap : e.grant.granted.All()) {
      for (const auto& decl : e.manifest.capabilities.All()) {
        if (decl.Canonical() == cap.Canonical()) {
          out.Add(cap);
          break;
        }
      }
    }
    return out;
  }
  return out;
}

bool ManifestStore::Find(const std::string& name, ScriptManifest* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& e : entries_) {
    if (e.manifest.name == name) {
      if (out) *out = e.manifest;
      return true;
    }
  }
  return false;
}

size_t ManifestStore::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return entries_.size();
}

void ManifestStore::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  entries_.clear();
}

}  // namespace Berry
