#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include <functional>
#include <string>
#include <vector>
#include <memory>

namespace Berry {

//----------------------------------------------------------------------------
// Signature scanning engine.
//
// The engine exposes functions to search executable memory for byte
// signatures, resolve AArch64 ADRP/ADD literal loads (the backbone of how the
// native Bedrock engine resolves globals), chase pointers, and scan for
// cross-references. This is what lets BerryAPI survive client updates with a
// single JSON signature table rather than hardcoded offsets.
//----------------------------------------------------------------------------

// A signature is a byte array where `0x00` bytes at positions covered by
// `mask` are wildcards.
struct BERRY_API Signature {
  std::string id;
  std::vector<u8> bytes;
  std::vector<u8> mask;  // 0 = wildcard, 1 = must-match
  i32 fromEndOffset = 0; // offset from scan start to "resolve target"

  Signature() = default;
  Signature(std::string id_, const char* hexPattern, i32 fromEndOffset_ = 0);

  /// Parses "xx xx x? xx ..." style pattern strings with '?' wildcards.
  bool Parse(const char* hex);
  bool Match(const u8* data) const;
  uptr Resolve(uptr scanStart) const;
};

// Memory range descriptor used for scanning.
struct BERRY_API MemRange {
  uptr base;
  size_t size;
  std::string name;
};

class BERRY_API ProcessImages {
 public:
  /// Enumerates readable/executable ranges of the current process, merged by
  /// mapping. On Android this is /proc/self/maps, on desktop /proc/self/maps.
  static std::vector<MemRange> EnumExecutable();
  /// The main game module (libminecraftpe.so) if identified.
  static uptr MainModuleBase(const char* name = "libminecraftpe.so");
  static size_t MainModuleSize();
  /// Loads an ELF section (e.g. .text) by reading the mapped file; used as a
  /// fallback if the in-memory layout is disturbed by runtime patching.
  static bool FindSection(uptr moduleBase, const char* sectionName, uptr* addr, size_t* size);
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

 private:
  std::vector<MemRange> ranges_;
};

//----------------------------------------------------------------------------
// AArch64 instruction helpers — resolving "adrp x0, #page" + "add x0, x0,
// #lo12" into a concrete global pointer, and branch/pcrel decoding for xrefs.
//----------------------------------------------------------------------------

namespace arm64 {

/// Decodes `adrp Rd, #imm` at `pc`; returns page address (Rd's value).
BERRY_API uptr DecodeAdrp(u32 insn, uptr pc);
/// Decodes `add Rd, Rn, #imm12`; returns the offset applied.
inline uptr DecodeAddImm(u32 insn) {
  u32 imm = (insn & 0x3FFC00U) >> 10;             // imm12
  u32 shift = (insn & 0xC00000U) >> 22;           // 0=byte,1=half,2=word,3=double
  return imm << (shift == 1 ? 1 : shift == 2 ? 2 : shift == 3 ? 3 : 0);
}
inline u32 InsnOpcode(u32 insn) { return insn & 0x7C000000U; }

/// Follows `adrp; add` sequence at `pc`, returning the resolved 64-bit address.
BERRY_API uptr ResolveAdrpAdd(uptr pc, u32 rdTarget);
/// Scans forward from `pc` for `add Xd, Xd, #imm12` for the adrp's register.
/// Returns the resolved address, or 0 on failure.
BERRY_API uptr ResolveAdrpAddForward(uptr adrpPc, u32 rd);

/// Scans the module for all instructions that reference `target` via the
/// adrp+add pair, returning up to maxResults PCs. Classic xref finder.
BERRY_API std::vector<uptr> FindAdrpXrefs(uptr moduleBase, size_t moduleSize,
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
  /// Resolves a registered signature to an absolute address (cached).
  uptr Resolve(const std::string& id, size_t occurrence = 0);
  /// Directly resolves a signature and registers it under `id`.
  uptr ResolveAndRegister(const std::string& id, const Signature& sig);

  bool Has(const std::string& id) const;
  size_t Count() const;

 private:
  struct Entry {
    Signature sig;
    std::vector<uptr> results;
    bool resolved = false;
  };
  SigCache() = default;
  std::vector<std::pair<std::string, Entry>> entries_;
  SigScanner* scanner_ = nullptr;
  SigScanner& Scanner();
};

}  // namespace Berry