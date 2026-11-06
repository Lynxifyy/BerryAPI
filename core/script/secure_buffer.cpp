// Locked, zeroized key buffer. See include/berry/secure_buffer.h.
#include "berry/secure_buffer.h"

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/mman.h>
#endif

namespace Berry {

SecureBuffer::SecureBuffer(size_t size) {
  if (size == 0) return;
  if (size > kMaxBytes) size = kMaxBytes;
  const long psz = sysconf(_SC_PAGESIZE);
  const size_t page = psz > 0 ? (size_t)psz : 4096;
  const size_t alloc = (size + page - 1) & ~(page - 1);
  // Page-aligned so mlock can lock whole pages.
  void* p = nullptr;
  if (posix_memalign(&p, page, alloc) != 0) return;
  data_ = static_cast<u8*>(p);
  size_ = size;
#if defined(__linux__) || defined(__APPLE__)
  // Best-effort: without CAP_IPC_LOCK a small process may not be able to lock,
  // which is reported but not fatal (the zeroize still happens).
  locked_ = mlock(data_, alloc) == 0;
#endif
  memset(data_, 0, size_);
}

SecureBuffer::~SecureBuffer() {
  Wipe();
  if (data_) {
#if defined(__linux__) || defined(__APPLE__)
    if (locked_) munlock(data_, size_);
#endif
    free(data_);
  }
}

void SecureBuffer::Wipe() {
  if (!data_ || size_ == 0) return;
  // A volatile pointer so the zeroing is not optimized away.
  volatile u8* p = data_;
  for (size_t i = 0; i < size_; ++i) p[i] = 0;
}

void SecureBuffer::Assign(const void* src, size_t n) {
  if (!data_ || !src) return;
  if (n > size_) n = size_;
  memcpy(data_, src, n);
}

}  // namespace Berry
