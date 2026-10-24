#include "berry/mem.h"
#include <unistd.h>

namespace Berry::mem {

bool Protect(page_t page, size_t len, int prot, int* oldProt) {
  const long psz = sysconf(_SC_PAGESIZE);
  if (psz <= 0) return false;
  page_t aligned = page & ~(page_t)(psz - 1);
  size_t lenAligned = (len + psz - 1) & ~(size_t)(psz - 1);
  int old = 0;
  if (mprotect((void*)aligned, lenAligned, prot) != 0) return false;
  if (oldProt) *oldProt = old;  // cannot recover old flags on POSIX w/o /proc
  return true;
}

}  // namespace Berry::mem