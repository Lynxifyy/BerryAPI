#pragma once
// Persistent script subscriptions to the engine event bus (the "script bus").
//
// `Events.once` (see berry/async.h) is a one-shot await: it suspends the
// caller and resumes it with a single payload. `Events.on` is the push
// counterpart a long-lived mod actually wants: a named handler that runs every
// time a channel fires, for the life of the script.
//
// Threading is the whole design problem. Engine channels are emitted from the
// game-tick thread, the render thread and the transport threads, but script
// code may only run on the script worker. A bus listener therefore never calls
// into the script: it copies the payload into a bounded queue. `Pump` drains
// that queue on the script worker, between hooks, and invokes the handler
// through `ScriptHost::RunHook` — the same path `on_tick` uses, so budgets,
// capability gating and error isolation apply unchanged.
//
// That split also makes re-entrancy impossible to get wrong: a handler that
// calls `Events.emit` appends to the queue, and the new event is delivered on a
// later pump (the per-pump delivery cap bounds a handler that emits its own
// channel, so it can never spin). The queue is bounded and drops on overflow
// rather than growing without limit, and it counts what it dropped.

#include "berry/event.h"
#include "berry/export.h"
#include "berry/script.h"
#include "berry/types.h"

#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

class BERRY_API ScriptEventBus {
 public:
  using SubscriptionId = u64;

  /// Hard bounds. A subscription is a small record, but a runaway script must
  /// not be able to grow the registry or the queue without limit.
  static constexpr size_t kMaxSubscriptions = 256;
  static constexpr size_t kMaxSubscriptionsPerScript = 64;
  static constexpr size_t kMaxQueued = 1024;
  /// Deliveries one `Pump` may perform, so a handler that republishes its own
  /// channel cannot spin inside a single tick.
  static constexpr size_t kMaxDeliveriesPerPump = 256;
  static constexpr size_t kMaxChannelLen = 64;
  static constexpr size_t kMaxHandlerLen = 64;
  static constexpr size_t kMaxPayloadLen = 1024;

  ScriptEventBus() = default;
  ~ScriptEventBus();

  ScriptEventBus(const ScriptEventBus&) = delete;
  ScriptEventBus& operator=(const ScriptEventBus&) = delete;

  /// A channel is a short, printable token: letters, digits, `_`, `:`, `.`
  /// and `-`. Empty or over-long is refused, so a name can never be a channel
  /// that some other layer then has to sanitise.
  static bool ValidChannel(const std::string& channel);
  /// A handler is the name of a function the script exports, so it shares the
  /// identifier shape (letters, digits, `_`) plus the `ns.fn` dot form.
  static bool ValidHandler(const std::string& handler);

  /// Registers a persistent subscription. Returns the subscription id, or 0
  /// when refused (invalid channel/handler, or a cap reached). Re-registering
  /// the same (script, channel, handler) triple returns the existing id rather
  /// than adding a duplicate.
  SubscriptionId On(const std::string& channel, const std::string& handler,
                    const std::string& script);

  /// Removes one subscription by id, but only when `script` owns it — a script
  /// must not be able to cancel another's handler. Returns true when removed.
  bool Off(SubscriptionId id, const std::string& script);
  /// Removes every subscription held by `script`. Returns how many were removed.
  size_t OffAll(const std::string& script);
  /// Subscriptions held by `script`.
  size_t CountFor(const std::string& script) const;
  /// Every subscription across all scripts.
  size_t Count() const;

  /// The IDL-declared channels plus every channel currently subscribed to,
  /// de-duplicated and sorted. What a script can discover with
  /// `Events.channels`.
  std::vector<std::string> Channels() const;

  /// Publishes `payload` on `channel`. Returns how many handler subscriptions
  /// the event was queued for. Delivery itself is deferred to `Pump`, so the
  /// return is a count of recipients, not of handler runs (a script with two
  /// handlers on the channel counts twice but is queued once).
  size_t Emit(const std::string& channel, const std::string& payload);

  /// Delivers queued events to `host` on the script worker. Returns the number
  /// of handler invocations that ran to completion. Stops early, leaving the
  /// rest queued, when the host suspends (the async layer owns it then) or
  /// faults.
  size_t Pump(ScriptHost& host);

  size_t queued() const;
  u64 dropped() const;
  u64 delivered() const;

 private:
  struct Sub {
    SubscriptionId id = 0;
    std::string channel;
    std::string handler;
    std::string script;
  };
  struct Pending {
    std::string channel;
    std::string payload;
    /// The script the entry was queued for. An event is delivered only by the
    /// pump of the host it belongs to, so one script's backlog never starves or
    /// leaks into another's.
    std::string script;
  };

  /// Adds a listener for `channel` on the engine bus (ref-counted, so the
  /// first subscriber installs it and the last one removes it).
  void Retain(const std::string& channel);
  void Release(const std::string& channel);

  /// Caller holds mutex_. Queues one entry per distinct script subscribed to
  /// `channel`. `Pump` runs every handler that script has on the channel, so
  /// queueing per subscription would run each handler once per subscription
  /// (a script with two handlers on one channel would get four runs). One
  /// entry per script gives each handler exactly one run per emitted event.
  void QueueForChannelLocked(const std::string& channel,
                             const std::string& payload);

  mutable std::mutex mutex_;
  std::vector<Sub> subs_;
  std::unordered_map<std::string, size_t> channelRefs_;
  std::unordered_map<std::string, EventBus::ListenerId> channelIds_;
  std::deque<Pending> queue_;
  SubscriptionId nextId_ = 1;
  u64 dropped_ = 0;
  u64 delivered_ = 0;
};

}  // namespace Berry
