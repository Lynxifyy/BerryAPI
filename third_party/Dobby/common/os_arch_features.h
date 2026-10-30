#pragma once

#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
#if defined(ANDROID)
#include <unistd.h>
#include <sys/mman.h>
#endif
#include "pac_kit.h"

#include "PlatformUnifiedInterface/platform.h"

namespace features {

template <typename T> inline T arm_thumb_fix_addr(T &addr) {
#if defined(__arm__) || defined(__aarch64__)
  addr = (T)((uintptr_t)addr & ~1);
#endif
  return addr;
}

namespace apple {
template <typename T> inline T arm64e_pac_strip(T &addr) {
  return pac_strip(addr);
}

template <typename T> inline T arm64e_pac_sign(T &addr) {
  return pac_sign(addr);
}

template <typename T> inline T arm64e_pac_strip_and_sign(T &addr) {
  return pac_strip_and_sign(addr);
}
} // namespace apple

namespace android {
inline void make_memory_readable(void *address, size_t size) {
#if defined(ANDROID)
  (void)size;
  long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0)
    return;
  uintptr_t page = (uintptr_t)address & ~((uintptr_t)page_size - 1);
  mprotect((void *)page, (size_t)page_size, PROT_READ | PROT_EXEC);
#endif
}
} // namespace android
} // namespace features