#pragma once
// A per-script secret store (improvement 65).
//
// A script sometimes needs a token (a relay room secret, an API key). Storing
// it in the script source or a manifest is unsafe, so this is a small
// namespaced key/value store the embedder backs with the platform keystore (or
// an in-memory map on the host). It is:
//
//   * namespaced per script, so one script can never read another's secret;
//   * bounded (value length and entry count), so a script cannot use it as a
//     general data store;
//   * wiped on Forget, so an unloaded script leaves no secret behind.
//
// The actual persistence is behind the `ISecretBackend` seam; the host test
// uses an in-memory backend, and a device build implements it over the
// platform keystore.

#include "berry/export.h"
#include "berry/types.h"

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// The persistence seam. A device build implements this over the platform
/// keystore; the host uses `MemorySecretBackend`.
class BERRY_API ISecretBackend {
 public:
  virtual ~ISecretBackend() = default;
  /// Reads a value. False when the key is absent.
  virtual bool Read(const std::string& ns, const std::string& key,
                    std::string* out) = 0;
  /// Writes a value. Returns false on a backend failure.
  virtual bool Write(const std::string& ns, const std::string& key,
                     const std::string& value) = 0;
  /// Deletes a key. Returns true when it existed.
  virtual bool Erase(const std::string& ns, const std::string& key) = 0;
  /// Lists the keys in a namespace.
  virtual std::vector<std::string> Keys(const std::string& ns) = 0;
};

/// An in-memory backend (the host default).
class BERRY_API MemorySecretBackend : public ISecretBackend {
 public:
  bool Read(const std::string& ns, const std::string& key,
            std::string* out) override;
  bool Write(const std::string& ns, const std::string& key,
             const std::string& value) override;
  bool Erase(const std::string& ns, const std::string& key) override;
  std::vector<std::string> Keys(const std::string& ns) override;
  void Clear();

 private:
  std::map<std::string, std::map<std::string, std::string>> data_;
  std::mutex mutex_;
};

/// The per-script facade over a backend.
class BERRY_API SecretStore {
 public:
  /// `backend` is borrowed and must outlive the store.
  explicit SecretStore(ISecretBackend* backend) : backend_(backend) {}

  /// Stores `value` under `key` for `script`. Refuses an empty/over-long key or
  /// value, or a store over its per-script entry cap.
  bool Put(const std::string& script, const std::string& key,
           const std::string& value);
  /// Reads a secret. False when absent.
  bool Get(const std::string& script, const std::string& key,
           std::string* out) const;
  /// Removes a key. True when it existed.
  bool Erase(const std::string& script, const std::string& key);
  /// The keys a script has stored.
  std::vector<std::string> Keys(const std::string& script) const;
  /// Removes every secret a script stored (on unload).
  size_t Forget(const std::string& script);

  static constexpr size_t kMaxKeyLen = 64;
  static constexpr size_t kMaxValueLen = 4096;
  static constexpr size_t kMaxEntriesPerScript = 32;

 private:
  ISecretBackend* backend_;
};

}  // namespace Berry
