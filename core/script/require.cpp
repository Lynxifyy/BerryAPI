// Sandboxed module resolver. See include/berry/require.h.
#include "berry/require.h"

#include <algorithm>

namespace Berry {

const char* RequireErrorName(RequireError e) {
  switch (e) {
    case RequireError::Ok: return "ok";
    case RequireError::BadName: return "bad-name";
    case RequireError::NotFound: return "not-found";
    case RequireError::Cycle: return "cycle";
    case RequireError::VersionMismatch: return "version-mismatch";
    case RequireError::TooManyModules: return "too-many-modules";
  }
  return "?";
}

ModuleResolver::ModuleResolver(IScriptFileSystem* fs, std::string root)
    : fs_(fs), root_(std::move(root)) {}

bool ModuleResolver::SafeName(const std::string& name) {
  if (name.empty() || name.size() > kMaxNameLen) return false;
  if (name.front() == '/' || name.front() == '\\') return false;
  if (name.find('\\') != std::string::npos) return false;
  if (name.find('\0') != std::string::npos) return false;
  // A `..` path component (not a substring, so "a..b" is allowed) climbs out.
  size_t start = 0;
  while (start <= name.size()) {
    size_t slash = name.find('/', start);
    const std::string seg = name.substr(
        start, slash == std::string::npos ? std::string::npos : slash - start);
    if (seg == "..") return false;
    if (slash == std::string::npos) break;
    start = slash + 1;
  }
  return true;
}

RequireError ModuleResolver::Resolve(const std::string& name,
                                     const std::string& constraint,
                                     std::string* out,
                                     std::string* resolvedVersion) {
  if (!fs_ || !out) return RequireError::BadName;
  if (!SafeName(name)) return RequireError::BadName;

  // A cached module still must satisfy the constraint.
  auto it = cache_.find(name);
  if (it != cache_.end()) {
    if (!constraint.empty() && !it->second.version.empty()) {
      VersionConstraint c;
      std::string cerr;
      SemVer v;
      if (VersionConstraint::Parse(constraint, &c, &cerr) &&
          SemVer::Parse(it->second.version, &v) && !c.Satisfies(v)) {
        return RequireError::VersionMismatch;
      }
    }
    *out = it->second.source;
    if (resolvedVersion) *resolvedVersion = it->second.version;
    return RequireError::Ok;
  }

  if (std::find(inProgress_.begin(), inProgress_.end(), name) != inProgress_.end()) {
    return RequireError::Cycle;
  }
  if (cache_.size() >= kMaxModules) return RequireError::TooManyModules;

  const std::string path =
      (root_.empty() ? std::string() : root_ + "/") + name + ".luau";
  std::string source;
  if (!fs_->ReadFile(path, &source)) return RequireError::NotFound;

  // The module's declared version comes from a leading `-- version: X.Y.Z`
  // comment, which the loader can read without compiling. A module that
  // declares none matches only an unconstrained require.
  std::string version;
  {
    const size_t nl = source.find('\n');
    const std::string first = source.substr(0, nl);
    const std::string marker = "version:";
    size_t p = first.find(marker);
    if (p != std::string::npos) {
      std::string v = first.substr(p + marker.size());
      size_t b = v.find_first_not_of(" \t\r");
      size_t e = v.find_last_not_of(" \t\r");
      if (b != std::string::npos) version = v.substr(b, e - b + 1);
    }
  }
  if (!constraint.empty()) {
    VersionConstraint c;
    std::string cerr;
    if (!VersionConstraint::Parse(constraint, &c, &cerr)) {
      return RequireError::VersionMismatch;
    }
    SemVer v;
    if (version.empty() || !SemVer::Parse(version, &v) || !c.Satisfies(v)) {
      return RequireError::VersionMismatch;
    }
  }

  cache_[name] = Entry{source, version};
  order_.push_back(name);
  *out = source;
  if (resolvedVersion) *resolvedVersion = version;
  return RequireError::Ok;
}

std::string ModuleResolver::Version(const std::string& name) const {
  auto it = cache_.find(name);
  return it == cache_.end() ? std::string() : it->second.version;
}

void ModuleResolver::Reset() {
  cache_.clear();
  order_.clear();
  inProgress_.clear();
}

}  // namespace Berry
