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
                                               "signatures", "layouts"};
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

  SignaturePack pack;
  pack.name = name->str;
  pack.build = build->str;

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

SignaturePackLoader::SignaturePackLoader(IScriptFileSystem* fs, std::string root)
    : fs_(fs), root_(std::move(root)) {}

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
  SigPackError pe = SigPackError::Ok;
  std::string detail;
  if (!SignaturePack::Parse(text, &pack, &pe, &detail)) {
    return finish(pe, detail);
  }
  local.name = pack.name;

  SigPackReport rep;
  SigPackError ae = ApplySignaturePack(pack, activeBuild, sigs, layouts, &rep);
  if (ae != SigPackError::Ok) {
    return finish(ae, SigPackErrorName(ae));
  }
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
