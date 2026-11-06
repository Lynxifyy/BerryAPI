#include "berry/mem.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>

namespace Berry::mem {

namespace {

long PageSize() {
  const long ps = sysconf(_SC_PAGESIZE);
  return ps > 0 ? ps : 4096;
}

/// Parses the protection of the mapping that contains `addr` out of
/// /proc/self/maps. Returns false when the map cannot be read, so a caller
/// fails closed rather than guessing a protection to restore. Lines are read
/// without a fixed-width buffer so a long path cannot be split and its tail
/// parsed as a new mapping (bug 13).
bool ReadProtection(uintptr_t addr, int* prot) {
  FILE* f = fopen("/proc/self/maps", "re");
  if (!f) return false;
  char* line = nullptr;
  size_t cap = 0;
  bool found = false;
  while (getline(&line, &cap, f) != -1) {
    unsigned long long start = 0, end = 0;
    char perms[8] = {0};
    if (sscanf(line, "%llx-%llx %7s", &start, &end, perms) != 3) continue;
    if (addr < start || addr >= end) continue;
    int p = 0;
    if (perms[0] == 'r') p |= PROT_READ;
    if (perms[1] == 'w') p |= PROT_WRITE;
    if (perms[2] == 'x') p |= PROT_EXEC;
    *prot = p;
    found = true;
    break;
  }
  free(line);
  fclose(f);
  return found;
}

}  // namespace

int ProtectionOf(const void* addr) {
  int prot = 0;
  if (!ReadProtection(reinterpret_cast<uintptr_t>(addr), &prot)) return -1;
  return prot;
}

bool Protect(page_t page, size_t len, int prot, int* oldProt) {
  const long psz = PageSize();
  page_t aligned = page & ~(page_t)(psz - 1);
  size_t lenAligned = (len + (size_t)psz - 1) & ~((size_t)psz - 1);
  if (lenAligned == 0) return false;

  // Read the current protection first. If it cannot be read we refuse rather
  // than change a page we could not restore — the old code fell back to the
  // requested protection, which then re-applied RWX on restore (bug 12).
  int old = 0;
  if (!ReadProtection(static_cast<uintptr_t>(aligned), &old)) return false;

  if (mprotect(reinterpret_cast<void*>(aligned), lenAligned, prot) != 0) {
    return false;
  }
  if (oldProt) *oldProt = old;
  return true;
}

ScopedWritable::ScopedWritable(void* addr, size_t len) {
  if (!addr || len == 0) return;
  const long psz = PageSize();
  const page_t first = reinterpret_cast<page_t>(addr) & ~(page_t)(psz - 1);
  const page_t last =
      (reinterpret_cast<page_t>(addr) + len - 1) & ~(page_t)(psz - 1);

  // Save every page's own protection before touching any of them. A span that
  // crosses an r-x and an r-- mapping must be restored per page, not with one
  // page's protection stamped over the whole range (bug 12).
  for (page_t p = first;; p += (page_t)psz) {
    int prot = 0;
    if (!ReadProtection(p, &prot)) {
      saved_.clear();   // nothing has been changed yet
      return;
    }
    saved_.push_back({p, prot});
    if (p >= last) break;
  }

  // Add write permission to each page. On a partial failure undo what was
  // already done and fail closed, so the caller never proceeds with a
  // half-open window.
  for (size_t i = 0; i < saved_.size(); ++i) {
    if (mprotect(reinterpret_cast<void*>(saved_[i].page), (size_t)psz,
                 saved_[i].prot | PROT_WRITE) != 0) {
      for (size_t j = 0; j < i; ++j) {
        mprotect(reinterpret_cast<void*>(saved_[j].page), (size_t)psz,
                 saved_[j].prot);
      }
      saved_.clear();
      return;
    }
  }
  ok_ = true;
}

ScopedWritable::~ScopedWritable() {
  if (!ok_) return;
  const long psz = PageSize();
  for (const PageProt& s : saved_) {
    mprotect(reinterpret_cast<void*>(s.page), (size_t)psz, s.prot);
  }
}

}  // namespace Berry::mem
