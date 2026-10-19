#include "berry/hooks.h"
#include "berry/mem.h"
#include "berry/logger.h"
#include "berry/types.h"

#include <dlfcn.h>
#include <cstring>
#include <map>
#include <mutex>

//----------------------------------------------------------------------------
// Shim backend — a real hotpatch detour backend for x86-64.
//
// The first 5 bytes of `target` are replaced with a relative JMP to `detour`.
// The original 5 bytes are relocated into trampoline memory followed by a
// relative jump back to target+5, so calling through the trampoline executes
// the original prologue and then returns to the natural flow. This is the
// same technique used by hotpatch libraries; it is fully unit-testable on the
// desktop harness without a game binary.
//----------------------------------------------------------------------------

using Berry::u8;
using Berry::u32;
using Berry::i32;
using Berry::i64;

namespace {

struct ShimTrampoline {
  u8 patch[5];    // original 5 bytes
  u8 jmp_back[5]; // jmp target+5
};

std::mutex g_shimMutex;
std::map<void*, ShimTrampoline*> g_patches;

void* g_shimTrampolineMem = nullptr;
size_t g_shimTrampolineOffset = 0;
constexpr size_t kTrampolinePool = 4096;

bool EnsurePool() {
  if (g_shimTrampolineMem) return true;
  // mmap with PROT_EXEC so the relocated jmp-back code is runnable.
  g_shimTrampolineMem = mmap(nullptr, kTrampolinePool,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  return g_shimTrampolineMem != MAP_FAILED && g_shimTrampolineMem != nullptr;
}

void WriteRelJmp(void* at, void* dest) {
  u8* p = static_cast<u8*>(at);
  p[0] = 0xE9;  // jmp rel32
  i64 rel = reinterpret_cast<i64>(dest) - (reinterpret_cast<i64>(at) + 5);
  i32 rel32 = static_cast<i32>(rel);
  memcpy(p + 1, &rel32, 4);
}

bool RelJmpInRange(void* at, void* dest) {
  i64 rel = reinterpret_cast<i64>(dest) - (reinterpret_cast<i64>(at) + 5);
  return rel >= INT32_MIN && rel <= INT32_MAX;
}

}  // namespace

extern "C" {

BERRY_API bool BERRY_ShimCanPatch() {
  return EnsurePool();
}

BERRY_API BERRY_PatchedFn BERRY_ShimPatchInPlace(void* target, void* detour) {
  if (!BERRY_ShimCanPatch()) return nullptr;
  std::lock_guard<std::mutex> lk(g_shimMutex);
  if (g_patches.find(target) != g_patches.end()) return nullptr;

  auto* tr = reinterpret_cast<ShimTrampoline*>(
      static_cast<u8*>(g_shimTrampolineMem) + g_shimTrampolineOffset);

  // 1. Copy original 5 bytes into trampoline.
  memcpy(tr->patch, target, sizeof(tr->patch));
  // 2. Emit jmp-back (target+5), verifying the jmp fits in rel32.
  if (!RelJmpInRange(tr->jmp_back, static_cast<u8*>(target) + 5)) return nullptr;
  if (g_shimTrampolineOffset + sizeof(ShimTrampoline) > kTrampolinePool) return nullptr;
  WriteRelJmp(tr->jmp_back, static_cast<u8*>(target) + 5);
  // 3. Patch the target: jmp detour.
  size_t span = 5;
  Berry::mem::WithWritable(target, span, [&]() {
    WriteRelJmp(target, detour);
  });

  g_patches[target] = tr;
  g_shimTrampolineOffset += sizeof(ShimTrampoline);
  return reinterpret_cast<BERRY_PatchedFn>(tr->patch);
}

BERRY_API int BERRY_ShimUnpatch(void* target) {
  std::lock_guard<std::mutex> lk(g_shimMutex);
  auto it = g_patches.find(target);
  if (it == g_patches.end()) return -1;
  // Restore the original 5 bytes.
  Berry::mem::WithWritable(target, 5, [&]() {
    memcpy(target, it->second->patch, 5);
  });
  g_patches.erase(it);
  return 0;
}

}  // extern "C"

namespace Berry {

class ShimBackend : public IHook {
 public:
  const char* BackendName() const override { return "BerryShim"; }
  bool CanHook() const override { return BERRY_ShimCanPatch(); }

  HookStatus Install(void* target, VoidFn detour, HookSymbol* out) override {
    if (!target || !detour) return HookStatus::NoTarget;
    if (IsHooked(target)) return HookStatus::AlreadyHooked;
    auto* tramp = BERRY_ShimPatchInPlace(target, reinterpret_cast<void*>(detour));
    if (!tramp) return HookStatus::BackendError;
    *out = HookSymbol{target, (void*)detour, (void*)tramp, true};
    return HookStatus::Ok;
  }

  HookStatus Uninstall(void* target) override {
    if (BERRY_ShimUnpatch(target) == 0) return HookStatus::Ok;
    return HookStatus::NoTarget;
  }

  bool IsHooked(void* target) const override {
    std::lock_guard<std::mutex> lk(const_cast<std::mutex&>(g_shimMutex));
    return g_patches.find(target) != g_patches.end();
  }
};

HookManager& HookManager::Instance() {
  static HookManager hm;
  return hm;
}

IHook* HookManager::Backend() {
  if (!backend_) {
    backend_ = new ShimBackend();
  }
  return backend_;
}

void HookManager::BackendEmulateShim() {
  delete backend_;
  backend_ = new ShimBackend();
}

#ifdef BERRY_HOOK_HAVE_DOBBY
// Implemented in backend_dobby.cpp — never defined on the host shim build.
void HookManager::UseDobby();
#endif

void HookManager::Track(void* target, HookSymbol symbol) {
  std::lock_guard<std::mutex> lk(mutex_);
  active_[target] = symbol;
}

void HookManager::Untrack(void* target) {
  std::lock_guard<std::mutex> lk(mutex_);
  active_.erase(target);
}

size_t HookManager::ActiveCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return active_.size();
}

//----------------------------------------------------------------------------
// VTablePatch
//----------------------------------------------------------------------------

VTablePatch::VTablePatch(void* obj, size_t index, void* replacement)
    : obj_(obj), index_(index), replacement_(replacement) {
  if (obj_) vtable_ = *(void***)obj_;
}

VTablePatch::~VTablePatch() {
  Revert();
}

bool VTablePatch::Apply() {
  if (applied_ || !vtable_ || !replacement_) return false;
  orig_ = vtable_[index_];
  Berry::mem::WithWritable(&vtable_[index_], sizeof(void*), [&]() {
    vtable_[index_] = replacement_;
  });
  applied_ = true;
  return true;
}

void VTablePatch::Revert() {
  if (!applied_ || !vtable_ || !orig_) return;
  Berry::mem::WithWritable(&vtable_[index_], sizeof(void*), [&]() {
    vtable_[index_] = orig_;
  });
  applied_ = false;
}

}  // namespace Berry