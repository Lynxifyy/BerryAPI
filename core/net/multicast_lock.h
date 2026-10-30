#pragma once
// Android WifiManager multicast lock.
//
// The voice channel (and the cosmetic channel) reach peers by joining an IPv4
// multicast group. Android's Wi-Fi power management drops multicast and
// broadcast traffic to the device unless the app holds a `WifiManager.
// MulticastLock`: without one a joined group silently receives nothing, so two
// phones never hear each other. The lock is a device-only concern and needs a
// JNI handle, so it lives behind this seam:
//
//   * on Android, `Acquire()` resolves the process's JavaVM, finds the running
//     application's Context through ActivityThread, calls
//     `createMulticastLock(tag)` and `acquire()`, holding the returned lock as a
//     global reference until `Release()`;
//   * on every other platform it is a no-op that reports `available() == false`,
//     so the host build links and a host test can assert the fallback.
//
// Fail closed and idempotent: a second `Acquire()` while held is a no-op, a
// `Release()` without a lock is a no-op, and any JNI step that fails leaves the
// lock unheld (a best-effort optimisation, never a correctness dependency — the
// transports work without it, they just may not receive on a real device).
//
// This header is internal (not part of the public SDK surface) and lives under
// core/.

#include "berry/export.h"
#include "berry/types.h"

#include <string>

namespace Berry {

/// Owns one platform multicast lock for as long as it is alive. Not
/// thread-safe on its own; a transport serialises access.
class BERRY_API MulticastLock {
 public:
  MulticastLock() = default;
  ~MulticastLock();

  MulticastLock(const MulticastLock&) = delete;
  MulticastLock& operator=(const MulticastLock&) = delete;

  /// Acquires the lock under `tag` (shown in the system's Wi-Fi lock list).
  /// Returns false when it cannot be acquired — an unsupported platform, no
  /// JavaVM, no application context, or a failed JNI call. A false return is
  /// not fatal: the caller proceeds and may simply receive no multicast.
  bool Acquire(const std::string& tag = "BerryAPI");

  /// Releases the lock if held. Idempotent.
  void Release();

  bool held() const { return held_; }
  /// True when the platform supports the lock (i.e. this is Android).
  static bool available();
  /// True when the JNI handle was resolved and a lock could be created. Only
  /// meaningful after `Acquire` returned true.
  bool usable() const { return usable_; }
  /// The last error, for diagnostics.
  const std::string& lastError() const { return err_; }

 private:
  bool held_ = false;
  bool usable_ = false;
  std::string err_;
  /// Platform-private state (the JavaVM/global refs on Android). Kept opaque so
  /// this header never needs jni.h.
  struct Impl;
  Impl* impl_ = nullptr;
};

}  // namespace Berry
