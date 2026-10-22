#include "berry/fs.h"

#include <algorithm>

namespace Berry {

ScriptPath ValidateScriptPath(const std::string& path, size_t maxLen,
                              size_t maxDepth) {
  ScriptPath out;
  if (path.empty() || path.size() > maxLen) return out;

  size_t depth = 0;
  size_t segStart = 0;
  for (size_t i = 0; i <= path.size(); ++i) {
    const bool atEnd = (i == path.size());
    const char c = atEnd ? '/' : path[i];
    if (!atEnd) {
      const unsigned char uc = (unsigned char)c;
      // Reject control characters, backslash (a separator on some platforms),
      // a NUL, and the characters a path separator or a Windows device name
      // would need. Keeping the set small means one rule for every platform.
      if (uc < 0x20 || uc == 0x7F) return out;
      if (c == '\\' || c == '\0' || c == ':' || c == '*') return out;
    }
    if (c == '/') {
      const std::string seg = path.substr(segStart, i - segStart);
      if (seg.empty()) return out;                 // no leading/adjacent slash
      if (seg == "." || seg == "..") return out;   // no traversal
      if (seg.size() > 128) return out;
      if (++depth > maxDepth) return out;
      segStart = i + 1;
    }
  }
  out.value = path;
  out.valid = true;
  return out;
}

bool ScriptFileStore::Write(const std::string& path, const std::string& data) {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return false;
  if (data.size() > kMaxFileBytes) return false;

  std::lock_guard<std::mutex> lk(mutex_);
  auto it = files_.find(p.value);
  const size_t oldSize = it == files_.end() ? 0 : it->second.size();
  const size_t newTotal = totalBytes_ - oldSize + data.size();
  if (newTotal > kMaxTotalBytes) return false;
  if (it == files_.end() && files_.size() >= kMaxFiles) return false;

  if (it == files_.end()) {
    files_.emplace(p.value, data);
  } else {
    it->second = data;
  }
  totalBytes_ = newTotal;
  return true;
}

bool ScriptFileStore::Read(const std::string& path, std::string* out) const {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = files_.find(p.value);
  if (it == files_.end()) return false;
  if (out) *out = it->second;
  return true;
}

bool ScriptFileStore::Append(const std::string& path, const std::string& data) {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = files_.find(p.value);
  const size_t oldSize = it == files_.end() ? 0 : it->second.size();
  const size_t newSize = oldSize + data.size();
  if (newSize > kMaxFileBytes) return false;
  const size_t newTotal = totalBytes_ - oldSize + newSize;
  if (newTotal > kMaxTotalBytes) return false;
  if (it == files_.end() && files_.size() >= kMaxFiles) return false;

  if (it == files_.end()) {
    files_.emplace(p.value, data);
  } else {
    it->second += data;
  }
  totalBytes_ = newTotal;
  return true;
}

bool ScriptFileStore::Exists(const std::string& path) const {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  return files_.find(p.value) != files_.end();
}

bool ScriptFileStore::Remove(const std::string& path) {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = files_.find(p.value);
  if (it == files_.end()) return false;
  totalBytes_ -= it->second.size();
  files_.erase(it);
  return true;
}

size_t ScriptFileStore::Size(const std::string& path) const {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return 0;
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = files_.find(p.value);
  return it == files_.end() ? 0 : it->second.size();
}

std::string ScriptFileStore::List(const std::string& prefix) const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::string out;
  for (const auto& [k, v] : files_) {
    if (!prefix.empty() && k.compare(0, prefix.size(), prefix) != 0) continue;
    if (!out.empty()) out += '\n';
    out += k;
  }
  return out;
}

void ScriptFileStore::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  files_.clear();
  totalBytes_ = 0;
}

size_t ScriptFileStore::FileCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return files_.size();
}

size_t ScriptFileStore::TotalBytes() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return totalBytes_;
}

}  // namespace Berry
