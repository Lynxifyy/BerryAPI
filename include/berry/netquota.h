#pragma once
// Per-script network quotas (improvement 53).
//
// A script's network grant says WHICH hosts it may reach; a quota says HOW
// MUCH. Without one, a script that is granted `network:*` can open sockets in a
// loop and saturate the radio. This enforces three bounds per script:
//
//   * a maximum number of concurrently open sockets,
//   * a byte-per-second budget (a token bucket, so a burst is allowed but the
//     sustained rate is capped),
//   * a maximum request rate per second.
//
// It owns no clock: `nowMs` is passed in, so it is deterministic and testable.

#include "berry/export.h"
#include "berry/types.h"

#include <map>
#include <mutex>
#include <string>

namespace Berry {

/// The bounds for one script. Zero means "no limit" for that dimension.
struct BERRY_API NetQuotaLimits {
  size_t maxConcurrentSockets = 0;
  u64 bytesPerSecond = 0;
  u32 requestsPerSecond = 0;
};

/// Why a quota check failed.
enum class NetQuotaResult : u32 {
  Ok = 0,
  TooManySockets,
  RateLimited,
  ByteBudgetExhausted,
};

const char* NetQuotaResultName(NetQuotaResult r);

/// Tracks per-script network usage against `NetQuotaLimits`.
class BERRY_API NetQuotaTracker {
 public:
  /// Sets the limits for `script` (replacing any previous). An all-zero limit
  /// clears the entry (unlimited).
  void SetLimits(const std::string& script, const NetQuotaLimits& limits);
  /// The limits for `script` (all zero when unset).
  NetQuotaLimits Limits(const std::string& script) const;

  /// Checks whether `script` may open one more socket now. Returns Ok or the
  /// reason it may not. On Ok, the caller must call `SocketOpened` when the
  /// socket is actually created (so the count reflects reality).
  NetQuotaResult AllowConnect(const std::string& script, u64 nowMs);
  /// Records that a socket was opened for `script`.
  void SocketOpened(const std::string& script);
  /// Records that a socket was closed for `script`.
  void SocketClosed(const std::string& script);

  /// Charges `bytes` against `script`'s byte budget. Returns Ok when the charge
  /// fits, or ByteBudgetExhausted (and does not charge) when it would not.
  NetQuotaResult ChargeBytes(const std::string& script, u64 bytes, u64 nowMs);

  /// The current concurrent socket count for `script`.
  size_t OpenSockets(const std::string& script) const;
  /// The bytes remaining in `script`'s budget this second.
  u64 BytesRemaining(const std::string& script, u64 nowMs) const;

  /// Drops all state for `script` (on unload).
  void Forget(const std::string& script);
  void Clear();

 private:
  struct State {
    NetQuotaLimits limits;
    size_t open = 0;
    // Request-rate bucket.
    u64 reqWindowStart = 0;
    u32 reqsThisSecond = 0;
    // Byte-rate bucket.
    u64 byteWindowStart = 0;
    u64 bytesThisSecond = 0;
  };
  State& For(const std::string& script);
  mutable std::mutex mutex_;
  std::map<std::string, State> states_;
};

}  // namespace Berry
