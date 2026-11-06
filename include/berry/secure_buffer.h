#pragma once
// A locked, zeroized buffer for key material (improvement 93).
//
// A std::string holding a key can be paged to disk or left in freed heap. This
// is a small RAII buffer that:
//
//   * allocates page-aligned storage and `mlock`s it, so the bytes are not
//     swapped out (best-effort: a process without CAP_IPC_LOCK has a small
//     RLIMIT_MEMLOCK, so a failure to lock is not fatal, only reported);
//   * zeroizes the storage with a non-elidable write on destruction;
//   * refuses to grow beyond a fixed cap, so it cannot be used as a general
//     buffer.
//
// It is deliberately not a std::string: a caller that needs to hand bytes to a
// crypto API reads `data()`/`size()` explicitly.

#include "berry/export.h"
#include "berry/types.h"

#include <cstddef>
#include <string>

namespace Berry {

/// A locked, zeroized byte buffer.
class BERRY_API SecureBuffer {
 public:
  /// `size` bytes of locked storage. `locked()` reports whether mlock succeeded.
  explicit SecureBuffer(size_t size = 0);
  ~SecureBuffer();

  SecureBuffer(const SecureBuffer&) = delete;
  SecureBuffer& operator=(const SecureBuffer&) = delete;

  /// True when the storage is mlock'ed (best-effort).
  bool locked() const { return locked_; }
  size_t size() const { return size_; }
  u8* data() { return data_; }
  const u8* data() const { return data_; }

  /// Copies `n` bytes from `src` (truncated to the buffer's size).
  void Assign(const void* src, size_t n);
  /// Fills the buffer with `n` bytes from a std::string.
  void Assign(const std::string& s) { Assign(s.data(), s.size()); }

  /// Zeroizes the bytes (called automatically on destruction).
  void Wipe();

  static constexpr size_t kMaxBytes = 4096;

 private:
  u8* data_ = nullptr;
  size_t size_ = 0;
  bool locked_ = false;
};

}  // namespace Berry
