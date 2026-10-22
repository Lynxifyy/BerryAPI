// Script package manager: semver, constraints, resolution and require.
// See include/berry/package.h for the model.
#include "berry/package.h"
#include "json_min.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>

namespace Berry {

namespace {

constexpr size_t kMaxRanges = 512;

bool ParseU32(const std::string& s, u32* out) {
  if (s.empty() || s.size() > 10) return false;
  u64 v = 0;
  for (char c : s) {
    if (!std::isdigit((unsigned char)c)) return false;
    v = v * 10 + (u64)(c - '0');
    if (v > 0xFFFFFFFFull) return false;
  }
  *out = (u32)v;
  return true;
}

/// A pre-release identifier: dot-separated alphanumerics and hyphens, non-empty.
bool ValidPrerelease(const std::string& s) {
  if (s.empty()) return false;
  size_t start = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    if (i == s.size() || s[i] == '.') {
      if (i == start) return false;   // empty identifier
      start = i + 1;
      continue;
    }
    const char c = s[i];
    if (!std::isalnum((unsigned char)c) && c != '-') return false;
  }
  return true;
}

/// Numeric if it is all digits (so "01" is not numeric — no leading-zero
/// numeric identifiers), else compared as text.
bool PrereleasePartNumeric(const std::string& s, u64* out) {
  if (s.empty()) return false;
  if (s.size() > 1 && s[0] == '0') return false;
  u64 v = 0;
  for (char c : s) {
    if (!std::isdigit((unsigned char)c)) return false;
    if (v > (std::numeric_limits<u64>::max() - (u64)(c - '0')) / 10) return false;
    v = v * 10 + (u64)(c - '0');
  }
  *out = v;
  return true;
}

int ComparePrerelease(const std::string& a, const std::string& b) {
  if (a == b) return 0;
  if (a.empty()) return 1;    // a release is greater than any pre-release
  if (b.empty()) return -1;

  std::vector<std::string> pa, pb;
  auto split = [](const std::string& s, std::vector<std::string>* out) {
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
      if (i == s.size() || s[i] == '.') {
        out->push_back(s.substr(start, i - start));
        start = i + 1;
      }
    }
  };
  split(a, &pa);
  split(b, &pb);

  const size_t n = std::min(pa.size(), pb.size());
  for (size_t i = 0; i < n; ++i) {
    u64 na = 0, nb = 0;
    const bool aNum = PrereleasePartNumeric(pa[i], &na);
    const bool bNum = PrereleasePartNumeric(pb[i], &nb);
    if (aNum && bNum) {
      if (na != nb) return na < nb ? -1 : 1;
    } else if (aNum != bNum) {
      return aNum ? -1 : 1;   // numeric identifiers sort before alphanumeric
    } else {
      const int c = pa[i].compare(pb[i]);
      if (c != 0) return c < 0 ? -1 : 1;
    }
  }
  if (pa.size() != pb.size()) return pa.size() < pb.size() ? -1 : 1;
  return 0;
}

/// Parses one comparator: an operator followed by a version, or a caret/tilde
/// that expands into two ranges.
bool ParseComparator(const std::string& text, std::vector<VersionRange>* out,
                     std::string* error) {
  std::string s = text;
  // trim
  size_t b = 0, e = s.size();
  while (b < e && std::isspace((unsigned char)s[b])) ++b;
  while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
  s = s.substr(b, e - b);
  if (s.empty()) {
    if (error) *error = "empty comparator";
    return false;
  }

  auto parseVer = [&](const std::string& t, SemVer* v) {
    if (!SemVer::Parse(t, v)) {
      if (error) *error = "bad version '" + t + "'";
      return false;
    }
    return true;
  };

  if (s == "*") {
    out->push_back(VersionRange{RangeOp::Ge, SemVer{}});
    return true;
  }

  if (s[0] == '^') {
    SemVer v;
    if (!parseVer(s.substr(1), &v)) return false;
    // ^1.2.3 -> >=1.2.3 <2.0.0 ; ^0.2.3 -> >=0.2.3 <0.3.0 ; ^0.0.3 -> =0.0.3
    out->push_back(VersionRange{RangeOp::Ge, v});
    SemVer hi;
    if (v.major > 0) {
      hi = SemVer{v.major + 1, 0, 0, ""};
    } else if (v.minor > 0) {
      hi = SemVer{0, v.minor + 1, 0, ""};
    } else {
      hi = SemVer{0, 0, v.patch + 1, ""};
    }
    out->push_back(VersionRange{RangeOp::Lt, hi});
    return true;
  }

  if (s[0] == '~') {
    SemVer v;
    if (!parseVer(s.substr(1), &v)) return false;
    // ~1.2.3 -> >=1.2.3 <1.3.0 ; ~1 -> >=1.0.0 <2.0.0
    out->push_back(VersionRange{RangeOp::Ge, v});
    out->push_back(VersionRange{RangeOp::Lt, SemVer{v.major, v.minor + 1, 0, ""}});
    return true;
  }

  RangeOp op = RangeOp::Eq;
  size_t vstart = 0;
  if (s.size() >= 2 && (s[0] == '>' || s[0] == '<') && s[1] == '=') {
    op = s[0] == '>' ? RangeOp::Ge : RangeOp::Le;
    vstart = 2;
  } else if (s[0] == '>') {
    op = RangeOp::Gt;
    vstart = 1;
  } else if (s[0] == '<') {
    op = RangeOp::Lt;
    vstart = 1;
  } else if (s[0] == '=') {
    op = RangeOp::Eq;
    vstart = 1;
  }
  SemVer v;
  if (!parseVer(s.substr(vstart), &v)) return false;
  out->push_back(VersionRange{op, v});
  return true;
}

/// Splits on a two-character separator.
std::vector<std::string> SplitOn(const std::string& s, const std::string& sep) {
  std::vector<std::string> out;
  size_t pos = 0;
  while (true) {
    const size_t next = s.find(sep, pos);
    if (next == std::string::npos) {
      out.push_back(s.substr(pos));
      break;
    }
    out.push_back(s.substr(pos, next - pos));
    pos = next + sep.size();
  }
  return out;
}

}  // namespace

//----------------------------------------------------------------------------
// Semver
//----------------------------------------------------------------------------

bool SemVer::Parse(const std::string& text, SemVer* out) {
  std::string s = text;
  size_t b = 0, e = s.size();
  while (b < e && std::isspace((unsigned char)s[b])) ++b;
  while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
  s = s.substr(b, e - b);
  if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s = s.substr(1);
  if (s.empty()) return false;

  std::string core = s, pre;
  // Build metadata is not part of the ordering; strip it before looking for a
  // pre-release, or a '-' inside the metadata would read as one.
  const size_t plus = core.find('+');
  if (plus != std::string::npos) core = core.substr(0, plus);
  const size_t dash = core.find('-');
  if (dash != std::string::npos) {
    pre = core.substr(dash + 1);
    core = core.substr(0, dash);
    if (!ValidPrerelease(pre)) return false;
  }

  const std::vector<std::string> parts = SplitOn(core, ".");
  if (parts.size() != 3) return false;
  u32 maj = 0, min = 0, pat = 0;
  if (!ParseU32(parts[0], &maj)) return false;
  if (!ParseU32(parts[1], &min)) return false;
  if (!ParseU32(parts[2], &pat)) return false;

  out->major = maj;
  out->minor = min;
  out->patch = pat;
  out->prerelease = pre;
  return true;
}

std::string SemVer::ToString() const {
  std::string s = std::to_string(major) + "." + std::to_string(minor) + "." +
                  std::to_string(patch);
  if (!prerelease.empty()) s += "-" + prerelease;
  return s;
}

int CompareSemVer(const SemVer& a, const SemVer& b) {
  if (a.major != b.major) return a.major < b.major ? -1 : 1;
  if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
  if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
  return ComparePrerelease(a.prerelease, b.prerelease);
}

const char* RangeOpName(RangeOp op) {
  switch (op) {
    case RangeOp::Eq: return "==";
    case RangeOp::Lt: return "<";
    case RangeOp::Le: return "<=";
    case RangeOp::Gt: return ">";
    case RangeOp::Ge: return ">=";
  }
  return "?";
}

//----------------------------------------------------------------------------
// Constraints
//----------------------------------------------------------------------------

VersionConstraint VersionConstraint::Any() {
  VersionConstraint c;
  c.groups.push_back({});   // one empty group: satisfied by everything
  return c;
}

VersionConstraint VersionConstraint::None() {
  return VersionConstraint{};   // no groups: satisfied by nothing
}

bool VersionConstraint::IsAny() const {
  return groups.size() == 1 && groups[0].empty();
}

bool VersionConstraint::IsNone() const { return groups.empty(); }

bool VersionConstraint::Satisfies(const SemVer& v) const {
  for (const auto& group : groups) {
    bool all = true;
    for (const VersionRange& r : group) {
      const int c = CompareSemVer(v, r.version);
      bool ok = false;
      switch (r.op) {
        case RangeOp::Eq: ok = (c == 0); break;
        case RangeOp::Lt: ok = (c < 0); break;
        case RangeOp::Le: ok = (c <= 0); break;
        case RangeOp::Gt: ok = (c > 0); break;
        case RangeOp::Ge: ok = (c >= 0); break;
      }
      if (!ok) { all = false; break; }
    }
    if (all) return true;
  }
  return false;
}

bool VersionConstraint::Parse(const std::string& text, VersionConstraint* out,
                              std::string* error) {
  std::string s = text;
  size_t b = 0, e = s.size();
  while (b < e && std::isspace((unsigned char)s[b])) ++b;
  while (e > b && std::isspace((unsigned char)s[e - 1])) --e;
  s = s.substr(b, e - b);

  if (s.empty() || s == "*") {
    *out = Any();
    return true;
  }

  VersionConstraint c;
  // OR of AND groups.
  for (const std::string& orPart : SplitOn(s, "||")) {
    std::vector<VersionRange> group;
    for (const std::string& andPart : SplitOn(orPart, ",")) {
      if (!ParseComparator(andPart, &group, error)) return false;
    }
    if (group.empty()) {
      if (error) *error = "empty constraint group";
      return false;
    }
    c.groups.push_back(std::move(group));
  }
  if (c.groups.empty()) {
    if (error) *error = "empty constraint";
    return false;
  }
  *out = std::move(c);
  return true;
}

std::string VersionConstraint::ToString() const {
  if (IsAny()) return "*";
  if (IsNone()) return "<none>";
  std::string out;
  for (size_t g = 0; g < groups.size(); ++g) {
    if (g) out += " || ";
    for (size_t r = 0; r < groups[g].size(); ++r) {
      if (r) out += ",";
      out += RangeOpName(groups[g][r].op);
      out += groups[g][r].version.ToString();
    }
  }
  return out;
}

size_t RangeCount(const VersionConstraint& c) {
  size_t n = 0;
  for (const auto& g : c.groups) n += g.size();
  return n;
}

VersionConstraint Intersect(const VersionConstraint& a, const VersionConstraint& b) {
  if (a.IsAny()) return b;
  if (b.IsAny()) return a;
  VersionConstraint out;
  size_t ranges = 0;
  for (const auto& ga : a.groups) {
    for (const auto& gb : b.groups) {
      std::vector<VersionRange> merged = ga;
      merged.insert(merged.end(), gb.begin(), gb.end());
      ranges += merged.size();
      if (ranges > kMaxRanges) return VersionConstraint::None();
      out.groups.push_back(std::move(merged));
    }
  }
  return out;
}

//----------------------------------------------------------------------------
// PackageManifest
//----------------------------------------------------------------------------

const char* PackageErrorName(PackageError e) {
  switch (e) {
    case PackageError::Ok: return "ok";
    case PackageError::BadJson: return "bad json";
    case PackageError::MissingName: return "missing name";
    case PackageError::BadName: return "bad name";
    case PackageError::MissingVersion: return "missing version";
    case PackageError::BadVersion: return "bad version";
    case PackageError::MissingEntry: return "missing entry";
    case PackageError::BadEntry: return "bad entry path";
    case PackageError::BadDependencies: return "bad dependencies";
    case PackageError::BadConstraint: return "bad constraint";
    case PackageError::Duplicate: return "duplicate dependency";
    case PackageError::TooManyDeps: return "too many dependencies";
  }
  return "unknown";
}

bool PackageManifest::ValidName(const std::string& name) {
  if (name.empty() || name.size() > 64) return false;
  const char c0 = name[0];
  if (!std::isalnum((unsigned char)c0)) return false;
  for (char c : name) {
    const unsigned char u = (unsigned char)c;
    if (!std::isalnum(u) && c != '-' && c != '_' && c != '.') return false;
  }
  // A name must not be a path fragment.
  if (name.find("..") != std::string::npos) return false;
  return true;
}

bool PackageManifest::ValidEntryPath(const std::string& entry) {
  if (entry.empty() || entry.size() > 256) return false;
  if (entry[0] == '/' || entry[0] == '\\') return false;
  if (entry.find('\\') != std::string::npos) return false;
  if (entry.find(':') != std::string::npos) return false;
  // No empty, "." or ".." segment.
  size_t start = 0;
  for (size_t i = 0; i <= entry.size(); ++i) {
    if (i == entry.size() || entry[i] == '/') {
      const std::string seg = entry.substr(start, i - start);
      if (seg.empty() || seg == "." || seg == "..") return false;
      start = i + 1;
    }
  }
  return true;
}

bool PackageManifest::Parse(const std::string& json, PackageManifest* out,
                            PackageError* outError, std::string* error) {
  auto fail = [&](PackageError e, const std::string& detail) {
    if (outError) *outError = e;
    if (error) *error = detail;
    return false;
  };

  detail::json::Value root;
  std::string jerr;
  if (!detail::json::Parse(json, &root, &jerr)) {
    return fail(PackageError::BadJson, jerr);
  }
  if (!root.IsObject()) return fail(PackageError::BadJson, "root is not an object");

  // Unknown keys are refused: a typo would otherwise silently drop a field.
  static const char* kKnown[] = {"name", "version", "entry", "author",
                                 "description", "dependencies"};
  for (const auto& kv : root.object) {
    bool known = false;
    for (const char* k : kKnown) {
      if (kv.first == k) { known = true; break; }
    }
    if (!known) return fail(PackageError::BadJson, "unknown key '" + kv.first + "'");
  }

  const detail::json::Value* name = root.Get("name");
  if (!name || !name->IsString() || name->str.empty()) {
    return fail(PackageError::MissingName, "missing or empty 'name'");
  }
  if (!ValidName(name->str)) return fail(PackageError::BadName, "bad name '" + name->str + "'");

  const detail::json::Value* version = root.Get("version");
  if (!version || !version->IsString() || version->str.empty()) {
    return fail(PackageError::MissingVersion, "missing or empty 'version'");
  }
  SemVer parsed;
  if (!SemVer::Parse(version->str, &parsed)) {
    return fail(PackageError::BadVersion, "bad version '" + version->str + "'");
  }

  const detail::json::Value* entry = root.Get("entry");
  if (!entry || !entry->IsString() || entry->str.empty()) {
    return fail(PackageError::MissingEntry, "missing or empty 'entry'");
  }
  if (!ValidEntryPath(entry->str)) {
    return fail(PackageError::BadEntry, "unsafe entry path '" + entry->str + "'");
  }

  PackageManifest m;
  m.name = name->str;
  m.version = version->str;
  m.parsedVersion = parsed;
  m.entry = entry->str;
  if (const detail::json::Value* a = root.Get("author"); a && a->IsString()) m.author = a->str;
  if (const detail::json::Value* d = root.Get("description"); d && d->IsString()) m.description = d->str;

  if (const detail::json::Value* deps = root.Get("dependencies")) {
    if (!deps->IsObject()) {
      return fail(PackageError::BadDependencies, "'dependencies' is not an object");
    }
    if (deps->object.size() > PackageRegistry::kMaxDeps) {
      return fail(PackageError::TooManyDeps, "too many dependencies");
    }
    std::set<std::string> seen;
    for (const auto& kv : deps->object) {
      if (!ValidName(kv.first)) {
        return fail(PackageError::BadName, "bad dependency name '" + kv.first + "'");
      }
      if (!seen.insert(kv.first).second) {
        return fail(PackageError::Duplicate, "duplicate dependency '" + kv.first + "'");
      }
      if (!kv.second.IsString()) {
        return fail(PackageError::BadDependencies,
                    "dependency '" + kv.first + "' constraint is not a string");
      }
      PackageDependency dep;
      dep.name = kv.first;
      dep.raw = kv.second.str;
      std::string cerr;
      if (!VersionConstraint::Parse(kv.second.str, &dep.constraint, &cerr)) {
        return fail(PackageError::BadConstraint,
                    "dependency '" + kv.first + "': " + cerr);
      }
      m.dependencies.push_back(std::move(dep));
    }
  }

  *out = std::move(m);
  if (outError) *outError = PackageError::Ok;
  if (error) error->clear();
  return true;
}

//----------------------------------------------------------------------------
// PackageRegistry
//----------------------------------------------------------------------------

bool PackageRegistry::Add(const PackageManifest& manifest, std::string* error) {
  if (!PackageManifest::ValidName(manifest.name)) {
    if (error) *error = "bad package name '" + manifest.name + "'";
    return false;
  }
  SemVer v;
  if (!SemVer::Parse(manifest.version, &v)) {
    if (error) *error = "bad version '" + manifest.version + "'";
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  if (manifests_.size() >= kMaxPackages) {
    if (error) *error = "package registry full";
    return false;
  }
  for (const auto& m : manifests_) {
    if (m.name == manifest.name && m.version == manifest.version) {
      if (error) *error = "duplicate " + manifest.name + "@" + manifest.version;
      return false;
    }
  }
  PackageManifest copy = manifest;
  copy.parsedVersion = v;
  manifests_.push_back(std::move(copy));
  return true;
}

PackageError PackageRegistry::AddJson(const std::string& json, std::string* error) {
  PackageManifest m;
  PackageError pe = PackageError::Ok;
  std::string detail;
  if (!PackageManifest::Parse(json, &m, &pe, &detail)) {
    if (error) *error = detail;
    return pe;
  }
  if (!Add(m, error)) return PackageError::Duplicate;
  return PackageError::Ok;
}

bool PackageRegistry::Has(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& m : manifests_) {
    if (m.name == name) return true;
  }
  return false;
}

bool PackageRegistry::HasVersion(const std::string& name, const std::string& version) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& m : manifests_) {
    if (m.name == name && m.version == version) return true;
  }
  return false;
}

std::vector<SemVer> PackageRegistry::Versions(const std::string& name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::vector<SemVer> out;
  for (const auto& m : manifests_) {
    if (m.name == name) out.push_back(m.parsedVersion);
  }
  std::sort(out.begin(), out.end());
  return out;
}

std::vector<std::string> PackageRegistry::Names() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::set<std::string> names;
  for (const auto& m : manifests_) names.insert(m.name);
  return std::vector<std::string>(names.begin(), names.end());
}

size_t PackageRegistry::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return manifests_.size();
}

const PackageManifest* PackageRegistry::Find(const std::string& name,
                                             const std::string& version) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& m : manifests_) {
    if (m.name == name && m.version == version) return &m;
  }
  return nullptr;
}

namespace {

/// Highest registered version of `name` satisfying `c`, or false.
bool HighestSatisfying(const std::vector<PackageManifest>& all,
                       const std::string& name, const VersionConstraint& c,
                       SemVer* out) {
  bool found = false;
  SemVer best;
  for (const auto& m : all) {
    if (m.name != name) continue;
    if (!c.Satisfies(m.parsedVersion)) continue;
    if (!found || best < m.parsedVersion) {
      best = m.parsedVersion;
      found = true;
    }
  }
  if (found) *out = best;
  return found;
}

}  // namespace

const char* ResolveErrorName(ResolveError e) {
  switch (e) {
    case ResolveError::Ok: return "ok";
    case ResolveError::MissingPackage: return "missing package";
    case ResolveError::NoVersion: return "no satisfying version";
    case ResolveError::Conflict: return "version conflict";
    case ResolveError::Cycle: return "dependency cycle";
    case ResolveError::TooMany: return "too many packages";
  }
  return "unknown";
}

ResolveResult PackageRegistry::Resolve(const std::vector<PackageDependency>& roots) const {
  ResolveResult result;

  // Snapshot under the lock; resolution runs without holding it so a slow
  // resolve cannot block registration.
  std::vector<PackageManifest> all;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    all = manifests_;
  }
  if (all.size() > kMaxPackages) {
    result.error = ResolveError::TooMany;
    result.detail = "registry holds too many packages";
    return result;
  }

  // constraints[name] is the conjunction every dependent imposes so far.
  std::vector<std::pair<std::string, VersionConstraint>> constraints;
  auto constraintFor = [&](const std::string& n) -> VersionConstraint* {
    for (auto& kv : constraints) {
      if (kv.first == n) return &kv.second;
    }
    return nullptr;
  };
  auto addConstraint = [&](const std::string& n, const VersionConstraint& c) {
    VersionConstraint* cur = constraintFor(n);
    if (cur) {
      *cur = Intersect(*cur, c);
    } else {
      constraints.emplace_back(n, c);
    }
  };

  for (const auto& r : roots) {
    if (!Has(r.name)) {
      result.error = ResolveError::MissingPackage;
      result.detail = "no package named '" + r.name + "'";
      return result;
    }
    addConstraint(r.name, r.constraint);
  }

  // Fixpoint: pick the highest satisfying version per constrained package,
  // fold in the dependencies of the chosen versions, repeat until stable.
  std::vector<std::pair<std::string, SemVer>> chosen;
  auto chosenVersion = [&](const std::string& n, SemVer* out) {
    for (const auto& kv : chosen) {
      if (kv.first == n) { *out = kv.second; return true; }
    }
    return false;
  };

  bool stable = false;
  for (size_t round = 0; round < kMaxRounds; ++round) {
    stable = true;
    // Re-pick every constrained package.
    for (auto& kv : constraints) {
      SemVer best;
      if (kv.second.IsNone() || !HighestSatisfying(all, kv.first, kv.second, &best)) {
        result.error = ResolveError::NoVersion;
        result.detail = "no version of '" + kv.first + "' satisfies " +
                        kv.second.ToString();
        return result;
      }
      SemVer have;
      if (!chosenVersion(kv.first, &have) || have != best) {
        // Update the choice and remember to fold its dependencies in.
        bool replaced = false;
        for (auto& c : chosen) {
          if (c.first == kv.first) { c.second = best; replaced = true; break; }
        }
        if (!replaced) chosen.emplace_back(kv.first, best);
        stable = false;
      }
    }
    if (stable) break;

    // Fold the dependencies of everything chosen so far.
    for (const auto& c : chosen) {
      const PackageManifest* m = nullptr;
      for (const auto& cand : all) {
        if (cand.name == c.first && cand.parsedVersion == c.second) { m = &cand; break; }
      }
      if (!m) {
        result.error = ResolveError::Conflict;
        result.detail = "internal: chosen " + c.first + "@" + c.second.ToString() +
                        " is not registered";
        return result;
      }
      for (const auto& dep : m->dependencies) {
        if (!Has(dep.name)) {
          result.error = ResolveError::MissingPackage;
          result.detail = c.first + " depends on missing package '" + dep.name + "'";
          return result;
        }
        addConstraint(dep.name, dep.constraint);
      }
    }
  }
  if (!stable) {
    result.error = ResolveError::Conflict;
    result.detail = "resolution did not converge within " +
                    std::to_string(kMaxRounds) + " rounds";
    return result;
  }

  // Topological order: dependencies before dependents. Deterministic: ties are
  // broken by name, and each package's dependencies are visited in name order.
  std::vector<std::string> names;
  for (const auto& c : chosen) names.push_back(c.first);
  std::sort(names.begin(), names.end());

  // The chosen manifest for a package, or nullptr.
  auto chosenManifest = [&](const std::string& n) -> const PackageManifest* {
    SemVer v;
    if (!chosenVersion(n, &v)) return nullptr;
    for (const auto& cand : all) {
      if (cand.name == n && cand.parsedVersion == v) return &cand;
    }
    return nullptr;
  };
  auto inClosure = [&](const std::string& n) {
    for (const auto& c : chosen) {
      if (c.first == n) return true;
    }
    return false;
  };

  std::vector<std::string> order;
  std::set<std::string> visiting, done;
  bool cycle = false;
  std::function<void(const std::string&)> visit = [&](const std::string& n) {
    if (done.count(n)) return;
    if (visiting.count(n)) { cycle = true; return; }
    visiting.insert(n);
    if (const PackageManifest* m = chosenManifest(n)) {
      std::vector<std::string> deps;
      for (const auto& d : m->dependencies) {
        if (inClosure(d.name)) deps.push_back(d.name);
      }
      std::sort(deps.begin(), deps.end());
      for (const auto& d : deps) visit(d);
    }
    visiting.erase(n);
    if (!done.count(n)) {
      done.insert(n);
      order.push_back(n);
    }
  };
  for (const auto& n : names) visit(n);
  if (cycle) {
    result.error = ResolveError::Cycle;
    result.detail = "dependency cycle among: " + [&] {
      std::string s;
      for (const auto& n : names) { if (!s.empty()) s += ","; s += n; }
      return s;
    }();
    return result;
  }

  for (const auto& n : order) {
    SemVer v;
    chosenVersion(n, &v);
    ResolvedPackage rp;
    rp.name = n;
    rp.version = v.ToString();
    for (const auto& cand : all) {
      if (cand.name == n && cand.parsedVersion == v) { rp.entry = cand.entry; break; }
    }
    result.order.push_back(std::move(rp));
  }
  std::vector<std::string> versions;
  for (const auto& n : names) {
    SemVer v;
    chosenVersion(n, &v);
    versions.push_back(n + "@" + v.ToString());
  }
  std::sort(versions.begin(), versions.end());
  result.versions = std::move(versions);
  result.error = ResolveError::Ok;
  return result;
}

void PackageRegistry::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  manifests_.clear();
}

bool ResolveRequire(const PackageRegistry& registry, const std::string& path,
                    std::string* name, std::string* entry, std::string* error) {
  auto fail = [&](const std::string& detail) {
    if (error) *error = detail;
    return false;
  };
  if (path.empty()) return fail("empty require path");

  // Split the first segment as the package, the rest as the file within it.
  const size_t slash = path.find('/');
  const std::string pkg = slash == std::string::npos ? path : path.substr(0, slash);
  const std::string sub = slash == std::string::npos ? "" : path.substr(slash + 1);

  if (!PackageManifest::ValidName(pkg)) return fail("bad package name '" + pkg + "'");
  if (!registry.Has(pkg)) return fail("no package named '" + pkg + "'");

  if (!sub.empty()) {
    // The sub-path must stay inside the package.
    if (!PackageManifest::ValidEntryPath(sub)) {
      return fail("unsafe require path '" + sub + "'");
    }
    if (name) *name = pkg;
    if (entry) *entry = sub;
    if (error) error->clear();
    return true;
  }

  // No file named: the package's own entry. Use the highest registered version.
  const std::vector<SemVer> versions = registry.Versions(pkg);
  if (versions.empty()) return fail("no version of '" + pkg + "'");
  const SemVer top = versions.back();
  const PackageManifest* m = registry.Find(pkg, top.ToString());
  if (!m) return fail("no manifest for " + pkg + "@" + top.ToString());
  if (m->entry.empty()) return fail("package '" + pkg + "' has no entry");
  if (name) *name = pkg;
  if (entry) *entry = m->entry;
  if (error) error->clear();
  return true;
}

}  // namespace Berry
