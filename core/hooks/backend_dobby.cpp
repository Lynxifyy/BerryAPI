// Dobby inline-hook backend for on-device (Android) builds.
//
// Compiled only when the target build enables Dobby
// (BERRY_PLATFORM_ANDROID + BERRY_HOOK_ENGINE_DOBBY). On the host harness
// the shim backend is used instead so tests never require the device libs.
//
// Dobby's public include lives at third_party/Dobby/include/dobby.h; the
// build wires the Dobby sources (or prebuilt lib) in. Hook<FnPtr> users call
// through the returned trampoline, which Dobby relocates from the original
// prologue.

#include "berry/hooks.h"
#include "berry/logger.h"
#include "berry/types.h"

#if defined(BERRY_HOOK_HAVE_DOBBY)

#include "dobby.h"
#include <map>
#include <mutex>

namespace Berry {

namespace {

class DobbyBackend : public IHook {
 public:
  const char* BackendName() const override { return "Dobby"; }
  bool CanHook() const override {
    return DobbyGetVersion() != nullptr;
  }

  HookStatus Install(void* target, VoidFn detour, HookSymbol* out) override {
    if (!target || !detour) return HookStatus::NoTarget;
    std::lock_guard<std::mutex> lk(m_);
    if (active_.find(target) != active_.end())
      return HookStatus::AlreadyHooked;

    void* tramp = nullptr;
    if (DobbyHook(target, reinterpret_cast<void*>(detour), &tramp) != 0)
      return HookStatus::BackendError;

    *out = HookSymbol{target, reinterpret_cast<void*>(detour), tramp, true};
    active_[target] = *out;
    return HookStatus::Ok;
  }

  HookStatus Uninstall(void* target) override {
    if (!target) return HookStatus::NoTarget;
    std::lock_guard<std::mutex> lk(m_);
    auto it = active_.find(target);
    if (it == active_.end()) return HookStatus::NoTarget;
    if (DobbyDestroy(target) != 0) return HookStatus::BackendError;
    active_.erase(it);
    return HookStatus::Ok;
  }

  bool IsHooked(void* target) const override {
    std::lock_guard<std::mutex> lk(m_);
    return active_.find(target) != active_.end();
  }

 private:
  mutable std::mutex m_;
  std::map<void*, HookSymbol> active_;
};

}  // namespace

void HookManager::UseDobby() {
  delete backend_;
  backend_ = new DobbyBackend();
}

}  // namespace Berry

#endif  // BERRY_HOOK_HAVE_DOBBY