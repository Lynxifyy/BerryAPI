#include "berry/mem.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace Berry::mem {

namespace {

/// Parses the protection of the mapping that contains `addr` out of
/// /proc/self/maps. Returns false when the map cannot be read, so a caller
/// falls back to a safe default instead of PROT_NONE.
bool ReadProtection(uintptr_t addr, int* prot) {
  FILE* f = fopen("/proc/self/maps", "re");
  if (!f) return false;
  char line[512];
  bool found = false;
  while (fgets(line, sizeof(line), f)) {
    unsigned long start = 0, end = 0;
    char perms[8] = {0};
    if (sscanf(line, "%lx-%lx %7s", &start, &end, perms) != 3) continue;
    if (addr < start || addr >= end) continue;
    int p = 0;
    if (perms[0] == 'r') p |= PROT_READ;
    if (perms[1] == 'w') p |= PROT_WRITE;
    if (perms[2] == 'x') p |= PROT_EXEC;
    *prot = p;
    found = true;
    break;
  }
  fclose(f);
  return found;
}

}  // namespace

bool Protect(page_t page, size_t len, int prot, int* oldProt) {
  const long psz = sysconf(_SC_PAGESIZE);
  if (psz <= 0) return false;
  page_t aligned = page & ~(page_t)(psz - 1);
  size_t lenAligned = (len + psz - 1) & ~(size_t)(psz - 1);

  // Recover the current protection before changing it, so a caller can restore
  // it. Never fall back to PROT_NONE: restoring a live text page to PROT_NONE
  // would unmap it and the next execution would fault.
  int old = prot;
  if (!ReadProtection(static_cast<uintptr_t>(aligned), &old)) old = prot;

  if (mprotect(reinterpret_cast<void*>(aligned), lenAligned, prot) != 0) {
    return false;
  }
  if (oldProt) *oldProt = old;
  return true;
}

}  // namespace Berry::mem