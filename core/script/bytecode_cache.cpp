// Compiled-bytecode cache. See include/berry/bytecode_cache.h.
#include "berry/bytecode_cache.h"

#include "berry/manifest.h"   // crypto::Sha256Hex

namespace Berry {

std::string BytecodeCache::KeyFor(const std::string& source) {
  return crypto::Sha256Hex(source);
}

bool BytecodeCache::Get(const std::string& source, std::string* bytecode) {
  if (!bytecode) return false;
  const std::string key = KeyFor(source);
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = index_.find(key);
  if (it == index_.end()) {
    ++misses_;
    return false;
  }
  // Move to the front (most-recently-used).
  lru_.splice(lru_.begin(), lru_, it->second);
  *bytecode = it->second->bytecode;
  ++hits_;
  return true;
}

void BytecodeCache::Put(const std::string& source, const std::string& bytecode) {
  if (bytecode.empty()) return;
  if (bytecode.size() > maxBytes_) return;   // too big to ever fit
  const std::string key = KeyFor(source);
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = index_.find(key);
  if (it != index_.end()) {
    // Replace, keeping the most-recent position.
    bytes_ -= it->second->bytecode.size();
    it->second->bytecode = bytecode;
    bytes_ += bytecode.size();
    lru_.splice(lru_.begin(), lru_, it->second);
    EvictLocked();
    return;
  }
  lru_.push_front(Entry{key, bytecode});
  bytes_ += bytecode.size();
  index_[key] = lru_.begin();
  EvictLocked();
}

void BytecodeCache::EvictLocked() {
  while ((lru_.size() > maxEntries_ || bytes_ > maxBytes_) && !lru_.empty()) {
    auto last = std::prev(lru_.end());
    bytes_ -= last->bytecode.size();
    index_.erase(last->key);
    lru_.erase(last);
  }
}

size_t BytecodeCache::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return lru_.size();
}

size_t BytecodeCache::Bytes() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return bytes_;
}

void BytecodeCache::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  lru_.clear();
  index_.clear();
  bytes_ = 0;
}

}  // namespace Berry
