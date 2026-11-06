#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include "berry/logger.h"
#include <sys/mman.h>
#include <unistd.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Memory utilities for the injected world: page protection for patching.
//----------------------------------------------------------------------------

namespace mem {

/// Applies a memory protection change to a page-aligned region. `oldProt`, when
/// non-null, receives the protection the region had. Returns false when the
/// current protection cannot be read or the change fails, so a caller never
/// changes a page it cannot restore.
BERRY_API bool Protect(page_t page, size_t len, int prot, int* oldProt);

/// The current protection bits of the mapping that contains `addr`, or -1 when
/// they cannot be determined. For diagnostics and tests.
BERRY_API int ProtectionOf(const void* addr);

/// RAII write window. On construction it saves the protection of *every* page in
/// [addr, addr+len) and makes them read-write-execute; the destructor restores
/// each page's own protection. If any page's protection cannot be read, nothing
/// is changed and ok() is false — a caller never writes through a window it
/// cannot undo, and a mixed r-x / r-- span is restored per page rather than
/// having one page's protection stamped over the whole range.
class BERRY_API ScopedWritable {
 public:
  ScopedWritable(void* addr, size_t len);
  ~ScopedWritable();
  ScopedWritable(const ScopedWritable&) = delete;
  ScopedWritable& operator=(const ScopedWritable&) = delete;

  bool ok() const { return ok_; }

 private:
  struct PageProt {
    page_t page;
    int prot;
  };
  std::vector<PageProt> saved_;
  bool ok_ = false;
};

/// Makes a region writable for patching, runs `fn`, restores protection. The
/// window is RAII, so a throwing `fn` still restores protection, and the
/// instruction cache for the span is flushed so freshly written code executes.
template <typename F>
inline bool WithWritable(void* addr, size_t len, F&& fn) {
  ScopedWritable w(addr, len);
  if (!w.ok()) {
    Log::Error("mem::WithWritable: cannot make %p (+%zu) writable", addr, len);
    return false;
  }
  fn();
  __builtin___clear_cache(reinterpret_cast<char*>(addr),
                          reinterpret_cast<char*>(addr) + len);
  return true;
}

}  // namespace mem

}  // namespace Berry