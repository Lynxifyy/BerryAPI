#pragma once
// An HTTP keep-alive connection pool (improvement 36).
//
// A request that sends `Connection: keep-alive` may reuse the TCP connection
// for the next request to the same host:port instead of paying a fresh
// handshake. This owns the idle sockets and hands them back out, with a bound
// on how many are kept and how long an idle one survives, so a pool cannot grow
// without limit or hold a dead socket forever.
//
// It owns no clock (`nowMs` is passed in) so it is deterministic and testable;
// the socket is an opaque handle the caller owns, so this never touches the
// transport directly.

#include "berry/export.h"
#include "berry/types.h"

#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>

namespace Berry {

/// Keeps idle connections keyed by "host:port", bounded by count and idle age.
template <typename Conn>
class ConnPool {
 public:
  /// `maxIdle` caps the total idle connections; `idleTimeoutMs` drops one that
  /// has been idle longer than this.
  ConnPool(size_t maxIdle = 8, u64 idleTimeoutMs = 30000)
      : maxIdle_(maxIdle), idleTimeoutMs_(idleTimeoutMs) {}

  /// Takes an idle connection for `key`, or a default-constructed `Conn` when
  /// none is available. A connection that has outlived the idle timeout is
  /// discarded (and handed to `close`) rather than returned.
  Conn Acquire(const std::string& key, u64 nowMs,
               const std::function<void(Conn&)>& close) {
    std::lock_guard<std::mutex> lk(mutex_);
    ReapExpired(nowMs, close);
    auto it = idle_.find(key);
    if (it == idle_.end() || it->second.empty()) return Conn{};
    Entry e = it->second.front();
    it->second.pop_front();
    if (it->second.empty()) idle_.erase(it);
    return e.conn;
  }

  /// Returns a connection to the pool for reuse. When the pool is full, the
  /// oldest idle connection is closed to make room.
  void Release(const std::string& key, Conn conn, u64 nowMs,
               const std::function<void(Conn&)>& close) {
    std::lock_guard<std::mutex> lk(mutex_);
    ReapExpired(nowMs, close);
    if (CountLocked() >= maxIdle_) {
      // Evict the globally oldest idle connection.
      std::string oldestKey;
      u64 oldest = ~0ull;
      for (const auto& kv : idle_) {
        if (!kv.second.empty() && kv.second.front().atMs < oldest) {
          oldest = kv.second.front().atMs;
          oldestKey = kv.first;
        }
      }
      if (!oldestKey.empty()) {
        Entry victim = idle_[oldestKey].front();
        idle_[oldestKey].pop_front();
        if (idle_[oldestKey].empty()) idle_.erase(oldestKey);
        close(victim.conn);
      }
    }
    idle_[key].push_back(Entry{std::move(conn), nowMs});
  }

  /// Closes and drops every idle connection.
  void Clear(const std::function<void(Conn&)>& close) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& kv : idle_) {
      for (auto& e : kv.second) close(e.conn);
    }
    idle_.clear();
  }

  /// The number of idle connections (diagnostics).
  size_t IdleCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return CountLocked();
  }
  size_t maxIdle() const { return maxIdle_; }

 private:
  struct Entry {
    Conn conn;
    u64 atMs = 0;
  };
  size_t CountLocked() const {
    size_t n = 0;
    for (const auto& kv : idle_) n += kv.second.size();
    return n;
  }
  void ReapExpired(u64 nowMs, const std::function<void(Conn&)>& close) {
    for (auto it = idle_.begin(); it != idle_.end();) {
      auto& dq = it->second;
      while (!dq.empty() && nowMs - dq.front().atMs > idleTimeoutMs_) {
        close(dq.front().conn);
        dq.pop_front();
      }
      if (dq.empty()) {
        it = idle_.erase(it);
      } else {
        ++it;
      }
    }
  }

  mutable std::mutex mutex_;
  std::map<std::string, std::list<Entry>> idle_;
  size_t maxIdle_;
  u64 idleTimeoutMs_;
};

}  // namespace Berry
