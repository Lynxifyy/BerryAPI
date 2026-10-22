#pragma once
// Sandboxed script file store (the `fs.*` surface).
//
// A script gets its own directory and nothing else. This is the object that
// enforces "nothing else": a path is validated before it is used, so a script
// cannot name a file outside its own tree no matter how it spells the path.
//
// The rules are a pure function of the path, so they are unit-testable and
// cannot drift between the write path and the read path:
//
//   * no absolute path, no backslash, no drive letter, no control character,
//   * no empty segment, no "." and no ".." (so no traversal), no "//",
//   * a bounded length, a bounded depth, and a bounded character set.
//
// On top of the path rules sit size limits: a per-file cap, a total cap and a
// file count, so a script cannot exhaust memory by writing in a loop. Every
// operation fails closed (returns false / empty) rather than throwing.
//
// Storage itself is behind this class. The host build keeps an in-memory tree,
// which is what the tests exercise; the device adapter can back the same
// interface with the script's data directory. Nothing here reads a path from
// game memory, and no path is ever opened by the core itself.

#include "berry/export.h"
#include "berry/types.h"

#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// A validated, sandbox-relative path (no leading slash, no traversal).
struct BERRY_API ScriptPath {
  /// The normalized relative path, or empty when invalid.
  std::string value;
  bool valid = false;
};

/// Validates a script-supplied path against the sandbox rules. Pure.
/// `maxLen` bounds the whole path; `maxDepth` bounds the segment count.
BERRY_API ScriptPath ValidateScriptPath(const std::string& path,
                                        size_t maxLen = 256,
                                        size_t maxDepth = 16);

/// An in-memory, sandboxed file tree for one script. Thread-safe.
class BERRY_API ScriptFileStore {
 public:
  /// Per-file cap. Larger than any config or save a script legitimately needs,
  /// small enough that a runaway write cannot exhaust the worker.
  static constexpr size_t kMaxFileBytes = 1u << 20;      // 1 MiB
  /// Total cap across the tree.
  static constexpr size_t kMaxTotalBytes = 8u << 20;     // 8 MiB
  /// Maximum number of files.
  static constexpr size_t kMaxFiles = 256;

  /// Writes (or overwrites) a file. Fails closed on an invalid path, on data
  /// over the per-file cap, or when the write would exceed the total/count
  /// caps (an overwrite of an existing file only counts the delta).
  bool Write(const std::string& path, const std::string& data);

  /// Reads a file into `out`. False when the path is invalid or absent.
  bool Read(const std::string& path, std::string* out) const;

  /// Appends to a file, creating it if needed. Fails closed on the same caps.
  bool Append(const std::string& path, const std::string& data);

  bool Exists(const std::string& path) const;
  bool Remove(const std::string& path);
  /// The file's size, or 0 when absent (an absent file and an empty file are
  /// distinguished by Exists).
  size_t Size(const std::string& path) const;

  /// Every path in the tree, sorted, newline-separated. `prefix` filters to
  /// paths that start with it; an empty prefix lists everything.
  std::string List(const std::string& prefix = "") const;

  /// Removes everything.
  void Clear();

  size_t FileCount() const;
  size_t TotalBytes() const;

 private:
  mutable std::mutex mutex_;
  std::map<std::string, std::string> files_;
  size_t totalBytes_ = 0;
};

}  // namespace Berry
