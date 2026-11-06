// Per-script secret store. See include/berry/secret_store.h.
#include "berry/secret_store.h"

namespace Berry {

bool MemorySecretBackend::Read(const std::string& ns, const std::string& key,
                               std::string* out) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = data_.find(ns);
  if (it == data_.end()) return false;
  auto k = it->second.find(key);
  if (k == it->second.end()) return false;
  *out = k->second;
  return true;
}

bool MemorySecretBackend::Write(const std::string& ns, const std::string& key,
                                const std::string& value) {
  std::lock_guard<std::mutex> lk(mutex_);
  data_[ns][key] = value;
  return true;
}

bool MemorySecretBackend::Erase(const std::string& ns, const std::string& key) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = data_.find(ns);
  if (it == data_.end()) return false;
  return it->second.erase(key) > 0;
}

std::vector<std::string> MemorySecretBackend::Keys(const std::string& ns) {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<std::string> out;
  auto it = data_.find(ns);
  if (it == data_.end()) return out;
  for (const auto& kv : it->second) out.push_back(kv.first);
  return out;
}

void MemorySecretBackend::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  data_.clear();
}

bool SecretStore::Put(const std::string& script, const std::string& key,
                      const std::string& value) {
  if (!backend_ || script.empty()) return false;
  if (key.empty() || key.size() > kMaxKeyLen) return false;
  if (value.size() > kMaxValueLen) return false;
  // Enforce the per-script entry cap before adding a new key.
  const std::vector<std::string> existing = backend_->Keys(script);
  bool isNew = true;
  for (const std::string& k : existing) {
    if (k == key) { isNew = false; break; }
  }
  if (isNew && existing.size() >= kMaxEntriesPerScript) return false;
  return backend_->Write(script, key, value);
}

bool SecretStore::Get(const std::string& script, const std::string& key,
                      std::string* out) const {
  if (!backend_ || !out || script.empty() || key.empty()) return false;
  return backend_->Read(script, key, out);
}

bool SecretStore::Erase(const std::string& script, const std::string& key) {
  if (!backend_ || script.empty() || key.empty()) return false;
  return backend_->Erase(script, key);
}

std::vector<std::string> SecretStore::Keys(const std::string& script) const {
  if (!backend_ || script.empty()) return {};
  return backend_->Keys(script);
}

size_t SecretStore::Forget(const std::string& script) {
  if (!backend_ || script.empty()) return 0;
  const std::vector<std::string> keys = backend_->Keys(script);
  size_t n = 0;
  for (const std::string& k : keys) {
    if (backend_->Erase(script, k)) ++n;
  }
  return n;
}

}  // namespace Berry
