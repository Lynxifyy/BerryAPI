#include "berry/sigscan.h"
#include "berry/logger.h"
#include "berry/manifest.h"   // crypto::Sha256Hex for the integrity fingerprint
#include "../script/json_min.h"   // offset-cache parse
#include <atomic>
#include <cctype>
#include <csignal>
#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <thread>
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

std::string BuildLibrary::HashSectionSha256(const u8* data, size_t len) {
  if (!data || !len) return std::string();
  return crypto::Sha256Hex(data, len);
}

std::string BuildLibrary::ReadBuildId(const char* path) {
  if (!path || !*path) return std::string();
  std::ifstream f(path, std::ios::binary);
  if (!f) return std::string();
  // Minimal ELF parse: header -> section-header table -> SHT_NOTE sections ->
  // NT_GNU_BUILD_ID. Enough for a build-id; a malformed file yields "".
  unsigned char ehdr[64];
  f.read(reinterpret_cast<char*>(ehdr), sizeof ehdr);
  if (f.gcount() < 64) return std::string();
  if (ehdr[0] != 0x7f || ehdr[1] != 'E' || ehdr[2] != 'L' || ehdr[3] != 'F') {
    return std::string();
  }
  const bool is64 = ehdr[4] == 2;
  const bool le = ehdr[5] == 1;
  auto rd16 = [&](const unsigned char* p) -> u16 {
    return le ? (u16)(p[0] | (p[1] << 8)) : (u16)((p[0] << 8) | p[1]);
  };
  auto rd32 = [&](const unsigned char* p) -> u32 {
    if (le) return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
  };
  auto rd64 = [&](const unsigned char* p) -> u64 {
    u64 lo = rd32(p), hi = rd32(p + 4);
    return le ? (lo | (hi << 32)) : ((lo << 32) | hi);
  };
  u64 shoff = is64 ? rd64(ehdr + 0x28) : (u64)rd32(ehdr + 0x20);
  u16 shentsize = is64 ? rd16(ehdr + 0x3A) : rd16(ehdr + 0x2E);
  u16 shnum = is64 ? rd16(ehdr + 0x3C) : rd16(ehdr + 0x30);
  if (shoff == 0 || shentsize == 0 || shnum == 0) return std::string();
  std::vector<unsigned char> sh((size_t)shentsize * shnum);
  f.seekg((std::streamoff)shoff, std::ios::beg);
  f.read(reinterpret_cast<char*>(sh.data()), (std::streamsize)sh.size());
  if ((size_t)f.gcount() < sh.size()) return std::string();
  for (u16 i = 0; i < shnum; ++i) {
    const unsigned char* s = sh.data() + (size_t)i * shentsize;
    u32 type = rd32(s + 4);
    if (type != 7) continue;   // SHT_NOTE
    u64 off = is64 ? rd64(s + 0x18) : (u64)rd32(s + 0x10);
    u64 size = is64 ? rd64(s + 0x20) : (u64)rd32(s + 0x14);
    if (size == 0 || size > (1u << 20)) continue;
    std::vector<unsigned char> notes((size_t)size);
    f.seekg((std::streamoff)off, std::ios::beg);
    f.read(reinterpret_cast<char*>(notes.data()), (std::streamsize)notes.size());
    if ((size_t)f.gcount() < notes.size()) continue;
    size_t p = 0;
    while (p + 12 <= notes.size()) {
      u32 namesz = rd32(&notes[p]);
      u32 descsz = rd32(&notes[p + 4]);
      u32 ntype = rd32(&notes[p + 8]);
      p += 12;
      if (namesz == 0 || p + namesz > notes.size()) break;
      std::string nname((const char*)&notes[p], namesz);
      while (!nname.empty() && nname.back() == '\0') nname.pop_back();
      p += (namesz + 3) & ~3u;
      if (p + descsz > notes.size()) break;
      if (nname == "GNU" && ntype == 3) {   // NT_GNU_BUILD_ID
        static const char* hex = "0123456789abcdef";
        std::string out;
        for (u32 k = 0; k < descsz; ++k) {
          out.push_back(hex[notes[p + k] >> 4]);
          out.push_back(hex[notes[p + k] & 0xF]);
        }
        return out;
      }
      p += (descsz + 3) & ~3u;
    }
  }
  return std::string();
}

BuildLibrary BuildLibrary::Fingerprint(const char* name) {
  BuildLibrary b;
  if (!name) return b;
  b.name = name;

  // Locate the module's executable ranges by name, and record its identity.
  std::string path;
  u64 total = 0;
  for (auto& r : ProcessImages::EnumExecutable()) {
    if (r.name.find(name) == std::string::npos) continue;
    if (b.base == 0) b.base = r.base;
    if (b.dev.empty()) { b.dev = r.dev; b.inode = r.inode; }
    if (path.empty() && !r.name.empty() && r.name[0] == '/') path = r.name;
    total += r.size;
  }
  if (b.base == 0) return b;
  b.fileSize = total;

  // The fingerprint is taken from the file on disk (build-id + SHA-256), never
  // from live .text: our own hooks change live bytes, and an FNV-1a hash of
  // 100+ MB of text is both slow and collision-prone (bugs 21, 22).
  if (!path.empty()) {
    b.buildId = ReadBuildId(path.c_str());
    std::ifstream f(path, std::ios::binary);
    if (f) {
      std::string data((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
      b.textSize = data.size();
      if (!data.empty()) b.textHash = HashSection((const u8*)data.data(), data.size());
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

namespace {

/// One hex nibble, or -1 when `c` is not a hex digit.
int HexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

bool Signature::Parse(const char* hex, std::string* error) {
  bytes.clear();
  mask.clear();
  wild.clear();
  auto fail = [&](const char* msg) {
    if (error) *error = msg;
    bytes.clear();
    mask.clear();
    wild.clear();
    return false;
  };
  if (!hex) return fail("null pattern");

  while (*hex) {
    while (*hex == ' ' || *hex == '\t') ++hex;
    if (!*hex) break;

    // C-escaped style: \x48, \x8D, \x??
    if (hex[0] == '\\' && (hex[1] == 'x' || hex[1] == 'X')) {
      hex += 2;
      int hi = HexNibble(*hex);
      if (hi < 0) {
        if (*hex == '?') { hex += 2; bytes.push_back(0); mask.push_back(0); wild.push_back(0x00); continue; }
        return fail("bad \\x escape");
      }
      ++hex;
      int lo = HexNibble(*hex);
      if (lo < 0) {
        if (*hex == '?') { ++hex; bytes.push_back((u8)(hi << 4)); mask.push_back(0); wild.push_back(0xF0); continue; }
        return fail("single hex digit after \\x");
      }
      ++hex;
      bytes.push_back((u8)((hi << 4) | lo));
      mask.push_back(1);
      wild.push_back(0xFF);
      continue;
    }

    // Token forms: "??", "?B", "A?", "AB".
    char a = hex[0];
    char b = hex[1];
    int ha = HexNibble(a), hb = HexNibble(b);
    bool aWild = (a == '?'), bWild = (b == '?');

    if (aWild && (bWild || b == '\0' || b == ' ' || b == '\t')) {
      // A whole wildcard byte: "?" or "??".
      bytes.push_back(0); mask.push_back(0); wild.push_back(0x00);
      hex += bWild ? 2 : 1;
      continue;
    }
    if (aWild && hb >= 0) {
      // "?B": low nibble fixed.
      bytes.push_back((u8)hb); mask.push_back(0); wild.push_back(0x0F);
      hex += 2;
      continue;
    }
    if (ha >= 0 && bWild) {
      // "A?": high nibble fixed (bug 14: this must be one byte, not two).
      bytes.push_back((u8)(ha << 4)); mask.push_back(0); wild.push_back(0xF0);
      hex += 2;
      continue;
    }
    if (ha >= 0 && hb >= 0) {
      bytes.push_back((u8)((ha << 4) | hb)); mask.push_back(1); wild.push_back(0xFF);
      hex += 2;
      continue;
    }
    if (ha >= 0 && (b == '\0' || b == ' ' || b == '\t')) {
      // A single hex digit with no wildcard is ambiguous; refuse it rather than
      // silently reading "F" as 0xF0 (bug 15).
      return fail("single hex digit (ambiguous)");
    }
    return fail("invalid hex token");
  }

  if (bytes.empty()) return fail("empty pattern");
  return true;
}

bool Signature::Match(const u8* data) const {
  if (bytes.empty()) return false;   // an empty pattern is never a real match
  if (!data) return false;
  for (size_t i = 0; i < bytes.size(); ++i) {
    const u8 m = i < wild.size() ? wild[i] : (u8)(mask[i] ? 0xFF : 0x00);
    if ((data[i] & m) != (bytes[i] & m)) return false;
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

namespace {

std::mutex g_mapsMutex;
std::vector<MemRange> g_mapsCache;
bool g_mapsCached = false;
std::atomic<u64> g_mapsGeneration{0};

/// Reads /proc/self/maps into `out`, merging adjacent mappings that belong to
/// the same backing file. The merge accumulates the size (`+=`), so a file with
/// three adjacent executable segments keeps its whole span rather than
/// collapsing to the last one (bug 16). The name is the path with any padding
/// stripped (bug 17). Execute-only (`--x`) mappings are kept, flagged, so a
/// scan can fall back to the on-disk file for their bytes (bug 18).
void ParseMaps(std::vector<MemRange>* out) {
  out->clear();
  std::ifstream maps("/proc/self/maps");
  if (!maps) return;
  std::string line;
  while (std::getline(maps, line)) {
    unsigned long long start = 0, end = 0, inode = 0;
    char perms[8] = {0};
    char dev[16] = {0};
    // "start-end perms offset dev inode path"
    int fields = sscanf(line.c_str(), "%llx-%llx %7s %*s %15s %llu",
                        &start, &end, perms, dev, &inode);
    if (fields < 3) continue;
    const bool readable = perms[0] == 'r';
    const bool executable = perms[2] == 'x';
    // Keep read-execute and execute-only; skip everything else.
    if (!executable) continue;
    const bool execOnly = executable && !readable;

    // Path = everything after the 5th space, trimmed of leading/trailing space.
    std::string path;
    {
      size_t first = 0;
      int spaces = 0;
      for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] == ' ') ++spaces;
        if (spaces == 5) { first = i + 1; break; }
      }
      if (first) path = line.substr(first);
      size_t b = path.find_first_not_of(" \t");
      size_t e = path.find_last_not_of(" \t\r");
      path = (b == std::string::npos) ? std::string() : path.substr(b, e - b + 1);
    }
    const std::string devStr = fields >= 4 ? dev : std::string();

    if (!out->empty()) {
      MemRange& last = out->back();
      // Merge only when it is the same backing file (by dev+inode, or by the
      // same anonymous name), and the ranges are contiguous.
      const bool sameFile =
          (inode != 0 && last.inode == inode && last.dev == devStr) ||
          (inode == 0 && last.inode == 0 && last.name == path);
      if (sameFile && (uptr)start == last.base + last.size) {
        last.size += (size_t)(end - start);   // accumulate, do not overwrite
        last.executeOnly = last.executeOnly && execOnly;
        continue;
      }
    }
    MemRange r;
    r.base = (uptr)start;
    r.size = (size_t)(end - start);
    r.name = path;
    r.dev = devStr;
    r.inode = inode;
    r.executeOnly = execOnly;
    out->push_back(r);
  }
}

}  // namespace

std::vector<MemRange> ProcessImages::EnumExecutable() {
  std::lock_guard<std::mutex> lk(g_mapsMutex);
  if (!g_mapsCached) {
    ParseMaps(&g_mapsCache);
    g_mapsCached = true;
  }
  return g_mapsCache;
}

void ProcessImages::Refresh() {
  {
    std::lock_guard<std::mutex> lk(g_mapsMutex);
    g_mapsCached = false;
    g_mapsCache.clear();
  }
  g_mapsGeneration.fetch_add(1);
}

u64 ProcessImages::Generation() { return g_mapsGeneration.load(); }

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

std::vector<u8> ProcessImages::ReadModuleBytes(const std::string& path,
                                               u64* outOffset) {
  if (outOffset) *outOffset = 0;
  std::vector<u8> out;
  if (path.empty()) return out;
  std::ifstream f(path, std::ios::binary);
  if (!f) return out;
  f.seekg(0, std::ios::end);
  const std::streamoff len = f.tellg();
  if (len <= 0) return out;
  f.seekg(0, std::ios::beg);
  out.resize((size_t)len);
  f.read(reinterpret_cast<char*>(out.data()), len);
  if (f.gcount() < len) out.clear();
  return out;
}

//----------------------------------------------------------------------------
// SigScanner
//----------------------------------------------------------------------------

SigScanner::SigScanner(const std::vector<MemRange>& ranges) : ranges_(ranges) {}

bool SigScanner::MatchesAt(uptr addr, const Signature& sig) const {
  return addr && sig.Match(reinterpret_cast<const u8*>(addr));
}

namespace {

/// The anchor byte: the fixed byte in `sig` that is rarest, so a memchr skip
/// discards the most candidates. Returns -1 when every byte is wild (then a
/// linear walk is the only option).
int AnchorIndex(const Signature& sig) {
  int best = -1;
  for (size_t i = 0; i < sig.bytes.size(); ++i) {
    const u8 m = i < sig.wild.size() ? sig.wild[i]
                                     : (u8)(sig.mask[i] ? 0xFF : 0x00);
    if (m == 0xFF) { best = (int)i; break; }   // a fully fixed byte is ideal
    if (m != 0 && best < 0) best = (int)i;     // a half-fixed byte is a fallback
  }
  return best;
}

/// Scans one range for `sig`, calling `onHit(addr)` for each match and stopping
/// when it returns true. Uses a memchr anchor when one exists, so the inner
/// loop skips long stretches in a single libc call (improvement 17).
template <typename OnHit>
void ScanRange(const MemRange& r, const Signature& sig, OnHit onHit) {
  const u8* base = reinterpret_cast<const u8*>(r.base);
  const size_t n = r.size;
  const size_t plen = sig.bytes.size();
  if (plen == 0 || plen > n) return;
  const size_t limit = n - plen + 1;
  const int anchor = AnchorIndex(sig);

  if (anchor >= 0) {
    // A full-byte anchor: memchr skips in one libc call (fastest).
    const u8 anchorMask =
        (size_t)anchor < sig.wild.size() ? sig.wild[(size_t)anchor] : 0xFF;
    const u8 anchorByte = sig.bytes[(size_t)anchor];
    if (anchorMask == 0xFF) {
      size_t i = 0;
      while (i < limit) {
        const u8* hit = (const u8*)memchr(base + i + anchor, anchorByte,
                                          limit - i);
        if (!hit) return;
        i = (size_t)(hit - base) - (size_t)anchor;
        if (sig.Match(base + i) && onHit(r.base + i)) return;
        ++i;
      }
      return;
    }
  }

  // No fully-fixed anchor: Horspool skipping on the last fixed byte
  // (improvement 17). Build a shift table: for a byte b, how far to slide when
  // the window's last fixed position mismatches. A half-fixed byte is compared
  // under its mask.
  int last = -1;
  u8 lastMask = 0, lastByte = 0;
  for (size_t k = 0; k < plen; ++k) {
    const u8 m = k < sig.wild.size() ? sig.wild[k]
                                     : (u8)(sig.mask[k] ? 0xFF : 0x00);
    if (m != 0) { last = (int)k; lastMask = m; lastByte = sig.bytes[k]; }
  }
  if (last < 0) {
    // Every byte is a wildcard: nothing to skip on; a linear walk is the only
    // exact option.
    for (size_t i = 0; i < limit; ++i) {
      if (sig.Match(base + i) && onHit(r.base + i)) return;
    }
    return;
  }
  // shift[b] = distance to move when the byte at `last` is b.
  int shift[256];
  for (int b = 0; b < 256; ++b) shift[b] = (int)plen;
  for (size_t k = 0; k < plen; ++k) {
    const u8 m = k < sig.wild.size() ? sig.wild[k]
                                     : (u8)(sig.mask[k] ? 0xFF : 0x00);
    if (m == 0xFF) shift[sig.bytes[k]] = (int)(plen - 1 - k);
  }
  // A zero shift would stall the scan; clamp to 1 (never needed for a genuine
  // mismatch, but cheap insurance).
  for (int b = 0; b < 256; ++b) {
    if (shift[b] < 1) shift[b] = 1;
  }
  // The last fixed position may be half-fixed; when it is, the shift for the
  // masked value is what we can prove, so fall back to a step of 1 for bytes
  // that do not match its mask.
  size_t i = 0;
  while (i < limit) {
    const u8 win = base[i + (size_t)last];
    if ((win & lastMask) != (lastByte & lastMask)) {
      i += (size_t)shift[win];
      continue;
    }
    if (sig.Match(base + i) && onHit(r.base + i)) return;
    ++i;
  }
}

}  // namespace

uptr SigScanner::Find(const Signature& sig, size_t* outIndex) const {
  for (size_t ri = 0; ri < ranges_.size(); ++ri) {
    uptr hit = 0;
    ScanRange(ranges_[ri], sig, [&](uptr addr) {
      hit = addr;
      if (outIndex) *outIndex = ri;
      return true;
    });
    if (hit) return hit;
  }
  return 0;
}

std::vector<uptr> SigScanner::FindAll(const Signature& sig, size_t maxResults) const {
  std::vector<uptr> found;
  for (const auto& r : ranges_) {
    if (found.size() >= maxResults) break;
    ScanRange(r, sig, [&](uptr addr) {
      found.push_back(addr);
      return found.size() >= maxResults;
    });
  }
  return found;
}

uptr SigScanner::FindUnique(const Signature& sig, size_t* matches) const {
  std::vector<uptr> all = FindAll(sig, 2);
  if (matches) *matches = all.size();
  return all.size() == 1 ? all[0] : 0;
}

uptr SigScanner::ClosestMatch(const Signature& sig, size_t* differingBits) const {
  if (differingBits) *differingBits = (size_t)-1;
  if (sig.bytes.empty()) return 0;
  uptr bestAddr = 0;
  size_t bestDiff = (size_t)-1;
  for (const auto& r : ranges_) {
    const u8* base = reinterpret_cast<const u8*>(r.base);
    const size_t n = r.size;
    const size_t plen = sig.bytes.size();
    if (plen > n) continue;
    const size_t limit = n - plen + 1;
    for (size_t i = 0; i < limit; ++i) {
      size_t diff = 0;
      for (size_t j = 0; j < plen; ++j) {
        const u8 m = j < sig.wild.size() ? sig.wild[j]
                                         : (u8)(sig.mask[j] ? 0xFF : 0x00);
        diff += (size_t)__builtin_popcount((unsigned)((base[i + j] ^ sig.bytes[j]) & m));
        if (diff >= bestDiff) break;
      }
      if (diff < bestDiff) {
        bestDiff = diff;
        bestAddr = r.base + i;
        if (diff == 0) {   // an exact match is the closest possible
          if (differingBits) *differingBits = 0;
          return bestAddr;
        }
      }
    }
  }
  if (bestAddr && differingBits) *differingBits = bestDiff;
  return bestAddr;
}

namespace {

// A process-wide SIGSEGV/SIGBUS guard used around a scan. A scan reads live
// module memory, which a concurrent dlclose can unmap mid-walk; the handler
// siglongjmps back to the scan so a fault fails the scan instead of crashing
// the game (bug 20). Only one guarded scan runs at a time (the scan is not
// reentrant from the handler), which the mutex enforces.
std::mutex g_guardMutex;
sigjmp_buf g_guardJmp;
volatile sig_atomic_t g_guardActive = 0;

void ScanFaultHandler(int, siginfo_t*, void*) {
  if (g_guardActive) siglongjmp(g_guardJmp, 1);
  // Not our fault: restore the default and re-raise so the process still dies
  // with the usual behaviour rather than looping.
  signal(SIGSEGV, SIG_DFL);
  raise(SIGSEGV);
}

struct GuardInstaller {
  struct sigaction oldSegv {};
  struct sigaction oldBus {};
  bool installed = false;
  GuardInstaller() {
    struct sigaction sa {};
    sa.sa_sigaction = ScanFaultHandler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    installed = sigaction(SIGSEGV, &sa, &oldSegv) == 0;
    if (installed) sigaction(SIGBUS, &sa, &oldBus);
  }
  ~GuardInstaller() {
    if (!installed) return;
    sigaction(SIGSEGV, &oldSegv, nullptr);
    sigaction(SIGBUS, &oldBus, nullptr);
  }
};

}  // namespace

uptr SigScanner::FindGuarded(const Signature& sig) const {
  std::lock_guard<std::mutex> lk(g_guardMutex);
  GuardInstaller guard;
  uptr result = 0;
  g_guardActive = 1;
  if (sigsetjmp(g_guardJmp, 1) == 0) {
    result = Find(sig);
  } else {
    result = 0;   // a fault: fail the scan, never crash
  }
  g_guardActive = 0;
  return result;
}

void SigScanner::FindAllMulti(const std::vector<Signature>& sigs,
                              std::vector<uptr>* hits) const {
  hits->assign(sigs.size(), 0);
  if (sigs.empty()) return;

  // Group signatures by the byte value of their anchor (the first fully fixed
  // byte, or the first byte otherwise). At each position in the stream we look
  // up the group whose anchor byte matches and only test those, so the whole
  // table costs one pass plus the tests of matching groups.
  struct Anchor {
    size_t sigIndex;
    size_t offset;   // where the anchor sits inside the pattern
  };
  std::vector<std::vector<Anchor>> byByte(256);
  std::vector<size_t> noAnchor;   // wildcard-only signatures
  std::vector<size_t> pending;    // still-unresolved signature indices
  for (size_t i = 0; i < sigs.size(); ++i) {
    if (sigs[i].bytes.empty()) continue;
    const int a = AnchorIndex(sigs[i]);
    if (a < 0) {
      noAnchor.push_back(i);
      continue;
    }
    byByte[sigs[i].bytes[(size_t)a]].push_back({i, (size_t)a});
  }

  for (const MemRange& r : ranges_) {
    const u8* base = reinterpret_cast<const u8*>(r.base);
    const size_t n = r.size;
    if (n == 0) continue;
    for (size_t pos = 0; pos < n; ++pos) {
      const u8 b = base[pos];
      auto& group = byByte[b];
      for (const Anchor& a : group) {
        if ((*hits)[a.sigIndex] != 0) continue;
        if (pos < a.offset) continue;   // the pattern would start before base
        const size_t start = pos - a.offset;
        if (start + sigs[a.sigIndex].bytes.size() > n) continue;
        if (sigs[a.sigIndex].Match(base + start)) {
          (*hits)[a.sigIndex] = r.base + start;
        }
      }
      // Wildcard-only signatures have no anchor; test them at every position.
      for (size_t si : noAnchor) {
        if ((*hits)[si] != 0) continue;
        if (pos + sigs[si].bytes.size() > n) continue;
        if (sigs[si].Match(base + pos)) (*hits)[si] = r.base + pos;
      }
    }
  }
  (void)pending;
}

uptr SigScanner::FindParallel(const Signature& sig, size_t workers) const {
  if (ranges_.empty()) return 0;
  if (ranges_.size() == 1 || workers <= 1) return Find(sig);
  const size_t nthreads = std::min(workers, ranges_.size());
  std::atomic<uptr> result{0};
  std::vector<std::thread> pool;
  std::atomic<size_t> next{0};
  for (size_t w = 0; w < nthreads; ++w) {
    pool.emplace_back([&]() {
      for (;;) {
        const size_t ri = next.fetch_add(1);
        if (ri >= ranges_.size() || result.load() != 0) return;
        ScanRange(ranges_[ri], sig, [&](uptr addr) {
          uptr expected = 0;
          if (result.compare_exchange_strong(expected, addr)) return true;
          return true;   // someone else found one; stop this range
        });
      }
    });
  }
  for (auto& t : pool) t.join();
  return result.load();
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

uptr ResolveAdrpAdd(uptr pc, u32 rdTarget, size_t maxInsns) {
  // Detects: adrp Xd, page  /  add Xd, Xd, #lo12. `maxInsns` bounds the read to
  // the caller's span so the look-ahead never runs off the end of a buffer.
  if (maxInsns == 0) return 0;
  u32 insn = *reinterpret_cast<const u32*>(pc);
  if (!IsAdrp(insn)) return 0;
  u32 rd = insn & 0x1F;
  if (rdTarget && rd != rdTarget) return 0;
  uptr page = DecodeAdrp(insn, pc);
  // look ahead for add Xd, Xd, #imm
  for (size_t i = 1; i < 8 && i < maxInsns; ++i) {
    u32 next = *reinterpret_cast<const u32*>(pc + i * 4);
    // add imm: 0x91000000 | Rn<<5 | Rd  (sf=1)
    if ((next & 0xFF800000U) == 0x91000000U && ((next >> 5) & 0x1F) == rd &&
        (next & 0x1F) == rd) {
      uptr off = DecodeAddImm(next);
      return page + off;
    }
  }
  // No matching add: the page alone is not a resolved address (returning it
  // would make any page-aligned target match), so fail (bug 29).
  return 0;
}

uptr ResolveAdrpAddForward(uptr adrpPc, u32 rd) {
  return ResolveAdrpAdd(adrpPc, rd);
}

std::vector<uptr> FindAdrpXrefs(uptr moduleBase, size_t moduleSize, uptr target,
                                size_t maxResults) {
  std::vector<uptr> out;
  if (!moduleBase || moduleSize < 8) return out;
  const u32* code = reinterpret_cast<const u32*>(moduleBase);
  const size_t count = moduleSize / 4;
  for (size_t i = 0; i + 1 < count && out.size() < maxResults; ++i) {
    u32 insn = code[i];
    if (!IsAdrp(insn)) continue;   // adrp (any immlo)
    uptr pc = moduleBase + i * 4;
    // Bound the look-ahead to the instructions actually left in the module.
    uptr resolved = ResolveAdrpAdd(pc, insn & 0x1F, count - i);
    if (resolved == target) out.push_back(pc);
  }
  return out;
}

uptr XrefBlTarget(uptr moduleBase, size_t moduleSize, uptr pc) {
  if (!moduleBase || pc < moduleBase || pc + 4 > moduleBase + moduleSize) return 0;
  const u32 insn = *reinterpret_cast<const u32*>(pc);
  // bl: bits 31:26 = 100101. b: 000101.
  if ((insn & 0xFC000000U) != 0x94000000U) return 0;
  return arm64::BranchTarget(insn, pc);
}

uptr XrefAdrpAdd(uptr moduleBase, size_t moduleSize, uptr target) {
  auto v = FindAdrpXrefs(moduleBase, moduleSize, target, 1);
  return v.empty() ? 0 : v[0];
}

std::vector<uptr> XrefBls(uptr moduleBase, size_t moduleSize, uptr target,
                          size_t maxResults) {
  std::vector<uptr> out;
  if (!moduleBase || moduleSize < 4) return out;
  const u32* code = reinterpret_cast<const u32*>(moduleBase);
  const size_t count = moduleSize / 4;
  for (size_t i = 0; i < count && out.size() < maxResults; ++i) {
    const u32 insn = code[i];
    if ((insn & 0xFC000000U) != 0x94000000U) continue;   // bl
    const uptr pc = moduleBase + i * 4;
    if (arm64::BranchTarget(insn, pc) == target) out.push_back(pc);
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

SigScanner& SigCache::ScannerLocked() {
  // Rebuild the scanner when the mapping set has changed (a dlopen/dlclose),
  // so a library loaded later is visible and an unmapped one does not dangle
  // (bug 25).
  const u64 gen = ProcessImages::Generation();
  if (!scanner_ || scannerGeneration_ != gen) {
    scanner_ = std::make_unique<SigScanner>(ProcessImages::EnumExecutable());
    scannerGeneration_ = gen;
  }
  return *scanner_;
}

void SigCache::SetExpectedBuild(const BuildLibrary& build) {
  std::lock_guard<std::mutex> lk(mutex_);
  expectedBuild_ = build;
  buildGateOpen_.store(false);

  bool empty = build.name.empty() && build.base == 0 && build.textHash == 0 &&
               build.buildId.empty();
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
  // Fingerprint the live module and compare against the expected one. The
  // comparison uses the ASLR-stable build-id (and the module identity) rather
  // than the load base, which changes on every launch (bug 22).
  BuildLibrary live = BuildLibrary::Fingerprint(expectedBuild_.name.c_str());
  bool ok = expectedBuild_.verified && live.base != 0;
  if (ok && !expectedBuild_.buildId.empty()) {
    // A build-id is the strongest, ASLR-independent identity.
    ok = expectedBuild_.buildId == live.buildId;
  } else if (ok && expectedBuild_.inode != 0) {
    // Otherwise identify by file identity, not by load address.
    ok = expectedBuild_.inode == live.inode &&
         (expectedBuild_.dev.empty() || expectedBuild_.dev == live.dev);
  } else if (ok && expectedBuild_.textHash != 0) {
    // Fall back to a content hash, still ASLR-independent.
    ok = expectedBuild_.textHash == live.textHash &&
         (expectedBuild_.textSize == 0 || expectedBuild_.textSize == live.textSize);
  } else if (ok) {
    // Nothing ASLR-stable to compare: the fingerprint is unusable, so fail
    // closed rather than trusting a base address that changes every launch.
    ok = false;
  }
  if (ok) {
    buildGateOpen_.store(true);
    return true;
  }
  Log::Warn("BerryAPI: engine build fingerprint does not match signature table "
            "(expected buildId=%s hash=%llx, live buildId=%s hash=%llx) — "
            "signature features disabled",
            expectedBuild_.buildId.c_str(),
            (unsigned long long)expectedBuild_.textHash,
            live.buildId.c_str(),
            (unsigned long long)live.textHash);
  return false;
}

bool SigCache::BuildVerified() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return BuildVerifiedLocked();
}

namespace {

/// Splits "1.20.3" into {1, 20, 3}; a missing component is 0. A non-numeric
/// component is 0, so a malformed version sorts below any real one (fail
/// closed: a bad version does not accidentally match a rule).
void ParseVersion(const std::string& v, int out[3]) {
  out[0] = out[1] = out[2] = 0;
  size_t start = 0;
  int part = 0;
  while (part < 3 && start <= v.size()) {
    size_t dot = v.find('.', start);
    const std::string seg = v.substr(start, dot == std::string::npos
                                                ? std::string::npos
                                                : dot - start);
    int n = 0;
    for (char c : seg) {
      if (c < '0' || c > '9') { n = 0; break; }
      n = n * 10 + (c - '0');
    }
    out[part++] = n;
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
}

int CompareVersion(const std::string& a, const std::string& b) {
  int pa[3], pb[3];
  ParseVersion(a, pa);
  ParseVersion(b, pb);
  for (int i = 0; i < 3; ++i) {
    if (pa[i] != pb[i]) return pa[i] < pb[i] ? -1 : 1;
  }
  return 0;
}

}  // namespace

bool SigCache::VersionInRange(const std::string& version,
                              const std::string& minVersion,
                              const std::string& maxVersion) {
  if (version.empty()) {
    // No game version selected: only a rule with no bounds is eligible.
    return minVersion.empty() && maxVersion.empty();
  }
  if (!minVersion.empty() && CompareVersion(version, minVersion) < 0) return false;
  if (!maxVersion.empty() && CompareVersion(version, maxVersion) > 0) return false;
  return true;
}

void SigCache::SetGameVersion(const std::string& version) {
  std::lock_guard<std::mutex> lk(mutex_);
  gameVersion_ = version;
  // A version change invalidates cached resolutions of versioned rules.
  for (auto& e : entries_) {
    if (!e.second.minVersion.empty() || !e.second.maxVersion.empty()) {
      e.second.state = SigState::Unknown;
      e.second.results.clear();
    }
  }
}

void SigCache::RegisterVersioned(const std::string& id, const Signature& sig,
                                 const std::string& minVersion,
                                 const std::string& maxVersion) {
  Register(id, sig);
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& e : entries_) {
    if (e.first == id) {
      e.second.minVersion = minVersion;
      e.second.maxVersion = maxVersion;
      return;
    }
  }
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
  entries_.emplace_back(id, Entry{sig, {}, initial, {}, {}});
}

bool SigCache::Has(const std::string& id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& e : entries_)
    if (e.first == id) return true;
  return false;
}

size_t SigCache::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return entries_.size();
}

size_t SigCache::LoadOffsetCache(const std::string& json) {
  if (json.empty()) return 0;
  detail::json::Value root;
  if (!detail::json::Parse(json, &root) || !root.IsObject()) return 0;
  const detail::json::Value* buildId = root.Get("build_id");
  std::lock_guard<std::mutex> lk(mutex_);
  // A cache for another build is ignored outright: adopting it could resolve a
  // rule to an address in the wrong binary.
  if (!buildId || !buildId->IsString() ||
      expectedBuild_.buildId.empty() ||
      buildId->str != expectedBuild_.buildId) {
    return 0;
  }
  const detail::json::Value* offsets = root.Get("offsets");
  if (!offsets || !offsets->IsObject()) return 0;
  // Resolve offsets against the LIVE module base: the offset is ASLR-stable,
  // the absolute address is not, so the base must come from this process.
  const uptr liveBase = ProcessImages::MainModuleBase(expectedBuild_.name.c_str());
  if (liveBase == 0) return 0;
  size_t adopted = 0;
  for (const auto& kv : offsets->object) {
    if (!kv.second.IsNumber()) continue;
    const uptr off = (uptr)(u64)kv.second.number;
    for (auto& e : entries_) {
      if (e.first != kv.first) continue;
      // Cache the relative offset as a single pseudo-result; Resolve adds the
      // module base back. Only adopt when the rule is still Unknown.
      if (e.second.state == SigState::Found) break;
      e.second.results.assign(1, liveBase + off);
      e.second.state = SigState::Found;
      ++adopted;
      break;
    }
  }
  return adopted;
}

std::string SigCache::SaveOffsetCache() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::string out = "{\"build_id\":\"";
  out += expectedBuild_.buildId;
  out += "\",\"offsets\":{";
  bool first = true;
  for (const auto& e : entries_) {
    if (e.second.state != SigState::Found || e.second.results.empty()) continue;
    if (!first) out += ",";
    first = false;
    // Store the module-relative offset (result - base) so the cache survives
    // ASLR: the absolute address changes every launch, the offset does not.
    const uptr base = expectedBuild_.base;
    const uptr off = e.second.results[0] >= base ? e.second.results[0] - base
                                                 : e.second.results[0];
    out += "\"" + e.first + "\":" + std::to_string((unsigned long long)off);
  }
  out += "}}";
  return out;
}

size_t SigCache::LoadOffsetCacheFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return 0;   // a missing cache is not an error
  std::string body((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  return LoadOffsetCache(body);
}

bool SigCache::SaveOffsetCacheFile(const std::string& path) const {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f << SaveOffsetCache();
  return f.good();
}

uptr SigCache::ResolveAndRegister(const std::string& id, const Signature& sig) {
  Register(id, sig);
  return Resolve(id);
}

uptr SigCache::Resolve(const std::string& id, size_t occurrence) {
  Signature sig;
  {
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
      // A versioned rule whose range does not contain the selected game version
      // is Missing (improvement 27): the table carries the rule for other
      // builds, and the right one is picked automatically.
      if (!VersionInRange(gameVersion_, en.minVersion, en.maxVersion)) {
        en.state = SigState::Missing;
        return 0;
      }
      // Unknown: resolve now, but only when the build passes the gate.
      if (!BuildVerifiedLocked() || !expectedBuild_.base) {
        en.state = SigState::Missing;
        Log::Warn("BerryAPI: rule '%s' not resolved (build not verified) — "
                  "dependent feature disabled",
                  id.c_str());
        return 0;
      }
      sig = en.sig;
      break;
    }
  }
  if (sig.bytes.empty()) return 0;   // unknown id

  // Scan WITHOUT holding the cache mutex: a multi-hundred-MB scan must not
  // block every other cache user (bug 26). The scanner is rebuilt under the
  // lock first, then the scan runs on a local copy of its ranges.
  std::vector<MemRange> ranges;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    ranges = ScannerLocked().ranges();
  }
  SigScanner local(ranges);
  std::vector<uptr> results = local.FindAll(sig, 64);

  {
    std::lock_guard<std::mutex> lk(mutex_);
    for (auto& e : entries_) {
      if (e.first != id) continue;
      Entry& en = e.second;
      if (results.empty()) {
        en.state = SigState::Missing;
        Log::Warn("BerryAPI: rule '%s' not found in verified build — dependent "
                  "feature disabled",
                  id.c_str());
        return 0;
      }
      en.results = std::move(results);
      en.state = SigState::Found;
      if (occurrence < en.results.size())
        return en.sig.Resolve(en.results[occurrence]);
      return 0;
    }
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