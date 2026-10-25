// Persistent script subscriptions to the engine event bus. See
// include/berry/script_bus.h for the threading contract.
#include "berry/script_bus.h"

#include "berry/generated_bindings.h"
#include "berry/logger.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace Berry {

namespace {

bool IsChannelChar(char c) {
  if (std::isalnum((unsigned char)c)) return true;
  return c == '_' || c == ':' || c == '.' || c == '-';
}

bool IsIdentChar(char c) {
  return std::isalnum((unsigned char)c) != 0 || c == '_';
}

/// True when `channel` is one of the channels the IDL declares. Only those are
/// bridged from the engine bus, because only they carry a payload this layer
/// can decode (the bus convention: `data` points at a `const char*`). A
/// script-defined channel is delivered in-process by `Emit`, so a channel the
/// engine emits with a non-string payload is never misread as a string.
bool IsDeclaredChannel(const std::string& channel) {
  const api::EventSpec* specs = api::EventSpecs();
  for (size_t i = 0; i < api::EventSpecCount(); ++i) {
    if (channel == specs[i].name) return true;
  }
  return false;
}

}  // namespace

bool ScriptEventBus::ValidChannel(const std::string& channel) {
  if (channel.empty() || channel.size() > kMaxChannelLen) return false;
  for (char c : channel) {
    if (!IsChannelChar(c)) return false;
  }
  return true;
}

bool ScriptEventBus::ValidHandler(const std::string& handler) {
  if (handler.empty() || handler.size() > kMaxHandlerLen) return false;
  if (handler.front() == '.' || handler.back() == '.') return false;
  bool prevDot = false;
  for (char c : handler) {
    if (c == '.') {
      if (prevDot) return false;   // no "a..b"
      prevDot = true;
      continue;
    }
    if (!IsIdentChar(c)) return false;
    prevDot = false;
  }
  return true;
}

ScriptEventBus::~ScriptEventBus() {
  // Drop every engine listener this bus installed. A listener holds a closure
  // over `this`, so leaving one registered would call into a destroyed bus.
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& kv : channelIds_) {
    EventBus::Instance().Off(kv.first.c_str(), kv.second);
  }
  channelIds_.clear();
  channelRefs_.clear();
  subs_.clear();
}

void ScriptEventBus::Retain(const std::string& channel) {
  // Caller holds mutex_. The first subscriber installs the engine listener; a
  // later one just bumps the refcount, so N subscriptions on a channel cost one
  // listener, not N.
  size_t& refs = channelRefs_[channel];
  ++refs;
  if (refs > 1) return;
  if (!IsDeclaredChannel(channel)) return;   // script-only channel: no bridge
  const std::string captured = channel;
  EventBus::ListenerId id = EventBus::Instance().On(
      captured.c_str(), [this, captured](const EventContext& e) {
        // On the emitting thread (game tick, render, transport). Never runs
        // script code: copy the payload into the bounded queue and return.
        std::string payload;
        if (e.data) {
          const char* p = *(const char**)e.data;
          if (p) payload = p;
        }
        if (payload.size() > kMaxPayloadLen) payload.resize(kMaxPayloadLen);
        std::lock_guard<std::mutex> lk(mutex_);
        QueueForChannelLocked(captured, payload);
        return EventResult::Continue;
      });
  channelIds_[channel] = id;
}

void ScriptEventBus::QueueForChannelLocked(const std::string& channel,
                                           const std::string& payload) {
  // One entry per distinct script, so `Pump` — which runs every handler the
  // script holds on the channel — delivers each handler exactly once.
  std::set<std::string> seen;
  for (const Sub& s : subs_) {
    if (s.channel != channel) continue;
    if (!seen.insert(s.script).second) continue;
    if (queue_.size() >= kMaxQueued) {
      ++dropped_;
      continue;
    }
    queue_.push_back(Pending{channel, payload, s.script});
  }
}

void ScriptEventBus::Release(const std::string& channel) {
  // Caller holds mutex_.
  auto it = channelRefs_.find(channel);
  if (it == channelRefs_.end()) return;
  if (it->second > 1) {
    --it->second;
    return;
  }
  channelRefs_.erase(it);
  auto idIt = channelIds_.find(channel);
  if (idIt != channelIds_.end()) {
    EventBus::Instance().Off(channel.c_str(), idIt->second);
    channelIds_.erase(idIt);
  }
}

ScriptEventBus::SubscriptionId ScriptEventBus::On(const std::string& channel,
                                                 const std::string& handler,
                                                 const std::string& script) {
  if (!ValidChannel(channel) || !ValidHandler(handler) || script.empty()) {
    return 0;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  // Re-registering the same pair is idempotent: a script that re-runs its
  // setup (a hot reload) must not accumulate duplicate deliveries.
  for (const Sub& s : subs_) {
    if (s.channel == channel && s.handler == handler && s.script == script) {
      return s.id;
    }
  }
  if (subs_.size() >= kMaxSubscriptions) return 0;
  size_t mine = 0;
  for (const Sub& s : subs_) {
    if (s.script == script) ++mine;
  }
  if (mine >= kMaxSubscriptionsPerScript) return 0;

  SubscriptionId id = nextId_++;
  subs_.push_back(Sub{id, channel, handler, script});
  Retain(channel);
  return id;
}

bool ScriptEventBus::Off(SubscriptionId id, const std::string& script) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = subs_.begin(); it != subs_.end(); ++it) {
    if (it->id == id && it->script == script) {
      Release(it->channel);
      subs_.erase(it);
      return true;
    }
  }
  return false;
}

size_t ScriptEventBus::OffAll(const std::string& script) {
  std::lock_guard<std::mutex> lk(mutex_);
  size_t removed = 0;
  for (auto it = subs_.begin(); it != subs_.end();) {
    if (it->script == script) {
      Release(it->channel);
      it = subs_.erase(it);
      ++removed;
    } else {
      ++it;
    }
  }
  return removed;
}

size_t ScriptEventBus::CountFor(const std::string& script) const {
  std::lock_guard<std::mutex> lk(mutex_);
  size_t n = 0;
  for (const Sub& s : subs_) {
    if (s.script == script) ++n;
  }
  return n;
}

size_t ScriptEventBus::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return subs_.size();
}

std::vector<std::string> ScriptEventBus::Channels() const {
  std::set<std::string> out;
  const api::EventSpec* specs = api::EventSpecs();
  for (size_t i = 0; i < api::EventSpecCount(); ++i) out.insert(specs[i].name);
  {
    std::lock_guard<std::mutex> lk(mutex_);
    for (const Sub& s : subs_) out.insert(s.channel);
  }
  return std::vector<std::string>(out.begin(), out.end());
}

size_t ScriptEventBus::Emit(const std::string& channel,
                            const std::string& payload) {
  if (!ValidChannel(channel)) return 0;
  if (IsDeclaredChannel(channel)) {
    // A declared channel goes out on the engine bus, so an engine emitter and a
    // script emitter reach subscribers by the same path (and the bus listener
    // is already installed for it). The listener queues one entry per
    // subscriber script; the return is the number of handlers that will run.
    const char* p = payload.c_str();
    EventBus::Instance().Emit(channel.c_str(), &p);
    std::lock_guard<std::mutex> lk(mutex_);
    size_t n = 0;
    for (const Sub& s : subs_) {
      if (s.channel == channel) ++n;
    }
    return n;
  }
  // A script-only channel has no engine emitter, so enqueue it here.
  std::lock_guard<std::mutex> lk(mutex_);
  std::string copy = payload;
  if (copy.size() > kMaxPayloadLen) copy.resize(kMaxPayloadLen);
  size_t n = 0;
  for (const Sub& s : subs_) {
    if (s.channel == channel) ++n;
  }
  QueueForChannelLocked(channel, copy);
  return n;
}

size_t ScriptEventBus::Pump(ScriptHost& host) {
  const std::string script = host.name();
  size_t ran = 0;
  for (size_t guard = 0; guard < kMaxDeliveriesPerPump; ++guard) {
    // A suspended host belongs to the async layer: stop and leave the queue
    // intact so the events are delivered once it resumes. A faulted/unloaded
    // host cannot run a handler at all.
    if (!host.loaded() || host.suspended()) break;

    Pending ev;
    std::vector<std::string> handlers;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      // Only consume an event addressed to this script. An event queued for
      // another script's pump is skipped over, not waited on, so one script's
      // backlog can never starve another's delivery.
      auto it = queue_.end();
      for (auto q = queue_.begin(); q != queue_.end(); ++q) {
        if (q->script == script) { it = q; break; }
      }
      if (it == queue_.end()) break;
      ev = *it;
      queue_.erase(it);
      for (const Sub& s : subs_) {
        if (s.script == script && s.channel == ev.channel) {
          handlers.push_back(s.handler);
        }
      }
    }
    for (const std::string& handler : handlers) {
      if (!host.loaded() || host.suspended()) return ran;
      const ScriptStatus st =
          host.RunHook(handler, {ScriptValue::Str(ev.payload)});
      if (st == ScriptStatus::NoFunction) continue;   // handler not exported
      if (st == ScriptStatus::Yielded) return ran;    // async owns it now
      if (st != ScriptStatus::Ok) {
        // A faulting handler unloads the host (error isolation). Report and
        // stop; the queue keeps the rest for a reload.
        Log::Warn("ScriptEventBus[%s]: handler '%s' on '%s' failed",
                  script.c_str(), handler.c_str(), ev.channel.c_str());
        return ran;
      }
      ++ran;
      std::lock_guard<std::mutex> lk(mutex_);
      ++delivered_;
    }
  }
  return ran;
}

size_t ScriptEventBus::queued() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return queue_.size();
}

u64 ScriptEventBus::dropped() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return dropped_;
}

u64 ScriptEventBus::delivered() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return delivered_;
}

}  // namespace Berry
