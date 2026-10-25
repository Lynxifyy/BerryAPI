#pragma once
#include "berry/version.h"
#include "berry/types.h"
#include "berry/export.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>

//----------------------------------------------------------------------------
// Dobby / inline-hook backend abstraction.
//
// On device (Android aarch64) this wraps Dobby / And64InlineHook's
// DobbyHook(). On the desktop harness, a "shim" backend patches 5-byte
// relative jumps (x86-64) so that all engine/runtime code paths can be unit
// tested without the game.
//
// Third-party hook engines (Dobby, And64InlineHook) are pointed to with
// BERRY_HOOK_INCLUDE and linked in by the build. BerryAPI itself never calls
// the game's own hooking facilities to keep a clean injection surface.
//----------------------------------------------------------------------------

#if defined(BERRY_PLATFORM_ANDROID) && defined(BERRY_HOOK_ENGINE_DOBBY)
#define BERRY_HOOK_HAVE_DOBBY 1
#endif

struct HookSymbol {
  void* target;         // old function pointer (used to call through)
  void* detour;         // new function pointer
  void* trampoline;     // where the original bytes were relocated
  bool  active;
};

namespace Berry {

// Value-return free function pointer.
using VoidFn = void (*)();

enum class HookStatus { Ok, NoTarget, NotAttached, InvalidArch, AlreadyHooked, BackendError };

class BERRY_API IHook {
 public:
  virtual ~IHook() = default;
  virtual HookStatus Install(void* target, VoidFn detour, HookSymbol* out) = 0;
  virtual HookStatus Uninstall(void* target) = 0;
  virtual bool IsHooked(void* target) const = 0;
  virtual const char* BackendName() const = 0;
  virtual bool CanHook() const = 0;
};

/// Typed helper around a hook: keeps the original trampoline to call through.
template <typename FnPtr>
class Hook {
 public:
  Hook() = default;
  Hook(IHook* backend, void* target, FnPtr detour)
      : backend_(backend) {
    Install(target, detour);
  }
  /// Constructs without installing, so a caller can defer the install until the
  /// target and detour are both known. The backend must be non-null before
  /// `Install`.
  explicit Hook(IHook* backend) : backend_(backend) {}
  /// Sets the backend after construction. Only valid before the first
  /// `Install`; installing twice on one `Hook` is refused.
  void SetBackend(IHook* backend) { backend_ = backend; }
  ~Hook() {
    if (backend_ && fine_ && symbol_.target)
      backend_->Uninstall(symbol_.target);
  }
  Hook(const Hook&) = delete;
  Hook& operator=(const Hook&) = delete;

  HookStatus Install(void* target, FnPtr detour) {
    HookSymbol sym;
    auto st = backend_->Install(target, reinterpret_cast<VoidFn>(detour), &sym);
    if (st == HookStatus::Ok) {
      symbol_ = sym;
      orig_ = reinterpret_cast<FnPtr>(sym.trampoline ? sym.trampoline : sym.target);
      target_ = target;
      fine_ = true;
    }
    return st;
  }

  /// Calls the original function (trampoline) with matching args.
  FnPtr original() const { return orig_; }
  bool installed() const { return fine_; }
  void* target() const { return target_; }
  IHook* backend() const { return backend_; }

  /// Removes the detour (if any) and clears the state, so the Hook can be
  /// re-installed on a new target. Idempotent. Non-copyable and non-movable,
  /// so a caller that needs to drop a live hook calls this rather than
  /// assigning a fresh Hook.
  void Reset() {
    if (backend_ && fine_ && symbol_.target) backend_->Uninstall(symbol_.target);
    symbol_ = HookSymbol{};
    orig_ = nullptr;
    target_ = nullptr;
    fine_ = false;
  }

 private:
  IHook* backend_ = nullptr;
  HookSymbol symbol_;
  FnPtr orig_ = nullptr;
  void* target_ = nullptr;
  bool fine_ = false;
};

//----------------------------------------------------------------------------
// Backend selection & manager.
//----------------------------------------------------------------------------

class BERRY_API HookManager {
 public:
  static HookManager& Instance();

  IHook* Backend();
  void BackendEmulateShim();

#ifdef BERRY_HOOK_HAVE_DOBBY
  /// Installs the real Dobby backend (only valid on-device).
  void UseDobby();
#endif

  /// Registers the game engine's hook target for later un-hooking.
  void Track(void* target, HookSymbol symbol);
  void Untrack(void* target);
  size_t ActiveCount() const;

  HookManager() = default;
  ~HookManager() = default;

 private:
  IHook* backend_ = nullptr;
  mutable std::mutex mutex_;
  std::unordered_map<void*, HookSymbol> active_;
};

//----------------------------------------------------------------------------
// VTable patching (function of an object replacing a vtable entry).
//----------------------------------------------------------------------------

class BERRY_API VTablePatch {
 public:
  VTablePatch() = default;
  VTablePatch(void* obj, size_t index, void* replacement);
  ~VTablePatch();
  VTablePatch(const VTablePatch&) = delete;
  VTablePatch& operator=(const VTablePatch&) = delete;

  bool Apply();
  void Revert();
  bool applied() const { return applied_; }
  void* original() const { return orig_; }

 private:
  void* obj_ = nullptr;
  void** vtable_ = nullptr;
  size_t index_ = 0;
  void* replacement_ = nullptr;
  void* orig_ = nullptr;
  bool applied_ = false;
};

}  // namespace Berry

//----------------------------------------------------------------------------
// Optional third-party backend glue — implemented in backend_dobby.cpp /
// backend_shim.cpp by the build.
//----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif
// Provided by the shim backend on x86-64 (host tests) —
// patches relative jumps. Returns old address to call.
typedef void (*BERRY_PatchedFn)();
BERRY_API BERRY_PatchedFn BERRY_ShimPatchInPlace(void* target, void* detour);
BERRY_API int BERRY_ShimUnpatch(void* target);
BERRY_API bool BERRY_ShimCanPatch();
#ifdef __cplusplus
}
#endif