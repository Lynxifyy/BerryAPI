#pragma once
// Local cosmetic model packs: cosmetic assets described as data, not code.
//
// A "model pack" is a JSON document that lists cosmetic assets (a cape, a pair
// of wings, a hat) with the geometry/texture handles, slot, bone and attach
// transform the renderer needs. It is the authoring half of the cosmetics
// system: a creator writes a pack, drops it in the pack directory, and the
// assets appear in the registry without a new client build — the same idea as
// the OTA signature packs, scoped to cosmetics.
//
// Reading is done through the loader's `IScriptFileSystem` seam, never the
// host filesystem directly, so a device build can route reads through the
// game's own file layer and the host tests need no disk.
//
// Fail closed, wholesale: a pack that is malformed, names an unknown key, or
// carries a single bad asset registers NOTHING. A half-loaded pack would leave
// the catalog in a state no author wrote, so the loader validates the whole
// document, then registers every asset in one atomic step.

#include "berry/cosmetic.h"
#include "berry/export.h"
#include "berry/loader.h"

#include <string>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Failures.
//----------------------------------------------------------------------------

/// Why a pack was refused. Every one of these leaves the registry unchanged.
enum class PackError : u32 {
  Ok = 0,
  Unreadable,       // file missing or unreadable
  BadJson,          // not a well-formed JSON document
  BadFormat,        // absent or unsupported `format` version
  MissingName,      // absent or empty pack `name`
  NoAssets,         // absent or empty `assets` array
  TooManyAssets,    // more assets than a pack may carry
  TooManyPacks,     // more pack files than a directory load may read
  BadAsset,         // an asset names an unknown key / slot / bone or a bad value
  DuplicateAsset,   // an asset id repeats within the pack or in the registry
  RegistryFull,     // registering would exceed the catalog cap
};

const char* PackErrorName(PackError e);

//----------------------------------------------------------------------------
// Result.
//----------------------------------------------------------------------------

/// One pack's outcome. `assets` is how many assets it contributed (0 on any
/// failure). `detail` is a human-readable reason, for a log line or an overlay.
struct BERRY_API PackLoadResult {
  std::string path;
  std::string name;
  PackError error = PackError::Ok;
  size_t assets = 0;
  std::string detail;
};

/// The aggregate of a directory load. `results` holds one entry per pack that
/// was actually attempted; when the directory held more pack files than
/// `CosmeticPackLoader::kMaxPacksPerLoad`, the first one not attempted is
/// appended with `PackError::TooManyPacks` and `truncated` is set.
struct BERRY_API PackLoadReport {
  size_t packsTried = 0;
  size_t packsLoaded = 0;
  size_t packsFailed = 0;
  size_t assetsRegistered = 0;
  bool truncated = false;
  std::vector<PackLoadResult> results;
};

//----------------------------------------------------------------------------
// Loader.
//----------------------------------------------------------------------------

/// Parses and loads model packs into `CosmeticRegistry::Instance()`.
///
/// Thread-safe against the registry (registration is atomic there); the loader
/// itself holds no mutable state beyond the seam and the root directory, so it
/// is safe to share once constructed.
class BERRY_API CosmeticPackLoader {
 public:
  /// The suffix a discoverable pack file must carry. `.json` alone is not
  /// enough: the pack directory may hold unrelated JSON, and silently picking
  /// it up would register assets no author intended.
  static constexpr const char* kSuffix = ".pack.json";
  /// The only pack format this build understands. A pack that declares any
  /// other version is refused rather than best-effort parsed.
  static constexpr u32 kFormatVersion = 1;
  /// Caps so a hostile or runaway pack directory cannot exhaust memory.
  static constexpr size_t kMaxPacksPerLoad = 256;
  static constexpr size_t kMaxAssetsPerPack = 512;

  /// `fs` is borrowed and must outlive the loader. `root` is the directory
  /// `LoadDirectory()` reads when called with no argument.
  explicit CosmeticPackLoader(IScriptFileSystem* fs, std::string root = "");

  CosmeticPackLoader(const CosmeticPackLoader&) = delete;
  CosmeticPackLoader& operator=(const CosmeticPackLoader&) = delete;

  /// Parses and validates one pack document WITHOUT touching the registry.
  /// On success `out` holds the assets in document order and `packName` the
  /// pack's name. Pure, so tests can exercise the format independently of the
  /// catalog. Returns the first error found (fail closed on the whole pack).
  static PackError ParsePack(const std::string& text,
                             std::vector<CosmeticAsset>* out,
                             std::string* packName = nullptr,
                             std::string* detail = nullptr);

  /// Parses `text` and registers its assets atomically. Either every asset
  /// registers or none does. `registered` receives the count; `detail` receives
  /// a human-readable reason on failure.
  PackError LoadPackText(const std::string& text, const std::string& path,
                         std::string* name = nullptr,
                         size_t* registered = nullptr,
                         std::string* detail = nullptr);

  /// Reads and loads a single pack file through the seam.
  PackError LoadPackFile(const std::string& path, std::string* name = nullptr,
                         size_t* registered = nullptr,
                         std::string* detail = nullptr);

  /// Loads every `*.pack.json` directly under `root` (or the configured root
  /// when `root` is empty). Returns a per-pack report; a failed pack does not
  /// stop the others.
  PackLoadReport LoadDirectory(const std::string& root = "");

  const std::string& root() const { return root_; }
  void SetRoot(std::string root) { root_ = std::move(root); }

 private:
  IScriptFileSystem* fs_ = nullptr;
  std::string root_;
};

}  // namespace Berry
