#include "berry/hooks.h"
#include "berry/mem.h"
#include "berry/logger.h"
#include "berry/types.h"
#include "berry/x86_length.h"

#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
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
using Berry::u16;
using Berry::u32;
using Berry::u64;
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

// Trampoline pools. Each is an anonymous mapping placed within +/-2 GiB of the
// target functions it serves, because the patch and the jmp-back are 5-byte
// `jmp rel32` instructions. A single pool mapped wherever the OS likes is out of
// rel32 range on a PIE executable (the text and the anonymous mapping are tens
// of TiB apart), so `EnsurePoolFor` searches for a free page near the target.
//
// A pool is mapped read-write while it is being filled and switched to
// read-execute before any trampoline in it can run, so no page is ever both
// writable and executable (W^X, bug 10). Slots are handed out from a free list
// and returned on unpatch, so repeated hot reloads reuse space instead of
// exhausting the pool (bug 9).
struct ShimPool {
  u8* mem = nullptr;
  bool writable = true;         // true until the first trampoline in it runs
  std::vector<u16> freeSlots;   // reusable slot indices
  u16 next = 0;                 // next never-used slot
};
std::vector<ShimPool> g_pools;

/// The largest whole-instruction prologue span we will relocate. The trampoline
/// holds 32 bytes of code plus a 5-byte jmp back, so 24 is the hard ceiling.
constexpr size_t kMaxPatchLen = 24;
constexpr size_t kTrampolineStride = sizeof(ShimTrampoline);
constexpr size_t kTrampolinePool = 4096;
constexpr size_t kSlotsPerPool = kTrampolinePool / kTrampolineStride;

long ShimPageSize() {
  const long ps = sysconf(_SC_PAGESIZE);
  return ps > 0 ? ps : 4096;
}

/// True when the whole trampoline at `mem` can be reached from `t` by a 5-byte
/// `jmp rel32` in both directions (the patch jumps to the trampoline, and the
/// trampoline's jmp-back jumps to the target).
bool SlotInRel32Range(u8* mem, uintptr_t t) {
  const uintptr_t base = reinterpret_cast<uintptr_t>(mem);
  const uintptr_t hi = base + kTrampolineStride + 5;
  const i64 loRel = (i64)base - (i64)t;
  const i64 hiRel = (i64)hi - (i64)t;
  return loRel >= INT32_MIN && hiRel <= INT32_MAX;
}

/// Reserves a free page exactly at `addr`, or returns null. MAP_FIXED_NOREPLACE
/// is only a hint on kernels before 4.17, so the returned address is compared
/// with the request and a mismatch is unmapped rather than registered (bug 7).
/// The page is mapped read-write; it is switched to read-execute when the first
/// trampoline in it is published.
u8* ReservePageAt(uintptr_t addr) {
  const long psz = ShimPageSize();
  if (addr < (uintptr_t)psz) return nullptr;
  const uintptr_t aligned = addr & ~(uintptr_t)(psz - 1);
  void* p = mmap(reinterpret_cast<void*>(aligned), kTrampolinePool,
                 PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  if (reinterpret_cast<uintptr_t>(p) != aligned) {
    // The kernel ignored the hint and mapped elsewhere: unmap and give up on
    // this address rather than register a pool outside rel32 range (bug 7).
    munmap(p, kTrampolinePool);
    return nullptr;
  }
  return static_cast<u8*>(p);
}

/// A pool with a free slot whose whole span is within rel32 of `target`.
/// Returns null when no free page is found nearby, so the caller fails closed
/// instead of writing an out-of-range jump.
ShimPool* EnsurePoolFor(void* target) {
  const uintptr_t t = reinterpret_cast<uintptr_t>(target);
  for (ShimPool& pool : g_pools) {
    if (pool.next >= kSlotsPerPool && pool.freeSlots.empty()) continue;
    if (!SlotInRel32Range(pool.mem, t)) continue;
    return &pool;
  }
  // Search outward from the target's own page for a free page. The scan is
  // capped well inside rel32 (~+/-1.75 GiB) and stops at the first free page in
  // either direction, so it does not walk the whole range on every hook.
  const uintptr_t page = (uintptr_t)ShimPageSize();
  const uintptr_t tPage = t & ~(page - 1);
  constexpr uintptr_t kMaxDelta = 0x60000000ULL;   // 1.5 GiB, inside rel32
  constexpr uintptr_t kStep = 0x100000ULL;         // 1 MiB probe step
  for (uintptr_t delta = page; delta < kMaxDelta; delta += kStep) {
    for (int dir = 0; dir < 2; ++dir) {
      uintptr_t cand = dir == 0 ? tPage + delta : tPage - delta;
      if (cand < page) continue;
      u8* p = ReservePageAt(cand);
      if (!p) continue;
      g_pools.push_back(ShimPool{});
      g_pools.back().mem = p;
      return &g_pools.back();
    }
  }
  return nullptr;
}

/// Allocates a trampoline slot in `pool` (reusing a freed one when possible).
/// The caller must hold the pool open for writing; the slot's memory is written
/// directly, so the pool must not be read-execute.
ShimTrampoline* AllocSlot(ShimPool* pool) {
  u16 idx;
  if (!pool->freeSlots.empty()) {
    idx = pool->freeSlots.back();
    pool->freeSlots.pop_back();
  } else {
    if (pool->next >= kSlotsPerPool) return nullptr;
    idx = pool->next++;
  }
  return reinterpret_cast<ShimTrampoline*>(pool->mem +
                                           (size_t)idx * kTrampolineStride);
}

/// Returns a slot to its pool's free list. `tr` is not dereferenced.
void FreeSlot(ShimTrampoline* tr) {
  for (ShimPool& pool : g_pools) {
    const uintptr_t base = reinterpret_cast<uintptr_t>(pool.mem);
    const uintptr_t a = reinterpret_cast<uintptr_t>(tr);
    if (a < base || a >= base + kTrampolinePool) continue;
    const size_t idx = (a - base) / kTrampolineStride;
    if ((a - base) % kTrampolineStride == 0 && idx < kSlotsPerPool) {
      pool.freeSlots.push_back((u16)idx);
    }
    return;
  }
}

/// A rel32 field inside the overwritten prologue that must be rebased when the
/// span moves to a trampoline. `field` is the byte offset of the field; `insnEnd`
/// is the offset just past the instruction that owns it, which is the base the
/// CPU computes a RIP-relative operand or a relative branch from — NOT the field
/// plus four (bug 1: for `cmp dword [rip+x], imm8` the field is not the last
/// thing in the instruction).
struct PrologueReloc {
  size_t field;
  size_t insnEnd;
};

/// How many whole instructions the detour must overwrite so at least 5 bytes are
/// covered, plus the rel32 fields inside that span that must be rebased when the
/// span moves to a trampoline. Returns 0 when the prologue cannot be decoded,
/// contains a rel8 branch (whose 127-byte reach cannot be re-created), or ends
/// in a terminal instruction before five bytes are covered (bug 3: patching past
/// a `ret` would overwrite the neighbouring function) — so a `jmp rel32` is never
/// written over a partial instruction.
size_t DecodePrologue(const u8* target, std::vector<PrologueReloc>* relocs) {
  size_t total = 0;
  while (total < 5) {
    const X86InsnInfo info = X86DecodeInsn(target + total, 16);
    if (!info.ok()) return 0;
    if (info.rel8Branch) return 0;   // cannot relocate a rel8 branch
    if (info.relocSize == 4)
      relocs->push_back({total + info.relocOffset, total + info.length});
    total += info.length;
    if (info.terminal) {
      // Control does not fall through here. If the terminal instruction did not
      // bring the span to five bytes there is nothing safe to patch.
      return total >= 5 ? total : 0;
    }
    if (total > kMaxPatchLen) return 0;   // pathological; refuse rather than guess
  }
  return total;
}

/// True when a branch somewhere in `span` targets a byte strictly inside the
/// overwritten range (offset 1..span-1). Such a branch would land in the middle
/// of the `jmp rel32` we install, so the patch is refused (bug 4). The scan is a
/// linear disassembly of the span; it cannot see a branch from elsewhere that
/// targets the middle, which is why the caller additionally verifies the
/// installed jump and documents the residual risk.
bool IntraSpanBranch(const u8* span, size_t len) {
  size_t i = 0;
  while (i < len) {
    const X86InsnInfo info = X86DecodeInsn(span + i, 16);
    if (!info.ok() || info.length == 0 || i + info.length > len) return false;
    if (info.relative && !info.rel8Branch && info.relocSize == 4) {
      i32 disp = 0;
      memcpy(&disp, span + i + info.relocOffset, 4);
      const i64 next = (i64)i + (i64)info.length;
      const i64 dest = next + disp;
      if (dest > 0 && dest < (i64)len) return true;   // into the middle
    }
    i += info.length;
  }
  return false;
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

//--- Per-hook accounting (improvement 9). Kept outside the patch map so a
//    caller can read the counters without taking g_shimMutex.
struct ShimStats {
  std::atomic<u64> calls{0};
  std::atomic<u64> faults{0};
};
std::map<void*, ShimStats> g_stats;

/// Switches a pool to read-execute once its trampolines are published, so a
/// pool page is never both writable and executable while code in it can run
/// (bug 10). Returns false if the change failed.
bool MakePoolExecutable(ShimPool* pool) {
  if (!pool->writable) return true;
  if (mprotect(pool->mem, kTrampolinePool, PROT_READ | PROT_EXEC) != 0)
    return false;
  pool->writable = false;
  return true;
}

/// Reopens a pool read-write for the short window in which a trampoline is
/// written. The pool is switched back to read-execute before the patch that
/// points at the new trampoline is published, so the executable image is never
/// writable at the moment it becomes reachable. Returns false on failure.
bool MakePoolWritable(ShimPool* pool) {
  if (pool->writable) return true;
  if (mprotect(pool->mem, kTrampolinePool, PROT_READ | PROT_WRITE) != 0)
    return false;
  pool->writable = true;
  return true;
}

/// Fills `tr` with the relocated prologue and the jmp-back. Returns false when
/// the prologue cannot be relocated.
bool BuildTrampoline(void* target, ShimTrampoline* tr,
                     std::vector<PrologueReloc>* relocs, size_t* patchLenOut) {
  const u8* code = static_cast<const u8*>(target);
  const size_t patchLen = DecodePrologue(code, relocs);
  if (patchLen < 5 || patchLen > kMaxPatchLen) return false;
  if (IntraSpanBranch(code, patchLen)) return false;   // bug 4

  u8* trampBytes = tr->bytes;
  // Keep the pristine bytes for restoration: the relocated copy has its rel32
  // fields rebased and must never be written back over the target.
  memcpy(tr->original, code, patchLen);
  memcpy(trampBytes, code, patchLen);
  for (const PrologueReloc& r : *relocs) {
    i32 disp = 0;
    memcpy(&disp, code + r.field, 4);
    // The displacement is relative to the END of the instruction that owns the
    // field (instruction start + length), which is the base the CPU uses for a
    // RIP-relative operand and for a relative branch.
    i64 origNext = reinterpret_cast<i64>(code + r.insnEnd) + disp;
    i64 newDisp = origNext - reinterpret_cast<i64>(trampBytes + r.insnEnd);
    if (newDisp < INT32_MIN || newDisp > INT32_MAX) return false;
    i32 nd = static_cast<i32>(newDisp);
    memcpy(trampBytes + r.field, &nd, 4);
  }
  // jmp-back to target + patchLen, verifying it fits in rel32.
  u8* jmpBack = trampBytes + patchLen;
  if (!RelJmpInRange(jmpBack, static_cast<u8*>(target) + patchLen)) return false;
  WriteRelJmp(jmpBack, static_cast<u8*>(target) + patchLen);
  *patchLenOut = patchLen;
  return true;
}

/// Publishes the patch. The target is made writable and the `jmp rel32` is
/// written with an int3-first protocol so a thread executing the prologue never
/// observes a torn jump: byte 0 is set to 0xCC first, the rest of the jump is
/// written, then byte 0 is swapped to 0xE9. The caller then re-reads the target
/// and refuses if the bytes are not what was intended (bug 5). Returns false
/// without leaving a partial patch when the write window cannot be opened.
bool InstallJump(void* target, void* detour, size_t patchLen) {
  // Pre-compute the full byte image so the window only ever contains stores.
  u8 image[kMaxPatchLen];
  image[0] = 0xE9;
  i64 rel = reinterpret_cast<i64>(detour) - (reinterpret_cast<i64>(target) + 5);
  if (rel < INT32_MIN || rel > INT32_MAX) return false;
  i32 rel32 = static_cast<i32>(rel);
  memcpy(image + 1, &rel32, 4);
  memset(image + 5, 0x90, patchLen - 5);

  u8* p = static_cast<u8*>(target);
  bool ok = Berry::mem::WithWritable(target, patchLen, [&]() {
    p[0] = 0xCC;                       // breakpoint guards the torn write
    __builtin___clear_cache((char*)p, (char*)p + 1);
    memcpy(p + 1, image + 1, patchLen - 1);
    __builtin___clear_cache((char*)p + 1, (char*)p + patchLen);
    p[0] = image[0];
  });
  if (!ok) return false;

  // Verify: a caller must never believe a hook is installed when it is not
  // (bug 5). If the bytes do not match, the patch did not take.
  return memcmp(p, image, patchLen) == 0;
}

/// Restores the pristine prologue bytes, but only when the target still holds
/// our jump — a hook layered on top later must not be clobbered (bug 9).
bool RestoreOriginal(void* target, ShimTrampoline* tr) {
  u8* p = static_cast<u8*>(target);
  if (p[0] != 0xE9) return false;   // someone else owns the prologue now
  return Berry::mem::WithWritable(target, tr->patchLen, [&]() {
    memcpy(p, tr->original, tr->patchLen);
  });
}

}  // namespace

extern "C" {

BERRY_API bool BERRY_ShimCanPatch() {
  // The shim can always patch on x86-64; the per-target pool is allocated on
  // demand in BERRY_ShimPatchInPlace.
  return true;
}

BERRY_API BERRY_PatchedFn BERRY_ShimPatchInPlace(void* target, void* detour) {
  if (!BERRY_ShimCanPatch() || !target || !detour) return nullptr;
  std::lock_guard<std::mutex> lk(g_shimMutex);
  if (g_patches.find(target) != g_patches.end()) return nullptr;

  // A trampoline pool within rel32 of the target, so both the patch and the
  // jmp-back fit in a 5-byte relative jump.
  ShimPool* pool = EnsurePoolFor(target);
  if (!pool) return nullptr;

  // Write the trampoline in a read-write window, record the bookkeeping while
  // the pool is still writable, then switch the pool to read-execute *before*
  // the patch that reaches it is published. The code is therefore never both
  // writable and reachable (bug 10), and no store ever targets an RX page.
  if (!MakePoolWritable(pool)) return nullptr;
  ShimTrampoline* tr = AllocSlot(pool);
  std::vector<PrologueReloc> relocs;
  size_t patchLen = 0;
  const bool built = tr && BuildTrampoline(target, tr, &relocs, &patchLen);
  if (built) {
    tr->len = patchLen;
    tr->patchLen = patchLen;
  }
  const bool executable = MakePoolExecutable(pool);
  if (!built || !executable) {
    if (tr) FreeSlot(tr);
    return nullptr;
  }

  if (!InstallJump(target, detour, patchLen)) {
    FreeSlot(tr);
    return nullptr;
  }

  g_patches[target] = tr;
  g_stats[target];            // create the counter entry
  return reinterpret_cast<BERRY_PatchedFn>(tr->bytes);
}

BERRY_API int BERRY_ShimUnpatch(void* target) {
  std::lock_guard<std::mutex> lk(g_shimMutex);
  auto it = g_patches.find(target);
  if (it == g_patches.end()) return -1;
  // Restore the original whole-instruction bytes, but only if the target still
  // holds our jump.
  if (!RestoreOriginal(target, it->second)) return -1;
  FreeSlot(it->second);
  g_patches.erase(it);
  return 0;
}

BERRY_API void BERRY_ShimHookStats(void* target, unsigned long long* calls,
                                   unsigned long long* faults) {
  if (calls) *calls = 0;
  if (faults) *faults = 0;
  std::lock_guard<std::mutex> lk(g_shimMutex);
  auto it = g_stats.find(target);
  if (it == g_stats.end()) return;
  if (calls) *calls = it->second.calls.load();
  if (faults) *faults = it->second.faults.load();
}

BERRY_API int BERRY_ShimListHooks(BERRY_ShimHookInfo* out, int maxOut) {
  if (!out || maxOut <= 0) return 0;
  std::lock_guard<std::mutex> lk(g_shimMutex);
  int n = 0;
  for (const auto& kv : g_patches) {
    if (n >= maxOut) break;
    out[n].target = kv.first;
    out[n].trampoline = kv.second->bytes;
    out[n].patchLen = kv.second->patchLen;
    out[n].calls = 0;
    out[n].faults = 0;
    auto sit = g_stats.find(kv.first);
    if (sit != g_stats.end()) {
      out[n].calls = sit->second.calls.load();
      out[n].faults = sit->second.faults.load();
    }
    ++n;
  }
  return n;
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
  // The backend pointer is read and (re)assigned under its own lock so a
  // BackendEmulateShim on another thread cannot delete it under a user (bug 11).
  std::lock_guard<std::mutex> lk(backendMutex_);
  if (!backend_) {
    backend_ = new ShimBackend();
  }
  return backend_;
}

void HookManager::BackendEmulateShim() {
  std::lock_guard<std::mutex> lk(backendMutex_);
  delete backend_;
  backend_ = new ShimBackend();
}

#ifdef BERRY_HOOK_HAVE_DOBBY
// UseDobby() is declared in hooks.h and defined in backend_dobby.cpp; it is
// never defined on the host shim build. An out-of-line redeclaration here
// would be a qualified declaration with no definition, which clang rejects.
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
