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
// Memory utilities for the injected world: page protection for patching.
//----------------------------------------------------------------------------

namespace mem {

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

}  // namespace mem

}  // namespace Berry