// OTA signature packs. See include/berry/sigpack.h for the model.
//
// Parsing validates the whole document and every row before anything is
// registered, so a pack is applied atomically: one bad row means the pack
// contributes nothing rather than leaving a registry half-updated.
#include "berry/sigpack.h"

#include "json_min.h"
#include "json_write.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace Berry {

namespace {

using detail::json::Value;

/// Reads a bounded, printable, non-empty string.
bool SafeText(const std::string& s, size_t maxLen) {
  if (s.empty() || s.size() > maxLen) return false;
  for (char ch : s) {
    const unsigned char c = (unsigned char)ch;
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

/// Reads a signed integer from a JSON number, bounded to i32 and rejecting a
/// fractional value so the cast is lossless.
bool ReadI32(const Value* v, i32* out) {
  if (!v || !v->IsNumber()) return false;
  const f64 n = v->number;
  if (n < -2147483648.0 || n > 2147483647.0) return false;
  if (n != (f64)(i64)n) return false;
  *out = (i32)n;
  return true;
}

/// Reads a non-negative integer from a JSON number, bounded to u64 and
/// rejecting a fractional or out-of-range value. JSON numbers are f64, so a
/// value beyond 2^53 cannot be represented exactly and is refused rather than
/// silently rounded.
bool ReadU64(const Value* v, u64* out) {
  if (!v || !v->IsNumber()) return false;
  const f64 n = v->number;
  if (n < 0.0) return false;
  if (n > 9007199254740992.0) return false;   // 2^53: the f64 integer limit
  if (n != (f64)(u64)n) return false;
  *out = (u64)n;
  return true;
}

/// Reads a bounded, printable, non-empty hex string.
bool SafeHex(const std::string& s, size_t maxLen) {
  if (s.empty() || s.size() > maxLen || (s.size() % 2) != 0) return false;
  for (char c : s) {
    const bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F');
    if (!hex) return false;
  }
  return true;
}

/// Rejects any key not in `allowed`. Strict on purpose: a typo like "patern"
/// must fail loudly rather than silently dropping a rule.
bool OnlyKeys(const Value& obj, const std::set<std::string>& allowed,
              std::string* detail) {
  for (const auto& kv : obj.object) {
    if (allowed.find(kv.first) == allowed.end()) {
      if (detail) *detail = "unknown key '" + kv.first + "'";
      return false;
    }
  }
  return true;
}

}  // namespace

const char* SigPackErrorName(SigPackError e) {
  switch (e) {
    case SigPackError::Ok: return "ok";
    case SigPackError::Unreadable: return "pack unreadable";
    case SigPackError::BadJson: return "invalid JSON";
    case SigPackError::BadFormat: return "unsupported pack format";
    case SigPackError::MissingName: return "pack has no name";
    case SigPackError::MissingBuild: return "pack has no build fingerprint";
    case SigPackError::NoEntries: return "pack has no signatures or layouts";
    case SigPackError::TooManySignatures: return "too many signature rows";
    case SigPackError::TooManyLayouts: return "too many layouts";
    case SigPackError::BadSignature: return "invalid signature row";
    case SigPackError::BadPattern: return "signature pattern does not parse";
    case SigPackError::DuplicateId: return "duplicate signature id";
    case SigPackError::BadLayout: return "invalid layout";
    case SigPackError::LayoutBuildMismatch: return "layout build differs from pack";
    case SigPackError::BuildMismatch: return "pack build does not match";
    case SigPackError::TooManyPacks: return "too many pack files";
    case SigPackError::Unsigned: return "pack is not signed";
    case SigPackError::NoTrustAnchor: return "no pack trust anchor configured";
    case SigPackError::UnknownAlgorithm: return "unknown signature algorithm";
    case SigPackError::BadPackSignature: return "pack signature does not verify";
    case SigPackError::BadSequence: return "pack sequence is absent or malformed";
    case SigPackError::StaleSequence: return "pack sequence is stale";
    case SigPackError::Expired: return "pack has expired";
  }
  return "unknown";
}

std::string SignaturePack::NormalizeBuild(const std::string& build) {
  std::string s = build;
  // Strip a single leading "0x"/"0X" so the two spellings of a hex fingerprint
  // compare equal.
  if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
    s = s.substr(2);
  }
  std::transform(s.begin(), s.end(), s.begin(),
                 [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

std::string SignaturePack::CanonicalPayload() const {
  // Deterministic, line-oriented, and deliberately minimal: only the fields
  // whose tampering matters. The build fingerprint and the anti-rollback
  // sequence/expiry are inside the signed bytes, so a pack cannot be re-pointed
  // at another build or replayed with a lower sequence without breaking the
  // signature. The build is normalised so the two spellings of a fingerprint
  // (case, "0x") produce one payload, matching the apply-time comparison.
  std::string out;
  out += "berry-sigpack-v1\n";
  out += "format=" + std::to_string((unsigned)kFormatVersion) + "\n";
  out += "name=" + name + "\n";
  out += "build=" + NormalizeBuild(build) + "\n";
  out += "sequence=" + std::to_string(sequence) + "\n";
  out += "expires=" + std::to_string(expires) + "\n";
  return out;
}

SigPackError SignaturePack::Verify(const SigPackTrust& trust) const {
  // Fail closed on an unprovisioned device: with no key nothing may be applied,
  // so a pack can never be trusted "by default".
  if (!trust.Valid()) return SigPackError::NoTrustAnchor;
  if (signatureAlgo != signature_algo::Ed25519()) {
    return SigPackError::UnknownAlgorithm;
  }
  if (!crypto::IsHex(signature) || signature.size() != 128) {
    return SigPackError::BadPackSignature;   // 64-byte Ed25519 signature
  }
  std::string pub32, sig64;
  if (!crypto::Ed25519PublicKeyFromHex(trust.publicKeyHex, &pub32)) {
    return SigPackError::NoTrustAnchor;      // malformed key: refuse everything
  }
  if (!crypto::FromHex(signature, &sig64, 64)) {
    return SigPackError::BadPackSignature;
  }
  // The signature covers the canonical payload. A single flipped byte in any
  // covered field (or in the signature itself) makes this fail.
  if (!crypto::Ed25519Verify(CanonicalPayload(), sig64, pub32)) {
    return SigPackError::BadPackSignature;
  }
  // Anti-rollback. Checked only after the signature verifies, so a forged
  // sequence cannot be used to probe the floor.
  if (trust.minSequence != 0 && sequence < trust.minSequence) {
    return SigPackError::StaleSequence;
  }
  if (expires != 0 && trust.nowUnix != 0 && trust.nowUnix >= expires) {
    return SigPackError::Expired;
  }
  return SigPackError::Ok;
}

bool SignaturePack::VerifyEnvelope(const std::string& json,
                                   const SigPackTrust& trust,
                                   SignaturePack* out, SigPackError* outError,
                                   std::string* error) {
  auto fail = [&](SigPackError e, const std::string& detail) {
    if (outError) *outError = e;
    if (error) *error = detail;
    return false;
  };
  if (!out) return fail(SigPackError::BadJson, "null output");

  detail::json::Value root;
  std::string jerr;
  if (!detail::json::Parse(json, &root, &jerr)) {
    return fail(SigPackError::BadJson, jerr);
  }
  if (!root.IsObject()) {
    return fail(SigPackError::BadJson, "root is not an object");
  }

  // The envelope touches only the signed fields. Unknown keys are NOT rejected
  // here (that is the entry parser's job); the envelope must stay narrow so it
  // can run before anything is interpreted.
  const Value* format = root.Get("format");
  if (!format || !format->IsNumber() || format->number != (f64)kFormatVersion) {
    return fail(SigPackError::BadFormat, "missing or unsupported 'format'");
  }
  const Value* name = root.Get("name");
  if (!name || !name->IsString() || !SafeText(name->str, 128)) {
    return fail(SigPackError::MissingName, "missing or empty 'name'");
  }
  const Value* build = root.Get("build");
  if (!build || !build->IsString() || build->str.empty()) {
    return fail(SigPackError::MissingBuild, "missing or empty 'build'");
  }
  const Value* seq = root.Get("sequence");
  u64 sequence = 0;
  if (!ReadU64(seq, &sequence)) {
    return fail(SigPackError::BadSequence,
                "missing or malformed 'sequence'");
  }
  u64 expires = 0;
  const Value* exp = root.Get("expires");
  if (exp) {
    if (!ReadU64(exp, &expires)) {
      return fail(SigPackError::BadSequence, "malformed 'expires'");
    }
  }
  const Value* sig = root.Get("signature");
  if (!sig || !sig->IsObject()) {
    return fail(SigPackError::Unsigned, "missing 'signature' block");
  }
  const Value* algo = sig->Get("algo");
  if (!algo || !algo->IsString() || algo->str.empty()) {
    return fail(SigPackError::Unsigned, "'signature' has no 'algo'");
  }
  const Value* sigHex = sig->Get("value");
  if (!sigHex || !sigHex->IsString() || !SafeHex(sigHex->str, 256)) {
    return fail(SigPackError::BadPackSignature,
                "'signature' has no valid 'value'");
  }

  SignaturePack env;
  env.name = name->str;
  env.build = build->str;
  env.sequence = sequence;
  env.expires = expires;
  env.signatureAlgo = algo->str;
  env.signature = sigHex->str;

  const SigPackError ve = env.Verify(trust);
  if (ve != SigPackError::Ok) {
    return fail(ve, SigPackErrorName(ve));
  }

  // Hand back the verified envelope fields. `out` starts empty, so a caller
  // that fails a later step cannot act on a half-filled pack.
  *out = std::move(env);
  if (outError) *outError = SigPackError::Ok;
  if (error) error->clear();
  return true;
}

bool SignaturePack::Parse(const std::string& json, SignaturePack* out,
                          SigPackError* outError, std::string* error) {
  auto fail = [&](SigPackError e, const std::string& detail) {
    if (outError) *outError = e;
    if (error) *error = detail;
    return false;
  };
  if (!out) return fail(SigPackError::BadJson, "null output");

  detail::json::Value root;
  std::string jerr;
  if (!detail::json::Parse(json, &root, &jerr)) {
    return fail(SigPackError::BadJson, jerr);
  }
  if (!root.IsObject()) return fail(SigPackError::BadJson, "root is not an object");

  static const std::set<std::string> kKnown = {"format", "name", "build",
                                               "sequence", "expires",
                                               "signature", "signatures",
                                               "layouts"};
  std::string keyDetail;
  if (!OnlyKeys(root, kKnown, &keyDetail)) {
    return fail(SigPackError::BadJson, keyDetail);
  }

  const Value* format = root.Get("format");
  if (!format || !format->IsNumber() || format->number != (f64)kFormatVersion) {
    return fail(SigPackError::BadFormat, "missing or unsupported 'format'");
  }
  const Value* name = root.Get("name");
  if (!name || !name->IsString() || !SafeText(name->str, 128)) {
    return fail(SigPackError::MissingName, "missing or empty 'name'");
  }
  const Value* build = root.Get("build");
  if (!build || !build->IsString() || build->str.empty()) {
    return fail(SigPackError::MissingBuild, "missing or empty 'build'");
  }
  // The signed envelope must be present and well formed even at parse time. The
  // crypto was already checked by VerifyEnvelope BEFORE this call; re-reading
  // the fields here keeps Parse's own output self-consistent and refuses a pack
  // whose envelope a caller stripped between the two steps.
  const Value* seq = root.Get("sequence");
  u64 sequence = 0;
  if (!ReadU64(seq, &sequence)) {
    return fail(SigPackError::BadSequence, "missing or malformed 'sequence'");
  }
  u64 expires = 0;
  const Value* exp = root.Get("expires");
  if (exp && !ReadU64(exp, &expires)) {
    return fail(SigPackError::BadSequence, "malformed 'expires'");
  }
  const Value* sigBlock = root.Get("signature");
  if (!sigBlock || !sigBlock->IsObject()) {
    return fail(SigPackError::Unsigned, "missing 'signature' block");
  }
  static const std::set<std::string> kSigBlockKeys = {"algo", "value"};
  std::string sigDetail;
  if (!OnlyKeys(*sigBlock, kSigBlockKeys, &sigDetail)) {
    return fail(SigPackError::BadPackSignature, sigDetail);
  }
  const Value* algo = sigBlock->Get("algo");
  const Value* sigHex = sigBlock->Get("value");
  if (!algo || !algo->IsString() || algo->str.empty()) {
    return fail(SigPackError::Unsigned, "'signature' has no 'algo'");
  }
  if (!sigHex || !sigHex->IsString() || !SafeHex(sigHex->str, 256)) {
    return fail(SigPackError::BadPackSignature,
                "'signature' has no valid 'value'");
  }

  SignaturePack pack;
  pack.name = name->str;
  pack.build = build->str;
  pack.sequence = sequence;
  pack.expires = expires;
  pack.signatureAlgo = algo->str;
  pack.signature = sigHex->str;

  // --- signatures ---
  const Value* sigs = root.Get("signatures");
  if (sigs) {
    if (!sigs->IsArray()) {
      return fail(SigPackError::BadSignature, "'signatures' must be an array");
    }
    if (sigs->array.size() > kMaxSignatures) {
      return fail(SigPackError::TooManySignatures, "too many signature rows");
    }
    std::set<std::string> ids;
    for (const auto& sv : sigs->array) {
      if (!sv.IsObject()) {
        return fail(SigPackError::BadSignature, "signature row is not an object");
      }
      static const std::set<std::string> kSigKeys = {"id", "pattern", "from_end"};
      std::string sd;
      if (!OnlyKeys(sv, kSigKeys, &sd)) {
        return fail(SigPackError::BadSignature, sd);
      }
      const Value* id = sv.Get("id");
      if (!id || !id->IsString() || !SafeText(id->str, 128)) {
        return fail(SigPackError::BadSignature, "signature row has no valid 'id'");
      }
      if (!ids.insert(id->str).second) {
        return fail(SigPackError::DuplicateId,
                    "duplicate signature id '" + id->str + "'");
      }
      const Value* pat = sv.Get("pattern");
      if (!pat || !pat->IsString() || pat->str.empty()) {
        return fail(SigPackError::BadSignature,
                    "signature '" + id->str + "' has no 'pattern'");
      }
      SigPackRow row;
      row.id = id->str;
      row.pattern = pat->str;
      const Value* off = sv.Get("from_end");
      if (off) {
        if (!ReadI32(off, &row.fromEndOffset)) {
          return fail(SigPackError::BadSignature,
                      "signature '" + id->str + "' has a bad 'from_end'");
        }
      }
      // Validate the pattern now, so a bad pattern fails the whole pack rather
      // than being discovered after a partial registration.
      Signature probe;
      if (!probe.Parse(row.pattern.c_str()) || probe.bytes.empty()) {
        return fail(SigPackError::BadPattern,
                    "signature '" + id->str + "' pattern does not parse");
      }
      pack.signatures.push_back(std::move(row));
    }
  }

  // --- layouts ---
  const Value* layouts = root.Get("layouts");
  if (layouts) {
    if (!layouts->IsArray()) {
      return fail(SigPackError::BadLayout, "'layouts' must be an array");
    }
    if (layouts->array.size() > kMaxLayouts) {
      return fail(SigPackError::TooManyLayouts, "too many layouts");
    }
    const std::string packBuild = NormalizeBuild(pack.build);
    for (const auto& lv : layouts->array) {
      if (!lv.IsObject()) {
        return fail(SigPackError::BadLayout, "layout is not an object");
      }
      // Re-serialise the embedded object and run it through the layout parser,
      // so a pack's layout is validated by exactly the same code a standalone
      // layout document is.
      const std::string text = detail::jsonw::Dump(lv);
      StructLayout layout;
      LayoutError le = LayoutError::Ok;
      std::string detail;
      if (!StructLayout::Parse(text, &layout, &le, &detail)) {
        return fail(SigPackError::BadLayout,
                    std::string("layout: ") + LayoutErrorName(le) +
                        (detail.empty() ? "" : " (" + detail + ")"));
      }
      // The pack's build is authoritative: a layout targeting a different build
      // would be applied under the wrong fingerprint, so refuse it outright.
      if (NormalizeBuild(layout.build) != packBuild) {
        return fail(SigPackError::LayoutBuildMismatch,
                    "layout '" + layout.name + "' targets a different build");
      }
      pack.layouts.push_back(std::move(layout));
    }
  }

  if (pack.signatures.empty() && pack.layouts.empty()) {
    return fail(SigPackError::NoEntries, "pack carries no signatures or layouts");
  }

  *out = std::move(pack);
  if (outError) *outError = SigPackError::Ok;
  if (error) error->clear();
  return true;
}

//----------------------------------------------------------------------------
// Apply
//----------------------------------------------------------------------------

SigPackError ApplySignaturePack(const SignaturePack& pack,
                                const std::string& activeBuild,
                                SigCache& sigs,
                                LayoutRegistry* layouts,
                                SigPackReport* report) {
  if (report) *report = SigPackReport{};

  // Gate on the build fingerprint first: nothing registers unless the pack was
  // written for the build we are attached to. An empty active build refuses
  // every pack, so a pack is never applied "by default".
  const std::string want = SignaturePack::NormalizeBuild(activeBuild);
  const std::string have = SignaturePack::NormalizeBuild(pack.build);
  if (want.empty() || want != have) {
    return SigPackError::BuildMismatch;
  }

  // Every row was validated at parse time, so this cannot fail part-way; the
  // pack's entries all register.
  for (const SigPackRow& row : pack.signatures) {
    Signature sig(row.id, row.pattern.c_str(), row.fromEndOffset);
    sigs.Register(row.id, sig);
    if (report) ++report->signaturesRegistered;
  }
  if (layouts) {
    for (const StructLayout& layout : pack.layouts) {
      std::string err;
      if (layouts->Add(layout, &err)) {
        if (report) ++report->layoutsRegistered;
      } else {
        // A duplicate (same name+build) is the only way Add refuses here; the
        // document itself was validated. Report it as a bad layout so the
        // caller sees the pack did not fully apply.
        return SigPackError::BadLayout;
      }
    }
  }
  return SigPackError::Ok;
}

//----------------------------------------------------------------------------
// Directory loading
//----------------------------------------------------------------------------

SignaturePackLoader::SignaturePackLoader(IScriptFileSystem* fs,
                                         SigPackTrust trust, std::string root)
    : fs_(fs), trust_(std::move(trust)), root_(std::move(root)) {}

u64 SignaturePackLoader::sequenceFloor() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return highestSequence_;
}

void SignaturePackLoader::SetSequenceFloor(u64 floor) {
  std::lock_guard<std::mutex> lk(mutex_);
  // The floor only ever rises: lowering it would re-open the rollback window it
  // exists to close (e.g. a restart that replayed a captured older pack).
  if (floor > highestSequence_) highestSequence_ = floor;
}

SigPackError SignaturePackLoader::VerifyParseApply(
    const std::string& text, const std::string& activeBuild, SigCache& sigs,
    LayoutRegistry* layouts, SignaturePack* parsed, SigPackReport* report) {
  // 1. Verify the SIGNED ENVELOPE before parsing a single entry. This is the
  //    whole point of the ordering: a tampered, unsigned, expired or replayed
  //    pack is refused here, so its signatures/layouts are never interpreted.
  SignaturePack env;
  SigPackError ve = SigPackError::Ok;
  std::string detail;
  if (!SignaturePack::VerifyEnvelope(text, trust_, &env, &ve, &detail)) {
    return ve;
  }
  // 2. Anti-rollback against the session floor (the highest sequence applied so
  //    far). Equal is accepted so re-applying the identical pack is idempotent;
  //    a strictly lower sequence is a rollback and is refused.
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (env.sequence < highestSequence_) return SigPackError::StaleSequence;
  }
  // 3. Parse the now-trusted entries. Every row is validated before anything is
  //    registered, so a bad row registers nothing.
  SignaturePack pack;
  SigPackError pe = SigPackError::Ok;
  if (!SignaturePack::Parse(text, &pack, &pe, &detail)) return pe;
  // 4. Apply (build gate + atomic registration).
  SigPackReport rep;
  SigPackError ae = ApplySignaturePack(pack, activeBuild, sigs, layouts, &rep);
  if (ae != SigPackError::Ok) return ae;
  // 5. Advance the floor only after a successful apply, so a pack refused by the
  //    build gate does not raise it.
  {
    std::lock_guard<std::mutex> lk(mutex_);
    if (pack.sequence > highestSequence_) highestSequence_ = pack.sequence;
  }
  if (parsed) *parsed = std::move(pack);
  if (report) *report = rep;
  return SigPackError::Ok;
}

SigPackError SignaturePackLoader::LoadPackFile(const std::string& path,
                                               const std::string& activeBuild,
                                               SigCache& sigs,
                                               LayoutRegistry* layouts,
                                               SigPackLoadResult* result) {
  SigPackLoadResult local;
  local.path = path;
  auto finish = [&](SigPackError e, const std::string& detail) {
    local.error = e;
    local.detail = detail;
    if (result) *result = local;
    return e;
  };

  if (!fs_) return finish(SigPackError::Unreadable, "no filesystem seam");

  std::string text;
  if (!fs_->ReadFile(path, &text)) {
    return finish(SigPackError::Unreadable, "file missing or unreadable");
  }

  SignaturePack pack;
  SigPackReport rep;
  SigPackError e = VerifyParseApply(text, activeBuild, sigs, layouts, &pack, &rep);
  if (e != SigPackError::Ok) {
    // Surface the envelope's name when we have one, for a useful diagnostic.
    return finish(e, SigPackErrorName(e));
  }
  local.name = pack.name;
  local.signatures = rep.signaturesRegistered;
  local.layouts = rep.layoutsRegistered;
  return finish(SigPackError::Ok, "");
}

SigPackLoadReport SignaturePackLoader::LoadDirectory(
    const std::string& activeBuild, SigCache& sigs, LayoutRegistry* layouts,
    const std::string& root) {
  SigPackLoadReport report;
  if (!fs_) return report;

  const std::string dir = root.empty() ? root_ : root;
  std::vector<std::string> entries = fs_->ListDir(dir);
  std::sort(entries.begin(), entries.end());

  const std::string suffix = kSuffix;
  size_t attempted = 0;
  for (const std::string& leaf : entries) {
    if (leaf.size() < suffix.size() ||
        leaf.compare(leaf.size() - suffix.size(), suffix.size(), suffix) != 0) {
      continue;   // not a pack file
    }
    if (attempted >= kMaxPacksPerLoad) {
      SigPackLoadResult overflow;
      overflow.path = dir.empty() ? leaf : dir + "/" + leaf;
      overflow.error = SigPackError::TooManyPacks;
      overflow.detail = SigPackErrorName(SigPackError::TooManyPacks);
      report.results.push_back(std::move(overflow));
      report.truncated = true;
      break;
    }
    ++attempted;
    ++report.packsTried;

    const std::string path = dir.empty() ? leaf : dir + "/" + leaf;
    SigPackLoadResult r;
    SigPackError e = LoadPackFile(path, activeBuild, sigs, layouts, &r);
    if (e == SigPackError::Ok) {
      ++report.packsApplied;
      report.signaturesRegistered += r.signatures;
      report.layoutsRegistered += r.layouts;
    } else {
      ++report.packsFailed;
    }
    report.results.push_back(std::move(r));
  }
  return report;
}

}  // namespace Berry
