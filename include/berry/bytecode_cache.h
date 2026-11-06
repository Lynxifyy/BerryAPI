#pragma once
// A compiled-bytecode cache (improvement 62).
//
// Compiling Luau source is the slow part of loading a script; the bytecode is
// deterministic for a given source + compiler, so caching it keyed by the
// source hash lets a warm start skip compilation. This is the cache component
// only — the VM decides when to consult it, so the caching policy (and its
// invalidation) is testable without the VM.
//
// A cache entry is keyed by the SHA-256 of the source, so a changed source can
// never return a stale chunk. Entries are bounded by count and total bytes, and
// the eviction is oldest-first.

#include "berry/export.h"
#include "berry/types.h"

#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// Caches compiled bytecode keyed by the source's SHA-256.
class BERRY_API BytecodeCache {
 public:
  BytecodeCache(size_t maxEntries = 64, size_t maxBytes = 32u * 1024 * 1024)
      : maxEntries_(maxEntries), maxBytes_(maxBytes) {}

  /// The cache key for `source`: lowercase hex SHA-256. Exposed so the VM and
  /// a test compute the same key.
  static std::string KeyFor(const std::string& source);

  /// Looks up the bytecode for `source`. Returns false on a miss. A hit moves
  /// the entry to the front (most-recently-used).
  bool Get(const std::string& source, std::string* bytecode);

  /// Stores the bytecode for `source`, evicting oldest entries until it fits.
  /// A bytecode larger than the whole budget is not stored.
  void Put(const std::string& source, const std::string& bytecode);

  size_t Count() const;
  size_t Bytes() const;
  void Clear();

  /// A hit/miss tally for diagnostics.
  u64 hits() const { return hits_; }
  u64 misses() const { return misses_; }

 private:
  struct Entry {
    std::string key;
    std::string bytecode;
  };
  void EvictLocked();

  mutable std::mutex mutex_;
  std::list<Entry> lru_;                                  // front = most recent
  std::map<std::string, std::list<Entry>::iterator> index_;
  size_t bytes_ = 0;
  size_t maxEntries_;
  size_t maxBytes_;
  u64 hits_ = 0;
  u64 misses_ = 0;
};

}  // namespace Berry
