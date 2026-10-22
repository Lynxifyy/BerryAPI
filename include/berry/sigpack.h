#pragma once
// OTA signature packs: signatures and struct layouts described as data.
//
// A signature table is written against one concrete client build. When the game
// updates, every offset in that table is stale, and the usual answer is a new
// client build. This turns that into a data update instead: a pack is a JSON
// document that carries the signature rows and the struct layouts for a build,
// and applying it registers both into the existing registries. A game update
// then means shipping a new pack file, not recompiling the library.
//
// The pack is gated by the build fingerprint, exactly like the tables it
// replaces:
//
//   * the pack names the build it was written against (`build`), and it is
//     applied only when that equals the active fingerprint. A pack for another
//     build is refused wholesale, so a stale pack cannot silently resolve to
//     addresses in a foreign build — which would be worse than resolving
//     nothing.
//   * every signature pattern is parsed and every layout validated BEFORE
//     anything is registered, so a pack with one bad row registers nothing.
//     The registries are never left half-updated.
//   * a pack that is malformed, names an unknown key, or exceeds a cap is
//     refused as a whole document.
//
// Reading is done through the loader's `IScriptFileSystem` seam, never the host
// filesystem directly, so a device build routes reads through the game's file
// layer and the host tests need no disk.

#include "berry/export.h"
#include "berry/layout.h"
#include "berry/loader.h"
#include "berry/sigscan.h"
#include "berry/types.h"

#include <string>
#include <vector>

namespace Berry {

/// Why a pack was refused. Every one of these leaves the registries unchanged.
enum class SigPackError : u32 {
  Ok = 0,
  Unreadable,        // file missing or unreadable
  BadJson,           // not a well-formed JSON document, or an unknown key
  BadFormat,         // absent or unsupported `format` version
  MissingName,       // absent or empty pack `name`
  MissingBuild,      // absent or empty `build` fingerprint
  NoEntries,         // neither signatures nor layouts
  TooManySignatures, // more signature rows than a pack may carry
  TooManyLayouts,    // more layouts than a pack may carry
  BadSignature,      // a signature row is malformed
  BadPattern,        // a signature pattern does not parse
  DuplicateId,       // a signature id repeats within the pack
  BadLayout,         // a layout document does not parse or validate
  LayoutBuildMismatch,  // a layout targets a build other than the pack's
  BuildMismatch,     // the pack's build does not match the active fingerprint
  TooManyPacks,      // more pack files than a directory load may read
};

const char* SigPackErrorName(SigPackError e);

/// One signature row: an id, a hex pattern, and the optional from-end offset.
struct BERRY_API SigPackRow {
  std::string id;
  std::string pattern;
  i32 fromEndOffset = 0;
};

/// A parsed, validated signature pack. Nothing is registered until `Apply`.
struct BERRY_API SignaturePack {
  /// The only pack format this build understands. A pack that declares any
  /// other version is refused rather than best-effort parsed.
  static constexpr u32 kFormatVersion = 1;
  static constexpr size_t kMaxSignatures = 4096;
  static constexpr size_t kMaxLayouts = 256;

  std::string name;
  std::string build;   // the build fingerprint this pack was written against
  std::vector<SigPackRow> signatures;
  std::vector<StructLayout> layouts;

  /// Parses and fully validates a pack document. Rejects an unknown key, a
  /// duplicate signature id, an unparseable pattern, a bad layout, a layout
  /// whose build differs from the pack's, and an over-cap pack — the whole
  /// document, or nothing.
  static bool Parse(const std::string& json, SignaturePack* out,
                    SigPackError* outError = nullptr,
                    std::string* error = nullptr);

  /// Normalises a build fingerprint for comparison: lower-cased, with an
  /// optional "0x" prefix stripped. Two fingerprints that differ only in case
  /// or prefix compare equal.
  static std::string NormalizeBuild(const std::string& build);
};

/// What an `Apply` did. All counts are 0 when the pack was refused.
struct BERRY_API SigPackReport {
  size_t signaturesRegistered = 0;
  size_t layoutsRegistered = 0;
};

/// Applies a parsed pack to the registries.
///
/// Refused (returning `BuildMismatch` and registering nothing) when the pack's
/// `build` does not equal `activeBuild`. An empty `activeBuild` refuses every
/// pack: without a fingerprint nothing may resolve, so a pack cannot be applied
/// "by default".
///
/// `sigs` is the signature cache the rows register into; `layouts` is optional
/// and may be null when the pack carries no layouts. Registration is atomic per
/// pack: every entry validated at parse time, so this only fails on the build
/// gate.
BERRY_API SigPackError ApplySignaturePack(const SignaturePack& pack,
                                          const std::string& activeBuild,
                                          SigCache& sigs,
                                          LayoutRegistry* layouts = nullptr,
                                          SigPackReport* report = nullptr);

//----------------------------------------------------------------------------
// Directory loading.
//----------------------------------------------------------------------------

/// One pack's outcome. `signatures`/`layouts` are what it contributed (0 on any
/// failure). `detail` is a human-readable reason.
struct BERRY_API SigPackLoadResult {
  std::string path;
  std::string name;
  SigPackError error = SigPackError::Ok;
  size_t signatures = 0;
  size_t layouts = 0;
  std::string detail;
};

/// The aggregate of a directory load. `results` holds one entry per pack that
/// was actually attempted; when the directory held more pack files than
/// `SignaturePackLoader::kMaxPacksPerLoad`, the first one not attempted is
/// appended with `SigPackError::TooManyPacks` and `truncated` is set.
struct BERRY_API SigPackLoadReport {
  size_t packsTried = 0;
  size_t packsApplied = 0;
  size_t packsFailed = 0;
  size_t signaturesRegistered = 0;
  size_t layoutsRegistered = 0;
  bool truncated = false;
  std::vector<SigPackLoadResult> results;
};

/// Loads signature packs from a directory through the filesystem seam.
///
/// Thread-safe against the registries (registration is atomic there); the
/// loader itself holds no mutable state beyond the seam and the root, so it is
/// safe to share once constructed.
class BERRY_API SignaturePackLoader {
 public:
  /// The suffix a discoverable pack file must carry. `.json` alone is not
  /// enough: the directory may hold unrelated JSON, and silently picking it up
  /// would register rules no author intended.
  static constexpr const char* kSuffix = ".sigpack.json";
  /// Caps so a hostile or runaway pack directory cannot exhaust memory.
  static constexpr size_t kMaxPacksPerLoad = 256;

  /// `fs` is borrowed and must outlive the loader. `root` is the directory
  /// `LoadDirectory()` reads when called with no argument.
  explicit SignaturePackLoader(IScriptFileSystem* fs, std::string root = "");

  SignaturePackLoader(const SignaturePackLoader&) = delete;
  SignaturePackLoader& operator=(const SignaturePackLoader&) = delete;

  /// Reads and applies a single pack file through the seam. The pack's build
  /// must match `activeBuild`. `result` receives the outcome.
  SigPackError LoadPackFile(const std::string& path,
                            const std::string& activeBuild,
                            SigCache& sigs,
                            LayoutRegistry* layouts = nullptr,
                            SigPackLoadResult* result = nullptr);

  /// Loads every `*.sigpack.json` directly under `root` (or the configured root
  /// when `root` is empty). A failed pack does not stop the others.
  SigPackLoadReport LoadDirectory(const std::string& activeBuild,
                                  SigCache& sigs,
                                  LayoutRegistry* layouts = nullptr,
                                  const std::string& root = "");

  const std::string& root() const { return root_; }
  void SetRoot(std::string root) { root_ = std::move(root); }

 private:
  IScriptFileSystem* fs_ = nullptr;
  std::string root_;
};

}  // namespace Berry
