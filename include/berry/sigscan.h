#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include <memory>

namespace Berry {

// Resolution state of a registered signature. A feature that depends on a
// signature must check this and disable itself when the state is anything
// other than Found — fail closed, never guess.
enum class SigState : u8 {
  Unknown = 0,  // not registered yet
  Missing,      // registered but resolution failed (or build not verified)
  Found         // resolved to a verified address
};

const char* SigStateName(SigState s);

//----------------------------------------------------------------------------
// Signature scanning engine.
//
// The engine exposes functions to search executable memory for byte
// signatures, resolve AArch64 ADRP/ADD literal loads (the backbone of how the
// native Bedrock engine resolves globals), chase pointers, and scan for
// cross-references. This is what lets BerryAPI survive client updates with a
// single JSON signature table rather than hardcoded offsets.
//----------------------------------------------------------------------------

// A signature is a byte array plus a per-byte mask of the bits that must match.
// `wild[i]` is the fixed-bit mask (0xFF = a whole byte must match, 0x00 = a
// full wildcard, 0xF0 / 0x0F = a half-nibble wildcard). `mask` is kept as the
// "1 = fully fixed, 0 = fully wildcard" view for existing callers.
struct BERRY_API Signature {
  std::string id;
  std::vector<u8> bytes;
  std::vector<u8> mask;  // 0 = wildcard, 1 = must-match
  std::vector<u8> wild;  // bit mask of fixed bits (0xFF fully fixed)
  i32 fromEndOffset = 0; // offset from scan start to "resolve target"

  Signature() = default;
  Signature(std::string id_, const char* hexPattern, i32 fromEndOffset_ = 0);

  /// Parses a hex pattern. Accepts space-separated tokens in several styles:
  ///   "48 8D 0D ? ? ? ?"   (classic '?' wildcard byte)
  ///   "48 8D ?? 0D"        (IDA/Ghidra '??' wildcard byte)
  ///   "A? 10 ?B"           (half-nibble wildcards)
  ///   "\x48\x8D\x0D"       (C-escaped bytes)
  /// A single hex digit with no wildcard ("F") is a parse error, not 0xF0
  /// (bugs 14, 15). On failure the pattern is cleared and `error` (when given)
  /// explains why.
  bool Parse(const char* hex, std::string* error = nullptr);
  bool Match(const u8* data) const;
  uptr Resolve(uptr scanStart) const;
};

// Memory range descriptor used for scanning.
struct BERRY_API MemRange {
  uptr base;
  size_t size;
  std::string name;
  /// The `dev` field and inode of the backing file (both 0 for an anonymous
  /// mapping), so a range is identified by identity rather than by a path
  /// substring (bugs 19, 23).
  std::string dev;
  u64 inode = 0;
  /// True for an execute-only (`--x`) mapping, whose bytes are not readable
  /// directly (bug 18).
  bool executeOnly = false;
};

class BERRY_API ProcessImages {
 public:
  /// Enumerates the readable/executable (and execute-only) ranges of the
  /// current process, merged by backing file identity. The result is cached;
  /// call `Refresh` after a dlopen/dlclose so newly loaded libraries appear
  /// (bugs 24, 25).
  static std::vector<MemRange> EnumExecutable();
  /// Drops the cached maps parse so the next EnumExecutable re-reads
  /// /proc/self/maps.
  static void Refresh();
  /// A counter bumped on every Refresh, so a cached scanner can tell that the
  /// mapping set changed and rebuild (bug 25).
  static u64 Generation();
  /// The main game module (libminecraftpe.so) if identified.
  static uptr MainModuleBase(const char* name = "libminecraftpe.so");
  static size_t MainModuleSize();
  /// Loads an ELF section (e.g. .text) by reading the mapped file; used as a
  /// fallback if the in-memory layout is disturbed by runtime patching.
  static bool FindSection(uptr moduleBase, const char* sectionName, uptr* addr, size_t* size);
};

//----------------------------------------------------------------------------
// Build/library hash — fingerprint of the executable we are attached to.
//
// Every signature table is written against a concrete build. Before any rule
// resolves, record the build fingerprint once; if the fingerprint does not
// match the table's expected value, rules stay Missing and features disable
// themselves instead of scanning a foreign build (fail closed).
//----------------------------------------------------------------------------

struct BERRY_API BuildLibrary {
  std::string name;          // e.g. "libminecraftpe.so"
  uptr base = 0;             // module base address (informational; ASLR-variant)
  u64 textHash = 0;          // hash of the exported .text bytes (on disk)
  u64 fileSize = 0;          // size of the backing file, if available
  size_t textSize = 0;
  /// The ELF NT_GNU_BUILD_ID of the module, hex. This is the ASLR-stable
  /// identity the gate compares (bug 22); it is empty when it cannot be read.
  std::string buildId;
  /// The module's `dev` and inode, which identify it without relying on a path
  /// substring (bugs 19, 23).
  std::string dev;
  u64 inode = 0;
  bool verified = false;     // matched an expected fingerprint

  /// Fingerprints the main game module (or `name`) in the current process.
  static BuildLibrary Fingerprint(const char* name = "libminecraftpe.so");
  /// Hashes `len` bytes with FNV-1a. Retained for callers that need a cheap
  /// in-memory fingerprint; integrity checks use `HashSectionSha256`.
  static u64 HashSection(const u8* data, size_t len);
  /// SHA-256 (hex) of `len` bytes. Used to fingerprint the on-disk file rather
  /// than live, hook-patched memory (bugs 21, improvement 22).
  static std::string HashSectionSha256(const u8* data, size_t len);
  /// The NT_GNU_BUILD_ID of the ELF at `path` (hex), or empty. Reads the file,
  /// not memory, so it is unaffected by our own hooks.
  static std::string ReadBuildId(const char* path);
};

/// Scans `range` memory for the signature; returns resolved target or 0.
class BERRY_API SigScanner {
 public:
  explicit SigScanner(const std::vector<MemRange>& ranges);

  /// Finds the first occurrence of `sig` in the given range(s).
  uptr Find(const Signature& sig, size_t* outIndex = nullptr) const;

  /// Finds all occurrences (useful for deduplicating vtables).
  std::vector<uptr> FindAll(const Signature& sig, size_t maxResults = 32) const;

  /// True if a pattern matches at `addr`.
  bool MatchesAt(uptr addr, const Signature& sig) const;

  /// A rule that must match exactly once. On success `out` holds the address;
  /// on failure `matches` reports how many times it matched (0 = not found,
  /// >1 = ambiguous), so a failure explains itself (improvement 24).
  uptr FindUnique(const Signature& sig, size_t* matches) const;

  /// The address of the occurrence that differs from `sig` in the fewest
  /// fixed bits, and that difference count, so a rule that missed after a game
  /// patch can be updated quickly (improvement 25). Returns 0 when no range is
  /// long enough to hold the pattern.
  uptr ClosestMatch(const Signature& sig, size_t* differingBits) const;

  /// Scans the ranges in parallel on a small worker pool, stopping at the first
  /// hit (improvement 31). Falls back to a serial scan for a single range.
  uptr FindParallel(const Signature& sig, size_t workers) const;

  /// Scans every range ONCE for all of `sigs` at the same time (improvement
  /// 16), instead of one pass per signature. On return `hits[i]` is the first
  /// address of `sigs[i]`, or 0. This is the whole-table resolve: N signatures
  /// cost one walk of memory rather than N.
  ///
  /// Implementation: the scan is anchored on each signature's fixed prefix
  /// byte, so the inner loop advances through the shared stream once and
  /// checks the candidate set that could start at each position. It is exact
  /// (no false negatives); a wildcard-only signature falls back to a linear
  /// check at every position, which is correct if slower.
  void FindAllMulti(const std::vector<Signature>& sigs,
                    std::vector<uptr>* hits) const;

  /// The ranges this scanner was built over (a copy), so a caller can scan
  /// without holding a lock on the cache.
  const std::vector<MemRange>& ranges() const { return ranges_; }

  /// Scans with a SIGSEGV guard: if a page in the range is unmapped or
  /// unreadable mid-scan (a dlclose raced the scan), the fault aborts the scan
  /// and returns 0 instead of crashing the game (bug 20, improvement 29).
  uptr FindGuarded(const Signature& sig) const;

 private:
  std::vector<MemRange> ranges_;
};

//----------------------------------------------------------------------------
// AArch64 instruction helpers — resolving "adrp x0, #page" + "add x0, x0,
// #lo12" into a concrete global pointer, and branch/pcrel decoding for xrefs.
//----------------------------------------------------------------------------

namespace arm64 {

/// True when `insn` is an `adrp`. The opcode field is `0x90` in bits 28:24 and
/// bit 31 (op) is 1; immlo in bits 30:29 varies, so testing only bits 31:24
/// against 0x90 misses three quarters of adrp instructions (bug 27).
inline bool IsAdrp(u32 insn) {
  return (insn & 0x9F000000U) == 0x90000000U;
}

/// Decodes `adrp Rd, #imm` at `pc`; returns page address (Rd's value).
BERRY_API uptr DecodeAdrp(u32 insn, uptr pc);
/// Decodes `add Rd, Rn, #imm12` (with the optional `lsl #12`); returns the
/// offset applied. The `sh` field is ONE bit (bit 22) meaning `<<12`, not a
/// two-bit shift count (bug 28): reading bits 23:22 as a shift count returned
/// 2 for `add x0, x0, #1, lsl 12` instead of 4096.
inline uptr DecodeAddImm(u32 insn) {
  u32 imm = (insn & 0x3FFC00U) >> 10;             // imm12
  u32 sh = (insn & 0x00400000U) >> 22;            // 0=<<0, 1=<<12
  return (uptr)(imm << (sh ? 12 : 0));
}
inline u32 InsnOpcode(u32 insn) { return insn & 0x7C000000U; }

/// Follows `adrp; add` sequence at `pc`, returning the resolved 64-bit address.
/// `maxInsns` bounds how many instructions may be read from `pc` (the caller's
/// remaining span), so the look-ahead never reads past the end of a buffer
/// (bug 29). Pass the number of whole instructions available at `pc`.
BERRY_API uptr ResolveAdrpAdd(uptr pc, u32 rdTarget, size_t maxInsns = 8);
/// Scans forward from `pc` for `add Xd, Xd, #imm12` for the adrp's register.
/// Returns the resolved address, or 0 on failure.
BERRY_API uptr ResolveAdrpAddForward(uptr adrpPc, u32 rd);

/// Scans the module for all instructions that reference `target` via the
/// adrp+add pair, returning up to maxResults PCs. Classic xref finder. The scan
/// stays inside [moduleBase, moduleBase+moduleSize) and never reads past the
/// end (bug 29).
BERRY_API std::vector<uptr> FindAdrpXrefs(uptr moduleBase, size_t moduleSize,
                                          uptr target, size_t maxResults = 32);

/// Typed xref helpers (improvement 26). Each returns 0 (or an empty vector)
/// when the instruction is not of the expected form, and every look-ahead is
/// bounds-checked against [moduleBase, moduleBase+moduleSize).

/// Destination of a `bl #imm` / `b #imm` at `pc`, or 0 when `pc` is out of
/// range or the instruction is not an unconditional branch-with-link.
BERRY_API uptr XrefBlTarget(uptr moduleBase, size_t moduleSize, uptr pc);
/// The PC of the first `adrp; add` pair that resolves to `target`.
BERRY_API uptr XrefAdrpAdd(uptr moduleBase, size_t moduleSize, uptr target);
/// All PCs of `bl #imm` instructions that target `target` in the module.
BERRY_API std::vector<uptr> XrefBls(uptr moduleBase, size_t moduleSize,
                                    uptr target, size_t maxResults = 32);

/// Decodes `bl #offset` (BL/B) at `pc` -> destination address.
inline uptr BranchTarget(u32 insn, uptr pc) {
  i64 imm = ((insn & 0x03FFFFFFU) << 2);
  if (imm & 0x08000000) imm |= ~0x0FFFFFFFLL;  // sign-extend 28 bit
  return (uptr)((i64)pc + imm);
}

}  // namespace arm64

//----------------------------------------------------------------------------
// Central registry: id -> resolved address, with auto-find on first use.
//----------------------------------------------------------------------------

class BERRY_API SigCache {
 public:
  static SigCache& Instance();

  /// Registers a signature. Does not resolve until `Resolve(id)`.
  void Register(const std::string& id, const Signature& sig);

  /// Sets the build fingerprint this signature table was written against.
  /// Rules only resolve to Found when the runtime fingerprint matches.
  /// Pass an empty BuildLibrary to force every rule to Missing (fail closed).
  void SetExpectedBuild(const BuildLibrary& build);

  /// Resolves a registered signature to an absolute address (cached).
  /// Returns 0 when the build is not verified (state will be Missing).
  uptr Resolve(const std::string& id, size_t occurrence = 0);

  /// Directly resolves a signature and registers it under `id`.
  uptr ResolveAndRegister(const std::string& id, const Signature& sig);

  /// Current state of a signature. Feature code checks this before using a
  /// resolved address; anything other than Found disables the feature.
  SigState State(const std::string& id) const;

  bool Has(const std::string& id) const;
  size_t Count() const;
  bool BuildVerified() const;

 private:
  struct Entry {
    Signature sig;
    std::vector<uptr> results;
    SigState state = SigState::Unknown;
  };
  SigCache() = default;
  bool BuildVerifiedLocked() const;
  /// Rebuilds the scanner from the current maps when they have changed.
  SigScanner& ScannerLocked();
  std::vector<std::pair<std::string, Entry>> entries_;
  std::unique_ptr<SigScanner> scanner_;
  u64 scannerGeneration_ = 0;
  BuildLibrary expectedBuild_;
  mutable std::atomic<bool> buildGateOpen_{false};   // fingerprint verified
  std::atomic<bool> gateClosedPermanently_{false};  // empty fingerprint given
  mutable std::mutex mutex_;
};

}  // namespace Berry