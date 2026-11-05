#pragma once
// Hook chains and RAII hook groups (improvements 8, 9, 10).
//
// The backend installs exactly one detour per target. A `HookChain` is how
// several subsystems share that one slot without clobbering each other: they
// register *handlers*, each a middleware function that receives a
// `callOriginal` continuation and the arguments, so handlers compose like an
// onion instead of replacing one another. Handlers run in descending priority
// order; the last one calls the original through the trampoline.
//
// The dispatcher is chosen from a fixed pool of compile-time slots (the
// backend needs a plain function pointer, and only a compile-time index can
// select one), so a process supports up to `kMaxHookChains` chains at a time.
//
// `ScopedHook` is the RAII form of the existing `Hook<FnPtr>`, and
// `HookGroup` installs a set of hooks and rolls all of them back if any one
// fails, so a partial install never leaves the process half-hooked.

#include "berry/export.h"
#include "berry/hooks.h"
#include "berry/types.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// Options for one handler in a chain.
struct BERRY_API HookHandlerOptions {
  /// Higher runs first. Ties keep registration order.
  int priority = 0;
  /// A label for diagnostics.
  std::string name;
};

namespace detail {

/// The most chains a process can have installed at once. Each needs a distinct
/// compile-time dispatcher slot.
constexpr size_t kMaxHookChains = 32;

/// One dispatcher slot. The chain that owns the slot is stored here, and the
/// per-slot dispatcher forwards to it.
struct BERRY_API ChainSlot {
  void* chain = nullptr;   // a ChainErased*, cast back in the dispatcher
  bool used = false;
};
BERRY_API ChainSlot* ChainSlots();   // kMaxHookChains entries

/// Type-erased base so a slot can hold any instantiation of HookChain.
struct BERRY_API ChainErased {
  virtual ~ChainErased() = default;
};

/// The typed view the dispatcher casts the slot back to.
template <typename R, typename... A>
struct ChainTyped : ChainErased {
  virtual R Invoke(A... args) = 0;
};

/// One dispatcher per compile-time slot. Defined in the header because the
/// concrete argument types are only known at the instantiation.
template <size_t Slot, typename R, typename... A>
R ChainDispatcher(A... args) {
  ChainSlot* slots = ChainSlots();
  auto* c = static_cast<ChainTyped<R, A...>*>(slots[Slot].chain);
  return c->Invoke(std::forward<A>(args)...);
}

/// Selects the dispatcher for runtime slot `want`, instantiating slots lazily.
template <size_t Slot, typename R, typename... A>
VoidFn SelectSlot(size_t want) {
  if (want == Slot) {
    return reinterpret_cast<VoidFn>(&ChainDispatcher<Slot, R, A...>);
  }
  if constexpr (Slot + 1 < kMaxHookChains) {
    return SelectSlot<Slot + 1, R, A...>(want);
  }
  return nullptr;
}

/// Claims a free slot, returning its index or kMaxHookChains when full.
BERRY_API size_t ClaimSlot();
BERRY_API void ReleaseSlot(size_t slot);

}  // namespace detail

/// Several handlers over one target, composed in priority order. A handler is
/// `R(Next next, A... args)`: call `next(args...)` to run the rest of the chain
/// (ending at the original), or return without calling it to short-circuit.
template <typename FnPtr>
class HookChain;

template <typename R, typename... A>
class HookChain<R (*)(A...)> : public detail::ChainTyped<R, A...> {
 public:
  using Fn = R (*)(A...);
  using Next = std::function<R(A...)>;
  using Handler = std::function<R(Next, A...)>;

  /// Installs the chain on `target` through `backend`. Fails (and installs
  /// nothing) when the backend cannot hook or no slot is free.
  HookChain(IHook* backend, void* target, const char* name = "chain")
      : backend_(backend), target_(target), name_(name ? name : "chain") {
    if (!backend_ || !target_) return;
    slot_ = detail::ClaimSlot();
    if (slot_ >= detail::kMaxHookChains) return;
    detail::ChainSlots()[slot_].chain = this;
    VoidFn dispatcher = detail::SelectSlot<0, R, A...>(slot_);
    if (!dispatcher) {
      detail::ReleaseSlot(slot_);
      slot_ = detail::kMaxHookChains;
      return;
    }
    HookSymbol sym;
    if (backend_->Install(target_, dispatcher, &sym) != HookStatus::Ok) {
      detail::ReleaseSlot(slot_);
      slot_ = detail::kMaxHookChains;
      return;
    }
    original_ = reinterpret_cast<Fn>(sym.trampoline ? sym.trampoline : sym.target);
    installed_ = true;
  }

  ~HookChain() { Remove(); }

  HookChain(const HookChain&) = delete;
  HookChain& operator=(const HookChain&) = delete;

  bool installed() const { return installed_; }

  /// Registers a handler. Returns false when the chain is not installed, the
  /// handler is empty, or the handler cap is reached.
  bool Add(const std::string& name, Handler fn, const HookHandlerOptions& opts = {}) {
    if (!installed_ || !fn) return false;
    std::lock_guard<std::mutex> lk(mutex_);
    if (handlers_.size() >= kMaxHandlers) return false;
    HandlerEntry e;
    e.name = name.empty() ? opts.name : name;
    e.priority = opts.priority;
    e.fn = std::move(fn);
    // Insert keeping descending priority, stable on ties (registration order).
    auto it = handlers_.begin();
    while (it != handlers_.end() && it->priority >= e.priority) ++it;
    handlers_.insert(it, std::move(e));
    return true;
  }

  /// Removes a handler by name. Returns true when one was removed.
  bool Remove(const std::string& name) {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto it = handlers_.begin(); it != handlers_.end(); ++it) {
      if (it->name == name) {
        handlers_.erase(it);
        return true;
      }
    }
    return false;
  }

  size_t handlerCount() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return handlers_.size();
  }

  /// The trampoline, so a handler can call the original directly.
  Fn original() const { return original_; }

  /// Calls the original through the trampoline (a safe `callOriginal`).
  R callOriginal(A... args) { return original_(std::forward<A>(args)...); }

  /// Uninstalls the chain. Idempotent.
  void Remove() {
    if (!installed_) return;
    installed_ = false;
    backend_->Uninstall(target_);
    if (slot_ < detail::kMaxHookChains) {
      detail::ChainSlots()[slot_].chain = nullptr;
      detail::ReleaseSlot(slot_);
      slot_ = detail::kMaxHookChains;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    handlers_.clear();
  }

  // The dispatcher entry point (detail::ChainTyped).
  R Invoke(A... args) override {
    std::vector<HandlerEntry> snapshot;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      snapshot = handlers_;
    }
    // Build the onion: the last (lowest-priority) handler calls the original.
    Next next = [this](A... a) { return original_(std::forward<A>(a)...); };
    for (auto it = snapshot.rbegin(); it != snapshot.rend(); ++it) {
      Handler h = it->fn;
      Next prev = next;
      next = [h, prev](A... a) { return h(prev, std::forward<A>(a)...); };
    }
    return next(std::forward<A>(args)...);
  }

  static constexpr size_t kMaxHandlers = 16;

 private:
  struct HandlerEntry {
    std::string name;
    int priority = 0;
    Handler fn;
  };
  IHook* backend_ = nullptr;
  void* target_ = nullptr;
  std::string name_;
  Fn original_ = nullptr;
  bool installed_ = false;
  size_t slot_ = detail::kMaxHookChains;
  mutable std::mutex mutex_;
  std::vector<HandlerEntry> handlers_;
};

/// RAII form of `Hook<FnPtr>` (improvement 10): installs on construction and
/// uninstalls on destruction, with an explicit `Release()` to uninstall early.
template <typename FnPtr>
class ScopedHook {
 public:
  ScopedHook() = default;
  ScopedHook(IHook* backend, void* target, FnPtr detour) {
    Install(backend, target, detour);
  }
  ~ScopedHook() { Release(); }
  ScopedHook(const ScopedHook&) = delete;
  ScopedHook& operator=(const ScopedHook&) = delete;

  HookStatus Install(IHook* backend, void* target, FnPtr detour) {
    if (hook_.installed()) return HookStatus::AlreadyHooked;
    backend_ = backend;
    // The inner Hook is default-constructed with no backend; hand it ours
    // before installing (Hook::SetBackend is valid only before the first
    // install).
    hook_.SetBackend(backend);
    return hook_.Install(target, detour);
  }
  void Release() {
    if (hook_.installed()) hook_.Reset();
  }
  bool installed() const { return hook_.installed(); }
  FnPtr original() const { return hook_.original(); }
  void* target() const { return hook_.target(); }

 private:
  Hook<FnPtr> hook_;
  IHook* backend_ = nullptr;
};

/// A set of hooks installed together: if any install fails, every hook
/// installed so far is rolled back, so a partial install never leaves the
/// process half-hooked (improvement 10).
template <typename FnPtr>
class HookGroup {
 public:
  HookGroup() = default;
  ~HookGroup() { RemoveAll(); }
  HookGroup(const HookGroup&) = delete;
  HookGroup& operator=(const HookGroup&) = delete;

  /// Installs every (target, detour) pair. On the first failure, uninstalls
  /// what was installed and returns false; `failedIndex` (optional) reports
  /// which pair failed.
  bool InstallAll(IHook* backend,
                  const std::vector<std::pair<void*, FnPtr>>& hooks,
                  size_t* failedIndex = nullptr) {
    for (size_t i = 0; i < hooks.size(); ++i) {
      // Hook is non-copyable and non-movable, so it lives behind a unique_ptr.
      auto h = std::make_unique<Hook<FnPtr>>(backend);
      if (h->Install(hooks[i].first, hooks[i].second) != HookStatus::Ok) {
        if (failedIndex) *failedIndex = i;
        RemoveAll();
        return false;
      }
      hooks_.push_back(std::move(h));
    }
    return true;
  }

  /// Uninstalls every hook in reverse order.
  void RemoveAll() { hooks_.clear(); }
  size_t count() const { return hooks_.size(); }

 private:
  std::vector<std::unique_ptr<Hook<FnPtr>>> hooks_;
};

}  // namespace Berry
