#include "berry/mem.h"
#include <unistd.h>
#include <fstream>
#include <iterator>

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

void WriteNops(void* addr, size_t count4Byte) {
  // aarch64 NOP = 0xD503201F (little-endian bytes: 1F 20 03 D5)
  const u32 nop = 0xD503201FU;
  u32* p = (u32*)addr;
  for (size_t i = 0; i < count4Byte; ++i) p[i] = nop;
}

std::vector<u8> ReadFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return {};
  return std::vector<u8>((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
}

}  // namespace Berry::mem