// Local cosmetic model packs. See include/berry/pack.h for the rules.
//
// The whole document is validated before anything is registered, and
// registration goes through CosmeticRegistry::RegisterAll so a pack is applied
// atomically. A single bad asset therefore contributes nothing rather than
// leaving the catalog half-updated.
#include "berry/pack.h"

#include "berry/logger.h"
#include "json_min.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <set>

namespace Berry {

namespace {

using detail::json::Value;
using detail::json::Parse;

/// Asset ids are embedded in the loadout's canonical text (one "slot=id" line
/// each), so an id carrying a newline could forge another line. Names appear in
/// logs and overlays. Both are kept printable, non-empty and bounded.
bool SafeText(const std::string& s, size_t maxLen) {
  if (s.empty() || s.size() > maxLen) return false;
  for (char ch : s) {
    const unsigned char c = (unsigned char)ch;
    if (c < 0x20 || c == 0x7f) return false;
  }
  return true;
}

/// Reads a finite number. A non-number or a non-finite value is an error, never
/// a silent zero: a pack that says "scale": "big" must not load.
bool ReadNumber(const Value& obj, const char* key, f64* out) {
  const Value* v = obj.Get(key);
  if (!v) return false;
  if (!v->IsNumber() || !std::isfinite(v->number)) return false;
  *out = v->number;
  return true;
}

/// Reads a non-negative integer id (geometry/texture handles) bounded to the
/// exactly-representable f64 range, so the cast is lossless.
bool ReadHandle(const Value& obj, const char* key, u64* out) {
  const Value* v = obj.Get(key);
  if (!v) return true;   // absent: leave the default (0)
  if (!v->IsNumber() || !std::isfinite(v->number)) return false;
  if (v->number < 0 || v->number > 9007199254740992.0) return false;  // 2^53
  *out = (u64)v->number;
  return true;
}

/// Reads a strict 3-number object with exactly the named keys.
bool ReadVec3(const Value& obj, const char* key, const char* k0, const char* k1,
              const char* k2, Vec3* out) {
  const Value* v = obj.Get(key);
  if (!v) return true;   // absent: leave the default
  if (!v->IsObject() || v->object.size() != 3) return false;
  f64 a = 0, b = 0, c = 0;
  if (!ReadNumber(*v, k0, &a) || !ReadNumber(*v, k1, &b) ||
      !ReadNumber(*v, k2, &c)) {
    return false;
  }
  *out = Vec3((f32)a, (f32)b, (f32)c);
  return true;
}

/// Rejects any key that is not in `allowed`. Strict on purpose: a typo like
/// "offest" must fail loudly rather than load an asset at the origin.
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

bool ReadTags(const Value& obj, std::vector<std::string>* out,
              std::string* detail) {
  const Value* v = obj.Get("tags");
  if (!v) return true;
  if (!v->IsArray()) {
    if (detail) *detail = "'tags' must be an array of strings";
    return false;
  }
  for (const auto& t : v->array) {
    if (!t.IsString() || !SafeText(t.str, 64)) {
      if (detail) *detail = "bad tag";
      return false;
    }
    out->push_back(t.str);
  }
  return true;
}

const std::set<std::string>& PackKeys() {
  static const std::set<std::string> k = {"format", "name", "author", "assets"};
  return k;
}

const std::set<std::string>& AssetKeys() {
  static const std::set<std::string> k = {
      "id", "slot", "bone", "geometry", "texture", "offset",
      "rotation", "scale", "tint", "trail", "animation", "animation_speed",
      "pack", "author", "tags"};
  return k;
}

}  // namespace

const char* PackErrorName(PackError e) {
  switch (e) {
    case PackError::Ok: return "ok";
    case PackError::Unreadable: return "pack unreadable";
    case PackError::BadJson: return "invalid JSON";
    case PackError::BadFormat: return "unsupported pack format";
    case PackError::MissingName: return "pack has no name";
    case PackError::NoAssets: return "pack has no assets";
    case PackError::TooManyAssets: return "too many assets in pack";
    case PackError::TooManyPacks: return "too many pack files";
    case PackError::BadAsset: return "invalid asset";
    case PackError::DuplicateAsset: return "duplicate asset id";
    case PackError::RegistryFull: return "cosmetic registry full";
  }
  return "unknown";
}

//----------------------------------------------------------------------------
// Parsing
//----------------------------------------------------------------------------

PackError CosmeticPackLoader::ParsePack(const std::string& text,
                                        std::vector<CosmeticAsset>* out,
                                        std::string* packName,
                                        std::string* detail) {
  if (out) out->clear();
  if (packName) packName->clear();
  if (detail) detail->clear();
  auto fail = [detail](PackError e, const std::string& msg) {
    if (detail) *detail = msg;
    return e;
  };

  Value doc;
  std::string jerr;
  if (!Parse(text, &doc, &jerr)) {
    return fail(PackError::BadJson, jerr);
  }
  if (!doc.IsObject()) {
    return fail(PackError::BadJson, "pack must be a JSON object");
  }
  std::string keyDetail;
  if (!OnlyKeys(doc, PackKeys(), &keyDetail)) {
    return fail(PackError::BadAsset, keyDetail);
  }

  // format: required and exact. A future version must not be read by a build
  // that does not know its fields.
  f64 format = 0;
  if (!ReadNumber(doc, "format", &format)) {
    return fail(PackError::BadFormat, "'format' must be a number");
  }
  if (format != (f64)kFormatVersion) {
    return fail(PackError::BadFormat,
                "unsupported format " + std::to_string((int)format));
  }

  const Value* name = doc.Get("name");
  if (!name || !name->IsString() || !SafeText(name->str, 128)) {
    return fail(PackError::MissingName, "'name' must be a non-empty string");
  }
  const std::string pack = name->str;

  std::string packAuthor;
  if (const Value* a = doc.Get("author")) {
    if (!a->IsString() || !SafeText(a->str, 128)) {
      return fail(PackError::BadAsset, "'author' must be a string");
    }
    packAuthor = a->str;
  }

  const Value* assets = doc.Get("assets");
  if (!assets || !assets->IsArray() || assets->array.empty()) {
    return fail(PackError::NoAssets, "'assets' must be a non-empty array");
  }
  if (assets->array.size() > kMaxAssetsPerPack) {
    return fail(PackError::TooManyAssets,
                "pack carries more than " + std::to_string(kMaxAssetsPerPack) +
                    " assets");
  }

  std::vector<CosmeticAsset> parsed;
  parsed.reserve(assets->array.size());
  std::set<std::string> ids;
  for (const Value& item : assets->array) {
    if (!item.IsObject()) {
      return fail(PackError::BadAsset, "each asset must be an object");
    }
    std::string adetail;
    if (!OnlyKeys(item, AssetKeys(), &adetail)) {
      return fail(PackError::BadAsset, adetail);
    }

    CosmeticAsset asset;
    const Value* id = item.Get("id");
    if (!id || !id->IsString() || !SafeText(id->str, 128)) {
      return fail(PackError::BadAsset, "'id' must be a non-empty string");
    }
    asset.id = id->str;

    const Value* slot = item.Get("slot");
    if (!slot || !slot->IsString()) {
      return fail(PackError::BadAsset, "asset '" + asset.id + "' has no slot");
    }
    asset.slot = CosmeticSlotFromName(slot->str);
    if (asset.slot == CosmeticSlot::None) {
      return fail(PackError::BadAsset,
                  "asset '" + asset.id + "' has unknown slot '" + slot->str + "'");
    }

    if (const Value* bone = item.Get("bone")) {
      if (!bone->IsString()) {
        return fail(PackError::BadAsset, "asset '" + asset.id + "' has bad bone");
      }
      if (bone->str != "none") {
        asset.bone = BoneFromName(bone->str);
        if (asset.bone == Bone::None) {
          return fail(PackError::BadAsset, "asset '" + asset.id +
                                               "' has unknown bone '" +
                                               bone->str + "'");
        }
      }
    }

    if (!ReadHandle(item, "geometry", &asset.geometry) ||
        !ReadHandle(item, "texture", &asset.texture)) {
      return fail(PackError::BadAsset,
                  "asset '" + asset.id + "' has a bad geometry/texture id");
    }
    if (!ReadVec3(item, "offset", "x", "y", "z", &asset.offset) ||
        !ReadVec3(item, "rotation", "x", "y", "z", &asset.rotation)) {
      return fail(PackError::BadAsset,
                  "asset '" + asset.id + "' has a bad offset/rotation");
    }
    if (const Value* scale = item.Get("scale")) {
      if (!scale->IsNumber() || !std::isfinite(scale->number) ||
          scale->number <= 0) {
        return fail(PackError::BadAsset,
                    "asset '" + asset.id + "' has a bad scale");
      }
      asset.scale = (f32)scale->number;
    }
    if (!ReadVec3(item, "tint", "r", "g", "b", &asset.tint)) {
      return fail(PackError::BadAsset, "asset '" + asset.id + "' has a bad tint");
    }
    if (const Value* trail = item.Get("trail")) {
      if (!trail->IsNumber() || !std::isfinite(trail->number) ||
          trail->number < 0 || trail->number > 1) {
        return fail(PackError::BadAsset,
                    "asset '" + asset.id + "' has a bad trail (0..1)");
      }
      asset.trail = (f32)trail->number;
    }
    if (!ReadTags(item, &asset.tags, &adetail)) {
      return fail(PackError::BadAsset,
                  "asset '" + asset.id + "': " + adetail);
    }
    // A custom-animation clip name (bounded) and its playback rate. The clip
    // itself is authored in the script layer (Animator) or another pack; the
    // asset only names it, so a missing clip fails closed to a rigid placement.
    if (const Value* an = item.Get("animation")) {
      if (!an->IsString() || !SafeText(an->str, 64)) {
        return fail(PackError::BadAsset,
                    "asset '" + asset.id + "' has a bad animation name");
      }
      asset.animation = an->str;
    }
    if (const Value* sp = item.Get("animation_speed")) {
      if (!sp->IsNumber() || !std::isfinite(sp->number) || sp->number <= 0) {
        return fail(PackError::BadAsset,
                    "asset '" + asset.id + "' has a bad animation_speed");
      }
      asset.animationSpeed = (f32)sp->number;
    }

    // Provenance defaults to the pack's own identity; an asset may name its own.
    asset.pack = pack;
    if (const Value* ap = item.Get("pack")) {
      if (!ap->IsString() || !SafeText(ap->str, 128)) {
        return fail(PackError::BadAsset, "asset '" + asset.id + "' has bad pack");
      }
      asset.pack = ap->str;
    }
    asset.author = packAuthor;
    if (const Value* aa = item.Get("author")) {
      if (!aa->IsString() || !SafeText(aa->str, 128)) {
        return fail(PackError::BadAsset, "asset '" + asset.id + "' has bad author");
      }
      asset.author = aa->str;
    }

    // Same validation the registry applies, so a ParsePack success means the
    // assets are registrable.
    CosmeticError cerr = CosmeticError::Ok;
    if (!ValidCosmeticTransform(asset, &cerr)) {
      return fail(PackError::BadAsset, "asset '" + asset.id + "': " +
                                           CosmeticErrorName(cerr));
    }
    if (!ids.insert(asset.id).second) {
      return fail(PackError::DuplicateAsset, "asset id '" + asset.id + "' repeats");
    }
    parsed.push_back(std::move(asset));
  }

  if (out) *out = std::move(parsed);
  if (packName) *packName = pack;
  return PackError::Ok;
}

//----------------------------------------------------------------------------
// Loader
//----------------------------------------------------------------------------

CosmeticPackLoader::CosmeticPackLoader(IScriptFileSystem* fs, std::string root)
    : fs_(fs), root_(std::move(root)) {}

PackError CosmeticPackLoader::LoadPackText(const std::string& text,
                                           const std::string& path,
                                           std::string* name,
                                           size_t* registered,
                                           std::string* detail) {
  if (registered) *registered = 0;
  if (detail) detail->clear();
  std::vector<CosmeticAsset> assets;
  std::string packName;
  std::string reason;
  const PackError err = ParsePack(text, &assets, &packName, &reason);
  if (name) *name = packName;
  if (err != PackError::Ok) {
    if (detail) *detail = reason;
    Log::Warn("pack '%s': %s (%s)", path.c_str(), PackErrorName(err),
              reason.c_str());
    return err;
  }

  CosmeticError cerr = CosmeticError::Ok;
  if (!CosmeticRegistry::Instance().RegisterAll(assets, &cerr)) {
    // A duplicate against the existing catalog is reported as such; anything
    // else (a full registry) maps to the matching pack error.
    const PackError mapped = (cerr == CosmeticError::DuplicateId)
                                 ? PackError::DuplicateAsset
                                 : PackError::RegistryFull;
    if (detail) *detail = CosmeticErrorName(cerr);
    Log::Warn("pack '%s': %s (%s)", path.c_str(), PackErrorName(mapped),
              CosmeticErrorName(cerr));
    return mapped;
  }
  if (registered) *registered = assets.size();
  return PackError::Ok;
}

PackError CosmeticPackLoader::LoadPackFile(const std::string& path,
                                           std::string* name,
                                           size_t* registered,
                                           std::string* detail) {
  if (registered) *registered = 0;
  std::string text;
  if (!fs_ || !fs_->ReadFile(path, &text)) {
    if (name) name->clear();
    if (detail) *detail = "cannot read file";
    return PackError::Unreadable;
  }
  return LoadPackText(text, path, name, registered, detail);
}

namespace {
std::string JoinLeaf(const std::string& dir, const std::string& leaf) {
  if (dir.empty()) return leaf;
  if (dir.back() == '/') return dir + leaf;
  return dir + "/" + leaf;
}
}  // namespace

PackLoadReport CosmeticPackLoader::LoadDirectory(const std::string& root) {
  const std::string dir = root.empty() ? root_ : root;
  PackLoadReport report;
  if (!fs_) return report;

  std::vector<std::string> names = fs_->ListDir(dir);
  std::sort(names.begin(), names.end());
  const std::string suffix = kSuffix;
  for (const auto& name : names) {
    if (name.size() <= suffix.size()) continue;
    if (name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) {
      continue;   // not a pack file
    }
    if (report.packsTried >= kMaxPacksPerLoad) {
      // Stop, but make the truncation visible in the report rather than only in
      // the log, so a caller (or overlay) can tell "no packs" from "too many".
      Log::Warn("pack directory '%s': more than %zu pack files; extra ignored",
                dir.c_str(), kMaxPacksPerLoad);
      report.truncated = true;
      PackLoadResult skipped;
      skipped.path = JoinLeaf(dir, name);
      skipped.error = PackError::TooManyPacks;
      report.results.push_back(std::move(skipped));
      break;
    }
    ++report.packsTried;
    PackLoadResult result;
    result.path = JoinLeaf(dir, name);
    result.error = LoadPackFile(result.path, &result.name, &result.assets,
                                &result.detail);
    if (result.error == PackError::Ok) {
      ++report.packsLoaded;
      report.assetsRegistered += result.assets;
    } else {
      ++report.packsFailed;
      result.assets = 0;
    }
    report.results.push_back(std::move(result));
  }
  return report;
}

}  // namespace Berry
