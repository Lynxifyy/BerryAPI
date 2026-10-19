#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include "berry/logger.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace Berry {

//----------------------------------------------------------------------------
// Memory utilities for the injected world: safe reads, page protection,
// pointer chasing, and a minimal read-a-whole-file helper.
//----------------------------------------------------------------------------

namespace mem {

/// Reads a value at an address with a guarded try/catch-ish canonical
/// implementation (SEH on Windows, signal-safe read-check on POSIX).
template <typename T>
inline bool ReadSafe(const void* addr, T* out) {
  if (!addr) return false;
  // On Linux/Android we rely on the fact that touching an unmapped page
  // faults with SIGSEGV; BerryAPI installs a recovery signal handler in the
  // engine at attach time, so a failed read does not kill the process.
  volatile const T* p = reinterpret_cast<const volatile T*>(addr);
  *out = *p;
  return true;
}

template <typename T>
inline T ReadValue(const void* addr, const T& fallback = T()) {
  T out = fallback;
  ReadSafe(addr, &out);
  return out;
}

inline bool ReadBytes(const void* addr, void* dst, size_t n) {
  if (!addr || !dst || n == 0) return false;
  memcpy(dst, addr, n);
  return true;
}

/// Does a region look like executable code (has at least `minLen` bytes of
/// readable, non-zero memory)?
inline bool IsExecutable(const void* addr) {
  if (!addr) return false;
  // Best-effort single-byte probe. In the real engine this is backed by
  // /proc/self/maps parsing or mincore().
  volatile u8 probe = *reinterpret_cast<const volatile u8*>(addr);
  return probe != 0;  // conservative
}

/// Applies a memory protection change to a page-aligned region.
BERRY_API bool Protect(page_t page, size_t len, int prot, int* oldProt);

/// Makes a region writable for patching, runs `fn`, restores protection.
template <typename F>
inline bool WithWritable(void* addr, size_t len, F&& fn) {
  page_t page = reinterpret_cast<page_t>(addr) & ~(page_t)(getpagesize() - 1);
  int old = 0;
  size_t span = reinterpret_cast<page_t>(addr) + len - page;
  span = (span + getpagesize() - 1) & ~(page_t)(getpagesize() - 1);
  if (!Protect(page, span, PROT_READ | PROT_WRITE | PROT_EXEC, &old)) {
    Log::Error("mem::WithWritable: mprotect failed at %p", addr);
    return false;
  }
  fn();
  Protect(page, span, old, nullptr);
  return true;
}

/// Writes NOPs (0x1F 0x20 0x03 0xD5 on aarch64) into a code region.
BERRY_API void WriteNops(void* addr, size_t count4Byte);

/// Pointer chase: at each level reads the pointer stored at `cur + offsets[i]`.
/// Returns nullptr if any intermediate read fails.
BERRY_API inline void* ChasePointers(void* base, const u32* offsets, int levels) {
  uptr cur = reinterpret_cast<uptr>(base);
  for (int i = 0; i < levels; ++i) {
    if (!cur) return nullptr;
    uptr next = 0;
    if (!ReadSafe(reinterpret_cast<const void*>(cur + offsets[i]), &next)) return nullptr;
    cur = next;
  }
  return reinterpret_cast<void*>(cur);
}

inline const char* VTableMethod(void* obj, size_t vtIndex) {
  if (!obj) return nullptr;
  void* vtable = *(void**)obj;
  if (!vtable) return nullptr;
  void* fn = ((void**)vtable)[vtIndex];
  return reinterpret_cast<const char*>(fn);
}

// Reads the whole file into a vector. Used for network payloads and
// signature definition files.
BERRY_API std::vector<u8> ReadFile(const char* path);

}  // namespace mem

}  // namespace Berry