#pragma once
// Script packages: semver, dependency resolution and `require`.
//
// A package is a JSON manifest plus an entry file. A script declares the
// packages it needs and a version constraint for each; the resolver picks one
// version per package, orders them so dependencies load first, and fails closed
// when no consistent choice exists.
//
// Three deliberate choices:
//
//   * Semver is parsed, not string-compared. `1.10.0` is newer than `1.9.0`,
//     and a pre-release (`1.0.0-rc.1`) sorts before its release.
//   * Constraints are normalised at parse time. `^1.2.3` and `~1.2.3` become
//     concrete `>=`/`<` pairs immediately, so satisfaction is a plain
//     comparison and cannot drift between the parser and the resolver.
//   * Resolution is deterministic and bounded. It is a fixpoint: recompute the
//     constraints implied by the currently chosen versions, re-pick the highest
//     version for each, and repeat until stable. If it does not stabilise
//     within a bounded number of rounds it fails, rather than guessing.
//
// Nothing here touches the disk. A registry is a set of manifests handed to it;
// reading them is the loader's job, so the resolver is testable without a
// filesystem and a hostile manifest cannot cause I/O.

#include "berry/export.h"
#include "berry/types.h"

#include <mutex>
#include <string>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Semver
//----------------------------------------------------------------------------

/// A `major.minor.patch` version with an optional pre-release tag.
struct BERRY_API SemVer {
  u32 major = 0;
  u32 minor = 0;
  u32 patch = 0;
  /// Empty for a release. Compared field-wise, then numerically per dot-part,
  /// with a pre-release sorting BEFORE the release it precedes.
  std::string prerelease;

  /// Parses `major.minor.patch[-prerelease]`, tolerating a leading `v` and
  /// surrounding space. Rejects a missing field, a non-numeric field, an empty
  /// or malformed pre-release, and a value that overflows u32.
  static bool Parse(const std::string& text, SemVer* out);

  std::string ToString() const;
  bool IsPrerelease() const { return !prerelease.empty(); }
};

/// Total order: -1, 0, +1. A pre-release precedes its release.
int CompareSemVer(const SemVer& a, const SemVer& b);
inline bool operator==(const SemVer& a, const SemVer& b) { return CompareSemVer(a, b) == 0; }
inline bool operator!=(const SemVer& a, const SemVer& b) { return CompareSemVer(a, b) != 0; }
inline bool operator<(const SemVer& a, const SemVer& b) { return CompareSemVer(a, b) < 0; }
inline bool operator<=(const SemVer& a, const SemVer& b) { return CompareSemVer(a, b) <= 0; }
inline bool operator>(const SemVer& a, const SemVer& b) { return CompareSemVer(a, b) > 0; }
inline bool operator>=(const SemVer& a, const SemVer& b) { return CompareSemVer(a, b) >= 0; }

//----------------------------------------------------------------------------
// Constraints
//----------------------------------------------------------------------------

enum class RangeOp : u32 {
  Eq = 0,   // == v
  Lt,       // < v
  Le,       // <= v
  Gt,       // > v
  Ge,       // >= v
};

const char* RangeOpName(RangeOp op);

/// One bound. Caret/tilde are expanded into these at parse time.
struct BERRY_API VersionRange {
  RangeOp op = RangeOp::Eq;
  SemVer version;
};

/// A disjunction of conjunctions: the version satisfies the constraint when it
/// satisfies EVERY range in at least ONE group. An empty group list means "no
/// version satisfies this" (the unsatisfiable constraint); `Any` is a single
/// empty group, which every version satisfies.
struct BERRY_API VersionConstraint {
  std::vector<std::vector<VersionRange>> groups;

  /// `"*"` or `""` is any version. Supports `=`, `>`, `>=`, `<`, `<=`, `^`,
  /// `~`, a bare version (exact), comma-separated AND, and `||`-separated OR.
  static bool Parse(const std::string& text, VersionConstraint* out,
                    std::string* error = nullptr);

  /// The constraint that admits every version.
  static VersionConstraint Any();
  /// The constraint that admits nothing.
  static VersionConstraint None();

  bool IsAny() const;
  bool IsNone() const;
  bool Satisfies(const SemVer& v) const;

  /// Canonical text, so a constraint round-trips through Parse().
  std::string ToString() const;
};

/// Conjunction of two constraints. Exact (it is the cross product of the OR
/// groups), bounded: an intersection that would exceed `kMaxRanges` collapses
/// to `None()` rather than growing without limit. A resolver treats that as a
/// conflict, which is fail-closed.
VersionConstraint Intersect(const VersionConstraint& a, const VersionConstraint& b);

/// The number of ranges across every group.
size_t RangeCount(const VersionConstraint& c);

//----------------------------------------------------------------------------
// Manifests
//----------------------------------------------------------------------------

/// Why a package manifest was refused.
enum class PackageError : u32 {
  Ok = 0,
  BadJson,
  MissingName,
  BadName,
  MissingVersion,
  BadVersion,
  MissingEntry,
  BadEntry,          // not a safe relative path
  BadDependencies,   // not an object, or a value that is not a string
  BadConstraint,
  Duplicate,         // a duplicate dependency name
  TooManyDeps,
};

const char* PackageErrorName(PackageError e);

/// A declared dependency: the package name and the versions that satisfy it.
struct BERRY_API PackageDependency {
  std::string name;
  VersionConstraint constraint;
  std::string raw;   // the constraint text, for diagnostics
};

struct BERRY_API PackageManifest {
  std::string name;
  std::string version;
  std::string entry;        // relative entry file, e.g. "main.luau"
  std::string author;
  std::string description;
  std::vector<PackageDependency> dependencies;
  SemVer parsedVersion;

  /// Parses and fully validates a package manifest. Rejects an unknown key, a
  /// duplicate dependency, a bad name/version/entry and a malformed
  /// constraint — the whole document, or nothing.
  static bool Parse(const std::string& json, PackageManifest* out,
                    PackageError* outError = nullptr,
                    std::string* error = nullptr);

  /// True when `entry` is a safe relative path: no absolute path, no `..`, no
  /// backslash, no empty segment. Same rule the script loader uses.
  static bool ValidEntryPath(const std::string& entry);
  /// True for a package name: a bounded identifier of `[a-z0-9-_.]`, starting
  /// with a letter or digit.
  static bool ValidName(const std::string& name);
};

//----------------------------------------------------------------------------
// Resolution
//----------------------------------------------------------------------------

enum class ResolveError : u32 {
  Ok = 0,
  MissingPackage,   // a dependency names a package the registry does not hold
  NoVersion,        // no registered version satisfies a constraint
  Conflict,         // the fixpoint did not converge on one version per package
  Cycle,            // dependencies form a cycle
  TooMany,          // more packages than the resolver will consider
};

const char* ResolveErrorName(ResolveError e);

/// One chosen package, in install order (dependencies before dependents).
struct BERRY_API ResolvedPackage {
  std::string name;
  std::string version;
  std::string entry;
};

struct BERRY_API ResolveResult {
  ResolveError error = ResolveError::Ok;
  std::string detail;                     // human-readable, empty on success
  std::vector<ResolvedPackage> order;     // install order; empty on failure
  std::vector<std::string> versions;      // "name@version", sorted by name

  bool ok() const { return error == ResolveError::Ok; }
};

/// The registry of known packages. Thread-safe.
class BERRY_API PackageRegistry {
 public:
  /// More packages than a sane script tree uses; a runaway one cannot make the
  /// resolver quadratic.
  static constexpr size_t kMaxPackages = 512;
  /// Bounded fixpoint rounds before a resolution is declared a conflict.
  static constexpr size_t kMaxRounds = 64;
  /// Bounded dependency count per manifest.
  static constexpr size_t kMaxDeps = 64;

  /// Registers a manifest. Refused for a duplicate `name@version` or an
  /// invalid manifest. The same name at different versions is expected.
  bool Add(const PackageManifest& manifest, std::string* error = nullptr);

  /// Parses and registers. Returns the parse error on failure.
  PackageError AddJson(const std::string& json, std::string* error = nullptr);

  bool Has(const std::string& name) const;
  bool HasVersion(const std::string& name, const std::string& version) const;
  /// Registered versions of `name`, ascending.
  std::vector<SemVer> Versions(const std::string& name) const;
  /// Registered package names, sorted.
  std::vector<std::string> Names() const;
  size_t Count() const;
  /// The manifest for `name@version`, or nullptr.
  const PackageManifest* Find(const std::string& name,
                              const std::string& version) const;

  /// Resolves `roots` to one version per package, in install order.
  ResolveResult Resolve(const std::vector<PackageDependency>& roots) const;

  void Clear();

 private:
  mutable std::mutex mutex_;
  std::vector<PackageManifest> manifests_;
};

/// Resolves a `require` path — `"pkg"` or `"pkg/sub/file.luau"` — against the
/// registry. On success `*name` is the package, `*entry` the file within it
/// (the package's own entry when the path names no file). Fails closed when the
/// package is absent, the sub-path escapes the package, or the package has no
/// entry.
bool BERRY_API ResolveRequire(const PackageRegistry& registry,
                              const std::string& path, std::string* name,
                              std::string* entry, std::string* error);

}  // namespace Berry
