#include "berry/hooks.h"
#include "berry/mem.h"
#include "berry/logger.h"
#include "berry/types.h"
#include "berry/x86_length.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

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
using Berry::X86InsnInfo;
using Berry::X86DecodeInsn;

namespace {

struct ShimTrampoline {
  // The relocated prologue (up to 5 + 14 bytes) followed by a 5-byte jmp back
  // to target+N. `original` keeps the pristine bytes for restoration — the
  // relocated copy has its rel32 fields rebased and must never be written back
  // over the target.
  u8 bytes[32];
  u8 original[32];
  size_t len = 0;          // relocated instruction bytes
  size_t patchLen = 0;     // bytes overwritten at the target (>= 5)
};

std::mutex g_shimMutex;
std::map<void*, ShimTrampoline*> g_patches;

// Trampoline pools. Each is an RWX mapping placed within +/-2 GiB of the target
// functions it serves, because the patch and the jmp-back are 5-byte `jmp rel32`
// instructions. A single pool mapped wherever the OS likes is out of rel32 range
// on a PIE executable (the text and the anonymous mapping are tens of TiB
// apart), so `EnsurePoolFor` searches for a free page near the target.
struct ShimPool {
  u8* mem = nullptr;
  size_t offset = 0;
};
std::vector<ShimPool> g_pools;
constexpr size_t kTrampolinePool = 4096;
constexpr size_t kTrampolineStride = sizeof(ShimTrampoline);

/// A pool with room for one more trampoline whose whole span is within rel32 of
/// `target`. Returns null when no free page is found nearby (so the caller
/// fails closed instead of writing an out-of-range jump).
ShimPool* EnsurePoolFor(void* target) {
  const uintptr_t t = reinterpret_cast<uintptr_t>(target);
  for (ShimPool& pool : g_pools) {
    if (pool.offset + kTrampolineStride > kTrampolinePool) continue;
    const uintptr_t base = reinterpret_cast<uintptr_t>(pool.mem);
    const uintptr_t hi = base + pool.offset + kTrampolineStride + 5;
    const i64 loRel = (i64)base - (i64)t;
    const i64 hiRel = (i64)hi - (i64)t;
    if (loRel >= INT32_MIN && hiRel <= INT32_MAX) return &pool;
  }
  // Search outward from the target's own page for a free page, and reserve it
  // atomically with MAP_FIXED_NOREPLACE (so an existing mapping is never
  // clobbered). The scan covers roughly +/-1.75 GiB, inside rel32.
  const uintptr_t page = 0x1000;
  const uintptr_t tPage = t & ~(uintptr_t)(page - 1);
  for (uintptr_t delta = page; delta < 0x70000000ULL; delta += page) {
    for (int dir = 0; dir < 2; ++dir) {
      uintptr_t cand = dir == 0 ? tPage + delta : tPage - delta;
      if (cand < page) continue;
      void* p = mmap(reinterpret_cast<void*>(cand), kTrampolinePool,
                     PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
      if (p == MAP_FAILED) continue;
      g_pools.push_back(ShimPool{static_cast<u8*>(p), 0});
      return &g_pools.back();
    }
  }
  return nullptr;
}

/// How many whole instructions the detour must overwrite so at least 5 bytes are
/// covered, plus the offsets of the rel32 fields inside that span that must be
/// rebased when the span moves to a trampoline. Returns 0 when the prologue
/// cannot be decoded or contains a rel8 branch (whose 127-byte reach cannot be
/// re-created), so a `jmp rel32` is never written over a partial instruction.
size_t DecodePrologue(const u8* target, std::vector<size_t>* relocs) {
  size_t total = 0;
  while (total < 5) {
    const X86InsnInfo info = X86DecodeInsn(target + total, 16);
    if (!info.ok()) return 0;
    if (info.rel8Branch) return 0;   // cannot relocate a rel8 branch
    if (info.relocSize == 4) relocs->push_back(total + info.relocOffset);
    total += info.length;
    if (total > 20) return 0;        // pathological; refuse rather than guess
  }
  return total;
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
  // The shim can always patch on x86-64; the per-target pool is allocated on
  // demand in BERRY_ShimPatchInPlace.
  return true;
}

BERRY_API BERRY_PatchedFn BERRY_ShimPatchInPlace(void* target, void* detour) {
  if (!BERRY_ShimCanPatch()) return nullptr;
  std::lock_guard<std::mutex> lk(g_shimMutex);
  if (g_patches.find(target) != g_patches.end()) return nullptr;

  const u8* code = static_cast<const u8*>(target);
  std::vector<size_t> relocs;
  const size_t patchLen = DecodePrologue(code, &relocs);
  if (patchLen < 5 || patchLen > sizeof(ShimTrampoline::bytes) - 5) return nullptr;

  // A trampoline pool within rel32 of the target, so both the patch and the
  // jmp-back fit in a 5-byte relative jump.
  ShimPool* pool = EnsurePoolFor(target);
  if (!pool) return nullptr;
  auto* tr = reinterpret_cast<ShimTrampoline*>(pool->mem + pool->offset);

  // 1. Relocate the whole instructions the detour overwrites, rebasing every
  //    rel32 field (RIP-relative operands, rel32 branches) to its new address.
  memcpy(tr->bytes, code, patchLen);
  memcpy(tr->original, code, patchLen);
  for (size_t off : relocs) {
    i32 disp = 0;
    memcpy(&disp, code + off, 4);
    // The field's value is relative to the end of the *original* instruction;
    // recompute it against the trampoline's copy.
    i64 origNext = reinterpret_cast<i64>(code + off + 4) + disp;
    i64 newDisp = origNext - reinterpret_cast<i64>(tr->bytes + off + 4);
    if (newDisp < INT32_MIN || newDisp > INT32_MAX) return nullptr;
    i32 nd = static_cast<i32>(newDisp);
    memcpy(tr->bytes + off, &nd, 4);
  }
  tr->len = patchLen;
  tr->patchLen = patchLen;
  // 2. Emit jmp-back (target+patchLen), verifying it fits in rel32.
  u8* jmpBack = tr->bytes + patchLen;
  if (!RelJmpInRange(jmpBack, static_cast<u8*>(target) + patchLen)) return nullptr;
  WriteRelJmp(jmpBack, static_cast<u8*>(target) + patchLen);
  // 3. Patch the target: jmp detour over the same whole instructions. Pad the
  //    remaining bytes of the span with NOPs (0x90) so the code stream stays
  //    valid if control somehow reaches them.
  Berry::mem::WithWritable(target, patchLen, [&]() {
    WriteRelJmp(target, detour);
    memset(static_cast<u8*>(target) + 5, 0x90, patchLen - 5);
  });

  g_patches[target] = tr;
  pool->offset += kTrampolineStride;
  return reinterpret_cast<BERRY_PatchedFn>(tr->bytes);
}

BERRY_API int BERRY_ShimUnpatch(void* target) {
  std::lock_guard<std::mutex> lk(g_shimMutex);
  auto it = g_patches.find(target);
  if (it == g_patches.end()) return -1;
  // Restore the original whole-instruction bytes.
  Berry::mem::WithWritable(target, it->second->patchLen, [&]() {
    memcpy(target, it->second->original, it->second->patchLen);
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