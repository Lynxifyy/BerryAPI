// Minimal x86-64 instruction-length decoder. See include/berry/x86_length.h.
//
// This is the classic length-only decoder: prefixes, REX, 1/2/3-byte opcodes,
// ModRM/SIB, displacement and immediate. It never reads an operand's value and
// never writes memory. Anything it cannot classify confidently — VEX/EVEX, an
// opcode outside the tables — returns 0 so the caller fails closed.
#include "berry/x86_length.h"

#include "berry/types.h"

namespace Berry {
namespace {

using u8 = uint8_t;

enum Imm : u8 {
  kNone = 0,
  kIb,     // imm8
  kIw,     // imm16
  kId,     // imm32
  kIz,     // imm16/imm32 by operand size
  kIv,     // imm16/imm32/imm64 by operand size
  kIwIb,   // imm16 then imm8 (enter)
  kMoffs,  // a memory offset the size of the address size
};

bool IsLegacyPrefix(u8 b) {
  switch (b) {
    case 0xF0: case 0xF2: case 0xF3:
    case 0x2E: case 0x36: case 0x3E: case 0x26: case 0x64: case 0x65:
      return true;
    default:
      return false;
  }
}

// One-byte opcodes that carry a ModRM byte. Everything not listed as
// "no ModRM" does.
bool OneByteNoModrm(u8 op) {
  // AL/eAX-imm and the short forms without a ModRM byte.
  switch (op) {
    case 0x04: case 0x05: case 0x0C: case 0x0D:
    case 0x14: case 0x15: case 0x1C: case 0x1D:
    case 0x24: case 0x25: case 0x2C: case 0x2D:
    case 0x34: case 0x35: case 0x3C: case 0x3D:
    case 0x06: case 0x07: case 0x0E: case 0x16: case 0x17: case 0x1E: case 0x1F:
    case 0x27: case 0x2F: case 0x37: case 0x3F:
      return true;
    default:
      break;
  }
  if (op >= 0x50 && op <= 0x5F) return true;   // push/pop r64
  if (op == 0x60 || op == 0x61) return true;   // invalid in 64-bit, still 1 byte
  if (op == 0x68 || op == 0x6A) return true;   // push imm
  if (op >= 0x6C && op <= 0x6F) return true;   // ins/outs
  if (op >= 0x70 && op <= 0x7F) return true;   // jcc rel8
  if (op >= 0x90 && op <= 0x9F) return true;   // xchg/nop/cbw/cwd/... /lahf
  if (op >= 0xA0 && op <= 0xAF) return true;   // moffs / movs / cmps / test / stos
  if (op >= 0xB0 && op <= 0xBF) return true;   // mov r,imm
  switch (op) {
    case 0xC2: case 0xC3: case 0xC8: case 0xC9:
    case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
    case 0xD4: case 0xD5: case 0xD6: case 0xD7:
      return true;
    default:
      break;
  }
  if (op >= 0xE0 && op <= 0xEF) return true;   // loop/jmp/call/in/out
  switch (op) {
    case 0xF1: case 0xF4: case 0xF5:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB: case 0xFC: case 0xFD:
      return true;
    default:
      return false;
  }
}

Imm OneByteImm(u8 op) {
  switch (op) {
    case 0x04: case 0x0C: case 0x14: case 0x1C:
    case 0x24: case 0x2C: case 0x34: case 0x3C:
    case 0x6A: case 0x6B:
      return kIb;
    case 0x05: case 0x0D: case 0x15: case 0x1D:
    case 0x25: case 0x2D: case 0x35: case 0x3D:
      return kIz;
    case 0x68: case 0x69: case 0xE8: case 0xE9: case 0x9A: case 0xEA:
      return kId;
    case 0x80: case 0x83: case 0xA8: case 0xC0: case 0xC1:
    case 0xC6: case 0xCD: case 0xD4: case 0xD5:
      return kIb;
    case 0x81: case 0xA9: case 0xC7:
      return kIz;
    case 0xC2: case 0xCA:
      return kIw;
    case 0xC8:
      return kIwIb;
    default:
      break;
  }
  if (op >= 0x70 && op <= 0x7F) return kIb;
  if (op >= 0xB0 && op <= 0xB7) return kIb;
  if (op >= 0xB8 && op <= 0xBF) return kIv;
  if (op >= 0xE0 && op <= 0xE3) return kIb;
  if (op >= 0xE4 && op <= 0xE7) return kIb;
  if (op == 0xEB) return kIb;
  if (op >= 0xA0 && op <= 0xA3) return kMoffs;
  return kNone;
}

bool TwoByteNoModrm(u8 op) {
  switch (op) {
    case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0B:
    case 0x0E: case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x37: case 0x77:
    case 0xA0: case 0xA1: case 0xA2: case 0xA8: case 0xA9: case 0xAA:
      return true;
    default:
      break;
  }
  if (op >= 0xC8 && op <= 0xCF) return true;   // bswap
  return false;
}

Imm TwoByteImm(u8 op) {
  switch (op) {
    case 0x0F: case 0x70: case 0x71: case 0x72: case 0x73:
    case 0xA4: case 0xAC: case 0xBA:
    case 0xC2: case 0xC4: case 0xC5: case 0xC6:
      return kIb;
    default:
      return kNone;
  }
}

}  // namespace

size_t X86InsnLength(const u8* code, size_t maxLen) {
  return X86DecodeInsn(code, maxLen).length;
}

X86InsnInfo X86DecodeInsn(const u8* code, size_t maxLen) {
  X86InsnInfo out;
  if (!code) return out;
  size_t i = 0;
  bool opsize = false;   // 0x66
  bool addrsize = false; // 0x67
  bool rexW = false;
  bool isRelBranch = false;
  bool rel8 = false;

  // Prefixes. Legacy prefixes may repeat; REX must be the last and appears once.
  for (;;) {
    if (i >= maxLen) return out;
    const u8 b = code[i];
    if (b == 0x66) { opsize = true; ++i; continue; }
    if (b == 0x67) { addrsize = true; ++i; continue; }
    if (IsLegacyPrefix(b)) { ++i; continue; }
    if (b >= 0x40 && b <= 0x4F) { rexW = (b & 0x08) != 0; ++i; break; }
    break;
  }
  if (i >= maxLen) return out;

  const u8 op = code[i++];
  bool hasModrm = false;
  Imm imm = kNone;

  if (op == 0x0F) {
    if (i >= maxLen) return out;
    const u8 op2 = code[i++];
    if (op2 == 0x38) {
      if (i >= maxLen) return out;
      ++i;                       // third opcode byte
      hasModrm = true;
    } else if (op2 == 0x3A) {
      if (i >= maxLen) return out;
      ++i;                       // third opcode byte
      hasModrm = true;
      imm = kIb;
    } else {
      hasModrm = !TwoByteNoModrm(op2);
      imm = TwoByteImm(op2);
      // 0x0F 0x8x is Jcc rel32.
      if (op2 >= 0x80 && op2 <= 0x8F) isRelBranch = true;
    }
  } else if (op == 0xC4 || op == 0xC5 || op == 0x62) {
    // VEX (C4/C5) and EVEX (62) are not decoded: refuse rather than guess.
    return out;
  } else {
    hasModrm = !OneByteNoModrm(op);
    imm = OneByteImm(op);
    // rel8/rel32 branches and calls, plus the rel8 loop forms.
    if (op == 0xE8 || op == 0xE9) isRelBranch = true;
    if (op == 0xEB) { isRelBranch = true; rel8 = true; }
    if (op >= 0x70 && op <= 0x7F) { isRelBranch = true; rel8 = true; }
    if (op >= 0xE0 && op <= 0xE3) { isRelBranch = true; rel8 = true; }
  }

  bool ripRel = false;
  size_t relocOffset = 0;
  u8 modrm = 0;
  if (hasModrm) {
    if (i >= maxLen) return out;
    modrm = code[i++];
    const u8 mod = modrm >> 6;
    const u8 rm = modrm & 0x07;
    if (mod != 3) {
      if (rm == 4) {
        if (i >= maxLen) return out;
        const u8 sib = code[i++];
        const u8 base = sib & 0x07;
        if (mod == 0 && base == 5) i += 4;
        else if (mod == 1) i += 1;
        else if (mod == 2) i += 4;
      } else {
        if (mod == 0 && rm == 5) { ripRel = true; relocOffset = i; i += 4; }
        else if (mod == 1) i += 1;
        else if (mod == 2) i += 4;
      }
    }
    // Group 3 (0xF6/0xF7): only the /0 and /1 forms carry an immediate.
    if ((op == 0xF6 || op == 0xF7) && imm == kNone) {
      const u8 reg = (modrm >> 3) & 0x07;
      if (reg <= 1) imm = (op == 0xF6) ? kIb : kIz;
    }
  }

  // A rel32 branch's displacement is the first 4 bytes of its immediate.
  if (isRelBranch && !rel8) relocOffset = i;

  switch (imm) {
    case kNone: break;
    case kIb: i += 1; break;
    case kIw: i += 2; break;
    case kId: i += 4; break;
    case kIz: i += opsize ? 2 : 4; break;
    case kIv: i += rexW ? 8 : (opsize ? 2 : 4); break;
    case kIwIb: i += 3; break;
    case kMoffs: i += addrsize ? 4 : 8; break;
  }

  if (i > maxLen || i > 15) return out;
  out.length = i;
  out.ripRelative = ripRel;
  out.relative = isRelBranch;
  out.rel8Branch = rel8;
  out.relocOffset = relocOffset;
  out.relocSize = (ripRel || (isRelBranch && !rel8)) ? 4 : 0;
  return out;
}

}  // namespace Berry
