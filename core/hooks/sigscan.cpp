#include "berry/sigscan.h"
#include "berry/logger.h"
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <algorithm>

#ifndef O_BINARY
#define O_BINARY 0
#endif

namespace Berry {

const char* SigStateName(SigState s) {
  switch (s) {
    case SigState::Unknown: return "unknown";
    case SigState::Missing: return "missing";
    case SigState::Found:   return "found";
  }
  return "unknown";
}

// FNV-1a 64-bit. Cheap, and a plain byte fingerprint is enough to distinguish
// one build from another.
static u64 Fnv1a(const u8* data, size_t len) {
  u64 h = 1469598103934665603ULL;
  for (size_t i = 0; i < len; ++i) {
    h ^= data[i];
    h *= 1099511628211ULL;
  }
  return h;
}

//----------------------------------------------------------------------------
// BuildLibrary
//----------------------------------------------------------------------------

u64 BuildLibrary::HashSection(const u8* data, size_t len) {
  return data && len ? Fnv1a(data, len) : 0;
}

BuildLibrary BuildLibrary::Fingerprint(const char* name) {
  BuildLibrary b;
  if (!name) return b;
  b.name = name;
  b.base = ProcessImages::MainModuleBase(name);
  if (!b.base) return b;

  // Hash the executable memory range(s) of the module (crosses merges).
  u64 h = 1469598103934665603ULL;  // FNV-1a accumulator seeded
  size_t total = 0;
  for (auto& r : ProcessImages::EnumExecutable()) {
    if (r.name.find(name) == std::string::npos) continue;
    const u8* p = reinterpret_cast<const u8*>(r.base);
    for (size_t i = 0; i < r.size; ++i) {
      h ^= p[i];
      h *= 1099511628211ULL;
      ++total;
    }
  }
  b.textSize = total;
  // If hashing could not touch any byte (no ranges found), hash 0 marks the
  // module as unverifiable -> features fail closed.
  b.textHash = total ? h : 0;
  // File size: report the size of the first matching executable mapping.
  for (auto& r : ProcessImages::EnumExecutable()) {
    if (r.name.find(name) != std::string::npos) {
      b.fileSize = (u64)r.size;
      break;
    }
  }
  return b;
}

//----------------------------------------------------------------------------
// Signature
//----------------------------------------------------------------------------

Signature::Signature(std::string id_, const char* hexPattern, i32 fromEndOffset_)
    : id(std::move(id_)), fromEndOffset(fromEndOffset_) {
  Parse(hexPattern);
}

bool Signature::Parse(const char* hex) {
  bytes.clear();
  mask.clear();
  while (*hex) {
    while (*hex == ' ' || *hex == '\t') ++hex;
    if (!*hex) break;
    char a = *hex;
    char b = *(hex + 1);
    if (a == '?' || a == 'x' || a == 'X') {
      bytes.push_back(0);
      mask.push_back(0);
      hex += (b && (b == '?' || b == ' ' || b == '\t' || b == '\0')) ? 1 : 1;
      if (b && b != '?' && b != 'x' && b != 'X' && b != ' ' && b != '\t' && b != '\0') ++hex;
      continue;
    }
    if (!std::isxdigit((unsigned char)a)) return false;
    int hi = std::isdigit((unsigned char)a) ? a - '0' : std::tolower(a) - 'a' + 10;
    int lo = 0;
    if (b && std::isxdigit((unsigned char)b)) {
      lo = std::isdigit((unsigned char)b) ? b - '0' : std::tolower(b) - 'a' + 10;
    }
    bytes.push_back((u8)((hi << 4) | lo));
    mask.push_back(1);
    hex += (b && std::isxdigit((unsigned char)b)) ? 2 : 1;
  }
  return !bytes.empty();
}

bool Signature::Match(const u8* data) const {
  if (bytes.empty()) return false;   // an empty pattern is never a real match
  if (!data) return false;
  for (size_t i = 0; i < bytes.size(); ++i) {
    if (mask[i] && data[i] != bytes[i]) return false;
  }
  return true;
}

uptr Signature::Resolve(uptr scanStart) const {
  return fromEndOffset != 0
             ? scanStart + (size_t)fromEndOffset
             : scanStart;
}

//----------------------------------------------------------------------------
// ProcessImages
//----------------------------------------------------------------------------

std::vector<MemRange> ProcessImages::EnumExecutable() {
  std::vector<MemRange> out;
  std::ifstream maps("/proc/self/maps");
  if (!maps) return out;
  std::string line;
  uptr lastEnd = 0;
  std::string lastName;
  while (std::getline(maps, line)) {
    unsigned long long start = 0, end = 0;
    char perms[8] = {0};
    int fields = 0;
    fields = sscanf(line.c_str(), "%llx-%llx %7s", &start, &end, perms);
    if (fields != 3) continue;
    if (!(perms[0] == 'r' && perms[2] == 'x')) continue;
    // Path = everything after the 5th space.
    std::string path;
    size_t first = 0;
    int spaces = 0;
    for (size_t i = 0; i < line.size(); ++i) {
      if (line[i] == ' ') ++spaces;
      if (spaces == 5) { first = i + 1; break; }
    }
    if (first) path = line.substr(first);
    if (start == lastEnd && !out.empty() && path == lastName) {
      out.back().size = (size_t)(end - start);
      lastEnd = end;
      continue;
    }
    MemRange r;
    r.base = (uptr)start;
    r.size = (size_t)(end - start);
    r.name = path;
    out.push_back(r);
    lastEnd = end;
    lastName = path;
  }
  return out;
}

uptr ProcessImages::MainModuleBase(const char* name) {
  for (auto& r : EnumExecutable()) {
    if (r.name.find(name) != std::string::npos) return r.base;
  }
  return 0;
}

size_t ProcessImages::MainModuleSize() {
  for (auto& r : EnumExecutable()) {
    if (r.name.find("libminecraftpe.so") != std::string::npos) return r.size;
  }
  return 0;
}

bool ProcessImages::FindSection(uptr moduleBase, const char* sectionName,
                                uptr* addr, size_t* size) {
  // moduleBase is accepted for API compatibility with ELF-parsing backends.
  BERRY_UNUSED(moduleBase);
  if (!sectionName) return false;
  if (strcmp(sectionName, ".text") != 0) return false;
  for (auto& r : EnumExecutable()) {
    if (r.name.find("libminecraftpe.so") != std::string::npos) {
      *addr = r.base;
      *size = r.size;
      return true;
    }
  }
  return false;
}

//----------------------------------------------------------------------------
// SigScanner
//----------------------------------------------------------------------------

SigScanner::SigScanner(const std::vector<MemRange>& ranges) : ranges_(ranges) {}

bool SigScanner::MatchesAt(uptr addr, const Signature& sig) const {
  return addr && sig.Match(reinterpret_cast<const u8*>(addr));
}

uptr SigScanner::Find(const Signature& sig, size_t* outIndex) const {
  for (size_t ri = 0; ri < ranges_.size(); ++ri) {
    const MemRange& r = ranges_[ri];
    const u8* base = reinterpret_cast<const u8*>(r.base);
    const size_t n = r.size;
    if (sig.bytes.size() > n) continue;
    const size_t limit = n - sig.bytes.size() + 1;
    for (size_t i = 0; i < limit; ++i) {
      if (sig.Match(base + i)) {
        if (outIndex) *outIndex = ri;
        return r.base + i;
      }
    }
  }
  return 0;
}

std::vector<uptr> SigScanner::FindAll(const Signature& sig, size_t maxResults) const {
  std::vector<uptr> found;
  for (const auto& r : ranges_) {
    const u8* base = reinterpret_cast<const u8*>(r.base);
    const size_t n = r.size;
    if (sig.bytes.size() > n) continue;
    const size_t limit = n - sig.bytes.size() + 1;
    for (size_t i = 0; i < limit && found.size() < maxResults; ++i) {
      if (sig.Match(base + i)) found.push_back(r.base + i);
      if (found.size() >= maxResults) break;
    }
  }
  return found;
}

//----------------------------------------------------------------------------
// AArch64 arm64 helpers
//----------------------------------------------------------------------------

namespace arm64 {

uptr DecodeAdrp(u32 insn, uptr pc) {
  // 01 0 000 .,i,immlo,immhi,Rd  -> adrp
  i64 immhi = (insn >> 5) & 0x7FFFF;
  i64 immlo = (insn >> 29) & 0x3;
  i64 imm = (immhi << 2) | immlo;
  if (imm & (1LL << 20)) imm |= ~((1LL << 21) - 1);  // sign extend 21-bit
  uptr page = pc & ~0xFFFULL;
  return (uptr)((i64)page + (imm << 12));
}

uptr ResolveAdrpAdd(uptr pc, u32 rdTarget) {
  // Detects: adrp Xd, page  /  add Xd, Xd, #lo12
  u32 insn = *reinterpret_cast<const u32*>(pc);
  if (((insn >> 24) & 0xFF) != 0x90) return 0;  // not adrp
  u32 rd = insn & 0x1F;
  if (rdTarget && rd != rdTarget) return 0;
  uptr page = DecodeAdrp(insn, pc);
  // look ahead for add Xd, Xd, #imm
  for (size_t i = 1; i < 8; ++i) {
    u32 next = *reinterpret_cast<const u32*>(pc + i * 4);
    // add imm: 0x91000000 | Rn<<5 | Rd  (sf=1)
    if ((next & 0xFF800000U) == 0x91000000U && ((next >> 5) & 0x1F) == rd &&
        (next & 0x1F) == rd) {
      uptr off = DecodeAddImm(next);
      return page + off;
    }
  }
  return page;
}

uptr ResolveAdrpAddForward(uptr adrpPc, u32 rd) {
  return ResolveAdrpAdd(adrpPc, rd);
}

std::vector<uptr> FindAdrpXrefs(uptr moduleBase, size_t moduleSize, uptr target,
                                size_t maxResults) {
  std::vector<uptr> out;
  const u32* code = reinterpret_cast<const u32*>(moduleBase);
  const size_t count = moduleSize / 4;
  for (size_t i = 0; i < count && out.size() < maxResults; ++i) {
    u32 insn = code[i];
    if (((insn >> 24) & 0xFF) != 0x90) continue;  // adrp
    uptr pc = moduleBase + i * 4;
    uptr resolved = ResolveAdrpAddForward(pc, insn & 0x1F);
    if (resolved == target) out.push_back(pc);
  }
  return out;
}

}  // namespace arm64

//----------------------------------------------------------------------------
// SigCache
//----------------------------------------------------------------------------

SigCache& SigCache::Instance() {
  static SigCache inst;
  return inst;
}

SigScanner& SigCache::Scanner() {
  if (!scanner_) scanner_ = new SigScanner(ProcessImages::EnumExecutable());
  return *scanner_;
}

void SigCache::SetExpectedBuild(const BuildLibrary& build) {
  std::lock_guard<std::mutex> lk(mutex_);
  expectedBuild_ = build;
  buildGateOpen_.store(false);

  bool empty = build.name.empty() && build.base == 0 && build.textHash == 0;
  if (empty) {
    // No fingerprint provided at all: the whole table is permanently closed.
    // Rules resolve to Missing on first use — features disable themselves.
    gateClosedPermanently_ = true;
  } else {
    gateClosedPermanently_ = false;
  }
  for (auto& e : entries_) e.second.state = SigState::Unknown;
}

bool SigCache::BuildVerifiedLocked() const {
  if (buildGateOpen_.load()) return true;
  if (gateClosedPermanently_.load()) return false;
  // Lazily fingerprint the live module and compare against the expected one.
  BuildLibrary live = BuildLibrary::Fingerprint(expectedBuild_.name.c_str());
  bool ok = expectedBuild_.verified && live.base != 0 &&
            expectedBuild_.base == live.base &&
            (expectedBuild_.fileSize == 0 || expectedBuild_.fileSize == live.fileSize) &&
            (expectedBuild_.textSize == 0 || expectedBuild_.textSize == live.textSize) &&
            (expectedBuild_.textHash == 0 || expectedBuild_.textHash == live.textHash);
  if (ok) {
    buildGateOpen_.store(true);
    return true;
  }
  Log::Warn("BerryAPI: engine build fingerprint does not match signature table "
            "(expected base=%llx, live base=%llx hash=%llx) — "
            "signature features disabled",
            (unsigned long long)expectedBuild_.base,
            (unsigned long long)live.base,
            (unsigned long long)live.textHash);
  return false;
}

bool SigCache::BuildVerified() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return BuildVerifiedLocked();
}

void SigCache::Register(const std::string& id, const Signature& sig) {
  std::lock_guard<std::mutex> lk(mutex_);
  SigState initial = gateClosedPermanently_.load() ? SigState::Missing
                                                   : SigState::Unknown;
  for (auto& e : entries_) {
    if (e.first == id) {
      e.second.sig = sig;
      e.second.state = initial;
      e.second.results.clear();
      return;
    }
  }
  entries_.emplace_back(id, Entry{sig, {}, initial});
}

bool SigCache::Has(const std::string& id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& e : entries_)
    if (e.first == id) return true;
  return false;
}

size_t SigCache::Count() const { return entries_.size(); }

uptr SigCache::ResolveAndRegister(const std::string& id, const Signature& sig) {
  Register(id, sig);
  return Resolve(id);
}

uptr SigCache::Resolve(const std::string& id, size_t occurrence) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& e : entries_) {
    if (e.first != id) continue;
    Entry& en = e.second;
    if (en.state == SigState::Found) {
      if (occurrence < en.results.size())
        return en.sig.Resolve(en.results[occurrence]);
      return 0;
    }
    if (en.state == SigState::Missing) return 0;
    // Unknown: resolve now, but only when the build passes the gate.
    if (!BuildVerifiedLocked() || !expectedBuild_.base) {
      en.state = SigState::Missing;
      Log::Warn("BerryAPI: rule '%s' not resolved (build not verified) — "
                "dependent feature disabled",
                id.c_str());
      return 0;
    }
    en.results = Scanner().FindAll(en.sig, 64);
    if (en.results.empty()) {
      en.state = SigState::Missing;
      Log::Warn("BerryAPI: rule '%s' not found in verified build — dependent "
                "feature disabled",
                id.c_str());
      return 0;
    }
    en.state = SigState::Found;
    if (occurrence < en.results.size())
      return en.sig.Resolve(en.results[occurrence]);
    return 0;
  }
  return 0;
}

SigState SigCache::State(const std::string& id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& e : entries_)
    if (e.first == id) return e.second.state;
  return SigState::Unknown;
}

}  // namespace Berry