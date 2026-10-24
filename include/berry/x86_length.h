#pragma once
// Minimal x86-64 instruction-length decoder, for the host hotpatch backend.
//
// A detour overwrites the first 5 bytes of a function with a `jmp rel32`. Those
// 5 bytes must cover *whole* instructions, or the trampoline's jump back lands
// in the middle of one: a `lock add` (7 bytes) overwritten by 5 bytes leaves 2
// stale bytes that execute as garbage. So the shim needs the length of each
// instruction in the prologue, and it must refuse to patch when it cannot
// decode one (fail closed) rather than guess.
//
// This is deliberately not a full disassembler: it decodes instruction length
// (prefixes, REX, 1/2/3-byte opcodes, ModRM, SIB, displacement, immediate) and
// returns 0 for anything it does not understand. It never writes memory and
// never dereferences an operand — it only walks the byte stream — so a wrong
// table entry can at worst mis-size an instruction, which the caller guards by
// requiring the total to be at least 5 and at most the pool stride.

#include "berry/export.h"

#include <cstddef>
#include <cstdint>

namespace Berry {

/// What the decoder learned about one instruction.
struct X86InsnInfo {
  size_t length = 0;        // 0 when undecodable
  /// The instruction reads an operand relative to RIP (a ModRM mod=00 rm=101
  /// memory form). Such an instruction cannot be relocated to a trampoline
  /// without rewriting its displacement.
  bool ripRelative = false;
  /// The instruction is a relative branch/call (rel8/rel32), which likewise
  /// cannot be moved without rewriting the target.
  bool relative = false;
  /// A relative branch with a rel8 displacement cannot be relocated (its reach
  /// is only +/-127 bytes, and a trampoline is not that close). The caller
  /// refuses such a prologue.
  bool rel8Branch = false;
  /// Byte offset (from the instruction start) and size (always 4) of the field
  /// that must be rebased when the instruction moves to a trampoline: the RIP
  /// displacement for `ripRelative`, or the rel32 branch displacement for
  /// `relative`. Valid only when one of those is set.
  size_t relocOffset = 0;
  size_t relocSize = 0;

  bool ok() const { return length != 0; }
};

/// Decodes the instruction at `code`. `code` must be readable for at least
/// `maxLen` bytes; the decoder only reads bytes it counts. Returns a zero-length
/// `X86InsnInfo` when the instruction cannot be decoded (so the caller fails
/// closed).
X86InsnInfo X86DecodeInsn(const uint8_t* code, size_t maxLen);

/// The length in bytes of the instruction at `code`, or 0 when it cannot be
/// decoded within `maxLen`.
///
/// On x86-64 the hard upper bound for one instruction is 15 bytes.
size_t X86InsnLength(const uint8_t* code, size_t maxLen);

}  // namespace Berry
