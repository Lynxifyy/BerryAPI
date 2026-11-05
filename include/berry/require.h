#pragma once
// A sandboxed `require` backed by a virtual filesystem (improvement 63).
//
// A script can `require("helpers")` and get the module's source to compile,
// without any ability to escape the script root or reach the host filesystem.
// The resolver:
//
//   * only reads through an `IScriptFileSystem` seam, so a device build can
//     route it through the game's own file layer;
//   * refuses any path with a `..` component, an absolute prefix, or a
//     backslash, so a module name cannot climb out of the root;
//   * appends a fixed suffix (`.luau`) and bounds the module-name length;
//   * caches resolved modules so a diamond dependency compiles once;
//   * detects a cycle and fails closed rather than recursing forever;
//   * honours a per-module version constraint from the caller against a
//     semver in the module's manifest, refusing a mismatch.

#include "berry/export.h"
#include "berry/loader.h"
#include "berry/package.h"
#include "berry/types.h"

#include <map>
#include <string>
#include <vector>

namespace Berry {

/// Why a require failed.
enum class RequireError : u32 {
  Ok = 0,
  BadName,        // empty, too long, or contains an illegal component
  NotFound,       // the file could not be read
  Cycle,          // the module is already being resolved (a require cycle)
  VersionMismatch,// the module's declared version does not satisfy the request
  TooManyModules, // the module cap was reached
};

const char* RequireErrorName(RequireError e);

/// Resolves and caches script modules from a virtual filesystem.
class BERRY_API ModuleResolver {
 public:
  /// `fs` is borrowed and must outlive the resolver. `root` is the directory
  /// (below which the seam reads) module names are relative to.
  ModuleResolver(IScriptFileSystem* fs, std::string root = "");

  /// Resolves `name` to source, honouring an optional version constraint
  /// (`constraint` empty means "any"). On success `out` holds the source and
  /// `resolvedVersion` the module's declared version (empty when it declares
  /// none). A cached module is returned without a second read.
  RequireError Resolve(const std::string& name, const std::string& constraint,
                       std::string* out, std::string* resolvedVersion = nullptr);

  /// The modules resolved so far (in first-resolution order).
  const std::vector<std::string>& Resolved() const { return order_; }
  /// The declared version of a resolved module, or empty.
  std::string Version(const std::string& name) const;

  /// Clears the cache (e.g. on reload).
  void Reset();

  static constexpr size_t kMaxModules = 256;
  static constexpr size_t kMaxNameLen = 128;

  /// True when `name` is a safe module name: no `..`, no leading `/`, no
  /// backslash, no NUL, and within the length bound. Exposed for the test.
  static bool SafeName(const std::string& name);

 private:
  struct Entry {
    std::string source;
    std::string version;
  };
  IScriptFileSystem* fs_;
  std::string root_;
  std::map<std::string, Entry> cache_;
  std::vector<std::string> order_;
  std::vector<std::string> inProgress_;   // for cycle detection
};

}  // namespace Berry
