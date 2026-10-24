#include "berry/fs.h"

#include <algorithm>
#include <vector>

#if !defined(_WIN32)
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace Berry {

namespace {

/// The prefix our write-then-rename staging files use. A scan skips these (and
/// removes leftovers), so a crash mid-write never counts against the quota.
constexpr char kTempPrefix[] = ".tmp.";

/// Splits a validated relative path into its directory part (may be empty) and
/// its final segment. The path is already validated, so the final segment is
/// never empty.
void SplitPath(const std::string& path, std::string* dir, std::string* leaf) {
  const size_t slash = path.rfind('/');
  if (slash == std::string::npos) {
    dir->clear();
    *leaf = path;
  } else {
    *dir = path.substr(0, slash);
    *leaf = path.substr(slash + 1);
  }
}

#if !defined(_WIN32)

/// Opens the directory that holds `relDir` beneath `rootFd`, creating any
/// missing components when `create` is set. Every component is opened with
/// `O_NOFOLLOW`, so a symlink anywhere on the way is refused (`ELOOP`) rather
/// than followed out of the sandbox. Returns -1 on any failure.
int OpenDir(int rootFd, const std::string& relDir, bool create) {
  int fd = dup(rootFd);
  if (fd < 0) return -1;
  if (relDir.empty()) return fd;
  size_t start = 0;
  while (start <= relDir.size()) {
    const size_t slash = relDir.find('/', start);
    const std::string seg = (slash == std::string::npos)
                                ? relDir.substr(start)
                                : relDir.substr(start, slash - start);
    if (seg.empty()) {
      close(fd);
      return -1;
    }
    int next = openat(fd, seg.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (next < 0 && errno == ENOENT && create) {
      if (mkdirat(fd, seg.c_str(), 0700) != 0 && errno != EEXIST) {
        close(fd);
        return -1;
      }
      next = openat(fd, seg.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    }
    if (next < 0) {
      close(fd);
      return -1;
    }
    close(fd);
    fd = next;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return fd;
}

/// `mkdir -p` for a trusted, embedder-supplied absolute root path.
bool MakeRootDirs(const std::string& path) {
  if (path.empty()) return false;
  std::string cur;
  size_t start = 0;
  if (path[0] == '/') {
    cur = "/";
    start = 1;
  }
  while (start <= path.size()) {
    const size_t slash = path.find('/', start);
    const std::string seg = (slash == std::string::npos)
                                ? path.substr(start)
                                : path.substr(start, slash - start);
    if (!seg.empty()) {
      if (cur.empty()) {
        cur = seg;
      } else if (cur.back() == '/') {
        cur += seg;
      } else {
        cur += '/' + seg;
      }
      if (mkdir(cur.c_str(), 0700) != 0 && errno != EEXIST) return false;
    }
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

/// A per-process, per-store unique staging name. The counter is process-global
/// and monotonic; the pid keeps two processes apart, so two writers never pick
/// the same temp file.
std::string TempName() {
  static std::atomic<unsigned> counter{0};
  const unsigned n = counter.fetch_add(1, std::memory_order_relaxed);
  char buf[64];
  std::snprintf(buf, sizeof buf, "%s%ld.%u", kTempPrefix, (long)getpid(), n);
  return buf;
}

/// Reads a whole regular file opened through `dirFd`. Refuses a non-regular
/// file (a symlink, a fifo) and bounds the read at the per-file cap.
bool ReadFileAt(int dirFd, const std::string& leaf, std::string* out) {
  const int fd = openat(dirFd, leaf.c_str(), O_RDONLY | O_NOFOLLOW);
  if (fd < 0) return false;
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return false;
  }
  std::string data;
  if (st.st_size > 0) data.reserve((size_t)st.st_size);
  char buf[8192];
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0) {
    data.append(buf, (size_t)n);
    if (data.size() > ScriptFileStore::kMaxFileBytes) {
      close(fd);
      return false;
    }
  }
  const bool bad = n < 0;
  close(fd);
  if (bad) return false;
  if (out) *out = std::move(data);
  return true;
}

/// Recursively walks an open directory. In `collect` mode it appends each
/// regular file's sandbox-relative path to `out`; otherwise it unlinks every
/// regular file. Symlinks are unlinked but never followed, so a planted link
/// cannot make this reach outside the sandbox. Leftover staging files are
/// always removed and never counted.
void WalkDir(int dirFd, const std::string& relPrefix, bool collect,
             std::string* out) {
  const int dupFd = dup(dirFd);
  if (dupFd < 0) return;
  DIR* d = fdopendir(dupFd);
  if (!d) {
    close(dupFd);
    return;
  }
  while (struct dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    struct stat st;
    if (fstatat(dirFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    if (S_ISLNK(st.st_mode)) {
      unlinkat(dirFd, name.c_str(), 0);
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      const int sub =
          openat(dirFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      if (sub < 0) continue;
      const std::string rel = relPrefix.empty() ? name : relPrefix + '/' + name;
      WalkDir(sub, rel, collect, out);
      close(sub);
      if (!collect) unlinkat(dirFd, name.c_str(), AT_REMOVEDIR);
      continue;
    }
    if (!S_ISREG(st.st_mode)) continue;
    if (name.compare(0, sizeof(kTempPrefix) - 1, kTempPrefix) == 0) {
      unlinkat(dirFd, name.c_str(), 0);
      continue;
    }
    if (collect) {
      if (out) {
        if (!out->empty()) *out += '\n';
        *out += relPrefix.empty() ? name : relPrefix + '/' + name;
      }
    } else {
      unlinkat(dirFd, name.c_str(), 0);
    }
  }
  closedir(d);
}

/// Recursively sums regular files (skipping symlinks and staging files) under
/// an open directory, so `Open` can seed the quota from what is already there.
void ScanDir(int dirFd, size_t* files, size_t* bytes) {
  const int dupFd = dup(dirFd);
  if (dupFd < 0) return;
  DIR* d = fdopendir(dupFd);
  if (!d) {
    close(dupFd);
    return;
  }
  while (struct dirent* e = readdir(d)) {
    const std::string name = e->d_name;
    if (name == "." || name == "..") continue;
    struct stat st;
    if (fstatat(dirFd, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0) continue;
    if (S_ISLNK(st.st_mode)) continue;                 // never follow
    if (S_ISDIR(st.st_mode)) {
      const int sub =
          openat(dirFd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
      if (sub < 0) continue;
      ScanDir(sub, files, bytes);
      close(sub);
      continue;
    }
    if (!S_ISREG(st.st_mode)) continue;
    if (name.compare(0, sizeof(kTempPrefix) - 1, kTempPrefix) == 0) {
      unlinkat(dirFd, name.c_str(), 0);               // crash leftover
      continue;
    }
    ++*files;
    *bytes += (size_t)st.st_size;
  }
  closedir(d);
}

#endif  // !_WIN32

}  // namespace

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

ScriptFileStore::~ScriptFileStore() = default;

//----------------------------------------------------------------------------
// Lifecycle
//----------------------------------------------------------------------------

bool ScriptFileStore::Open(const std::string& rootDirectory) {
  std::lock_guard<std::mutex> lk(mutex_);
  // Re-binding is a fresh start: drop whatever mode we were in.
  files_.clear();
  totalBytes_ = 0;
  fileCount_ = 0;
  persistent_ = false;
  root_.clear();

  if (rootDirectory.empty()) return false;
#if defined(_WIN32)
  // Persistent storage is POSIX-only in this build; on Windows the store stays
  // in-memory rather than persisting without the symlink protections above.
  (void)rootDirectory;
  return false;
#else
  if (!MakeRootDirs(rootDirectory)) return false;
  const int fd = open(rootDirectory.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  close(fd);
  root_ = rootDirectory;
  size_t files = 0, bytes = 0;
  if (!ScanDisk(&files, &bytes)) {
    root_.clear();
    return false;
  }
  // Count what is already there against the quota; a directory that already
  // holds more than the caps refuses to open rather than starting over-limit.
  if (files > kMaxFiles || bytes > kMaxTotalBytes) {
    root_.clear();
    return false;
  }
  persistent_ = true;
  fileCount_ = files;
  totalBytes_ = bytes;
  return true;
#endif
}

bool ScriptFileStore::ScanDisk(size_t* files, size_t* bytes) const {
  if (files) *files = 0;
  if (bytes) *bytes = 0;
#if defined(_WIN32)
  return false;
#else
  const int fd = open(root_.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  size_t f = 0, b = 0;
  ScanDir(fd, &f, &b);
  close(fd);
  if (files) *files = f;
  if (bytes) *bytes = b;
  return true;
#endif
}

bool ScriptFileStore::persistent() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return persistent_;
}

std::string ScriptFileStore::root() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return root_;
}

//----------------------------------------------------------------------------
// Memory mode
//----------------------------------------------------------------------------

bool ScriptFileStore::MemWrite(const std::string& path,
                               const std::string& data) {
  if (data.size() > kMaxFileBytes) return false;
  auto it = files_.find(path);
  const size_t oldSize = it == files_.end() ? 0 : it->second.size();
  const size_t newTotal = totalBytes_ - oldSize + data.size();
  if (newTotal > kMaxTotalBytes) return false;
  if (it == files_.end() && fileCount_ >= kMaxFiles) return false;

  if (it == files_.end()) {
    files_.emplace(path, data);
    ++fileCount_;
  } else {
    it->second = data;
  }
  totalBytes_ = newTotal;
  return true;
}

bool ScriptFileStore::MemRead(const std::string& path, std::string* out) const {
  auto it = files_.find(path);
  if (it == files_.end()) return false;
  if (out) *out = it->second;
  return true;
}

bool ScriptFileStore::MemRemove(const std::string& path) {
  auto it = files_.find(path);
  if (it == files_.end()) return false;
  totalBytes_ -= it->second.size();
  --fileCount_;
  files_.erase(it);
  return true;
}

//----------------------------------------------------------------------------
// Disk mode
//----------------------------------------------------------------------------

#if !defined(_WIN32)

bool ScriptFileStore::DiskWrite(const std::string& path,
                                const std::string& data) {
  if (data.size() > kMaxFileBytes) return false;

  std::string dir, leaf;
  SplitPath(path, &dir, &leaf);

  const int rootFd = open(root_.c_str(), O_RDONLY | O_DIRECTORY);
  if (rootFd < 0) return false;
  const int dirFd = OpenDir(rootFd, dir, /*create=*/true);
  close(rootFd);
  if (dirFd < 0) return false;

  // An existing target must be a regular file; a symlink (or anything else) is
  // refused rather than written through.
  struct stat st;
  size_t oldSize = 0;
  const bool exists =
      fstatat(dirFd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0;
  if (exists) {
    if (!S_ISREG(st.st_mode)) {
      close(dirFd);
      return false;
    }
    oldSize = (size_t)st.st_size;
  }

  const size_t newTotal = totalBytes_ - oldSize + data.size();
  if (newTotal > kMaxTotalBytes) {
    close(dirFd);
    return false;
  }
  if (!exists && fileCount_ >= kMaxFiles) {
    close(dirFd);
    return false;
  }

  // Stage in a temp file in the same directory, fsync, then rename into place.
  // renameat is atomic on the same filesystem, so a reader sees the old file or
  // the new one, never a partial write.
  const std::string tmp = TempName();
  const int tfd = openat(dirFd, tmp.c_str(),
                         O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
  if (tfd < 0) {
    close(dirFd);
    return false;
  }
  bool ok = true;
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = write(tfd, data.data() + off, data.size() - off);
    if (n <= 0) {
      ok = false;
      break;
    }
    off += (size_t)n;
  }
  if (ok && fsync(tfd) != 0) ok = false;
  if (close(tfd) != 0) ok = false;

  if (ok && renameat(dirFd, tmp.c_str(), dirFd, leaf.c_str()) != 0) ok = false;
  if (!ok) {
    unlinkat(dirFd, tmp.c_str(), 0);
    close(dirFd);
    return false;
  }
  close(dirFd);

  totalBytes_ = newTotal;
  if (!exists) ++fileCount_;
  return true;
}

bool ScriptFileStore::DiskRead(const std::string& path,
                               std::string* out) const {
  std::string dir, leaf;
  SplitPath(path, &dir, &leaf);
  const int rootFd = open(root_.c_str(), O_RDONLY | O_DIRECTORY);
  if (rootFd < 0) return false;
  const int dirFd = OpenDir(rootFd, dir, /*create=*/false);
  close(rootFd);
  if (dirFd < 0) return false;
  const bool ok = ReadFileAt(dirFd, leaf, out);
  close(dirFd);
  return ok;
}

bool ScriptFileStore::DiskExists(const std::string& path) const {
  std::string dir, leaf;
  SplitPath(path, &dir, &leaf);
  const int rootFd = open(root_.c_str(), O_RDONLY | O_DIRECTORY);
  if (rootFd < 0) return false;
  const int dirFd = OpenDir(rootFd, dir, /*create=*/false);
  close(rootFd);
  if (dirFd < 0) return false;
  struct stat st;
  const bool ok = fstatat(dirFd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
                  S_ISREG(st.st_mode);
  close(dirFd);
  return ok;
}

size_t ScriptFileStore::DiskSize(const std::string& path) const {
  std::string dir, leaf;
  SplitPath(path, &dir, &leaf);
  const int rootFd = open(root_.c_str(), O_RDONLY | O_DIRECTORY);
  if (rootFd < 0) return 0;
  const int dirFd = OpenDir(rootFd, dir, /*create=*/false);
  close(rootFd);
  if (dirFd < 0) return 0;
  struct stat st;
  size_t size = 0;
  if (fstatat(dirFd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 &&
      S_ISREG(st.st_mode)) {
    size = (size_t)st.st_size;
  }
  close(dirFd);
  return size;
}

bool ScriptFileStore::DiskRemove(const std::string& path) {
  std::string dir, leaf;
  SplitPath(path, &dir, &leaf);
  const int rootFd = open(root_.c_str(), O_RDONLY | O_DIRECTORY);
  if (rootFd < 0) return false;
  const int dirFd = OpenDir(rootFd, dir, /*create=*/false);
  close(rootFd);
  if (dirFd < 0) return false;
  struct stat st;
  if (fstatat(dirFd, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISREG(st.st_mode)) {
    close(dirFd);
    return false;
  }
  const bool ok = unlinkat(dirFd, leaf.c_str(), 0) == 0;
  close(dirFd);
  if (ok) {
    totalBytes_ -= (size_t)st.st_size;
    --fileCount_;
  }
  return ok;
}

#else   // _WIN32

bool ScriptFileStore::DiskWrite(const std::string&, const std::string&) {
  return false;
}
bool ScriptFileStore::DiskRead(const std::string&, std::string*) const {
  return false;
}
bool ScriptFileStore::DiskExists(const std::string&) const { return false; }
size_t ScriptFileStore::DiskSize(const std::string&) const { return 0; }
bool ScriptFileStore::DiskRemove(const std::string&) { return false; }

#endif  // !_WIN32

//----------------------------------------------------------------------------
// Public operations
//----------------------------------------------------------------------------

bool ScriptFileStore::Write(const std::string& path, const std::string& data) {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  return persistent_ ? DiskWrite(p.value, data) : MemWrite(p.value, data);
}

bool ScriptFileStore::Read(const std::string& path, std::string* out) const {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  return persistent_ ? DiskRead(p.value, out) : MemRead(p.value, out);
}

bool ScriptFileStore::Append(const std::string& path, const std::string& data) {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return false;
  std::lock_guard<std::mutex> lk(mutex_);

  if (persistent_) {
    // A missing file is an empty base; a symlink or other non-regular file at
    // the path makes DiskWrite refuse, so the append fails closed rather than
    // replacing it with a fresh file.
    std::string base;
    if (DiskExists(p.value)) DiskRead(p.value, &base);
    std::string merged = base;
    merged += data;
    if (merged.size() > kMaxFileBytes) return false;
    return DiskWrite(p.value, merged);
  }

  auto it = files_.find(p.value);
  std::string merged = it == files_.end() ? std::string() : it->second;
  merged += data;
  if (merged.size() > kMaxFileBytes) return false;
  return MemWrite(p.value, merged);
}

bool ScriptFileStore::Exists(const std::string& path) const {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (!persistent_) return files_.find(p.value) != files_.end();
  return DiskExists(p.value);
}

bool ScriptFileStore::Remove(const std::string& path) {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  return persistent_ ? DiskRemove(p.value) : MemRemove(p.value);
}

size_t ScriptFileStore::Size(const std::string& path) const {
  const ScriptPath p = ValidateScriptPath(path);
  if (!p.valid) return 0;
  std::lock_guard<std::mutex> lk(mutex_);
  if (!persistent_) {
    auto it = files_.find(p.value);
    return it == files_.end() ? 0 : it->second.size();
  }
  return DiskSize(p.value);
}

std::string ScriptFileStore::List(const std::string& prefix) const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::string out;
  if (!persistent_) {
    for (const auto& [k, v] : files_) {
      (void)v;
      if (!prefix.empty() && k.compare(0, prefix.size(), prefix) != 0) continue;
      if (!out.empty()) out += '\n';
      out += k;
    }
    return out;
  }
#if !defined(_WIN32)
  std::string listed;
  const int fd = open(root_.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return out;
  WalkDir(fd, std::string(), /*collect=*/true, &listed);
  close(fd);
  // WalkDir returns readdir order; sort for a stable listing.
  std::vector<std::string> paths;
  size_t start = 0;
  while (start <= listed.size()) {
    const size_t nl = listed.find('\n', start);
    const std::string item = (nl == std::string::npos)
                                 ? listed.substr(start)
                                 : listed.substr(start, nl - start);
    if (!item.empty() &&
        (prefix.empty() || item.compare(0, prefix.size(), prefix) == 0)) {
      paths.push_back(item);
    }
    if (nl == std::string::npos) break;
    start = nl + 1;
  }
  std::sort(paths.begin(), paths.end());
  for (const auto& s : paths) {
    if (!out.empty()) out += '\n';
    out += s;
  }
#endif
  return out;
}

void ScriptFileStore::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!persistent_) {
    files_.clear();
    totalBytes_ = 0;
    fileCount_ = 0;
    return;
  }
#if !defined(_WIN32)
  const int fd = open(root_.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    WalkDir(fd, std::string(), /*collect=*/false, nullptr);
    close(fd);
  }
#endif
  totalBytes_ = 0;
  fileCount_ = 0;
}

size_t ScriptFileStore::FileCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return fileCount_;
}

size_t ScriptFileStore::TotalBytes() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return totalBytes_;
}

}  // namespace Berry
