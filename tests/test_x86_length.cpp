// Host tests for the x86-64 instruction-length decoder.
//
// The shim detour overwrites the first >=5 bytes of a function with a `jmp
// rel32`; those bytes must cover *whole* instructions, and every rel32/RIP
// displacement inside the overwritten span must be rebased when the span is
// relocated to a trampoline. A mis-sized instruction (bug 2) or a reloc field
// whose base is not the instruction end (bug 1) makes the trampoline jump into
// the middle of an instruction and crash the game, so the decoder is pinned
// here byte-for-byte.

#include "berry/types.h"
#include "berry/x86_length.h"

#include <cstdio>
#include <vector>

using namespace Berry;

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
  printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)

namespace {

struct Case {
  const char* name;
  std::vector<u8> bytes;
  size_t len;        // expected decoded length (0 = must refuse)
  bool rip = false;  // RIP-relative operand
  bool rel = false;  // relative branch/call
  bool rel8 = false; // rel8 form (not relocatable)
  size_t relocOff = 0;
};

void Run(const Case& c) {
  const X86InsnInfo info = X86DecodeInsn(c.bytes.data(), c.bytes.size());
  if (info.length != c.len) {
    printf("FAIL decode %-26s length %zu, want %zu\n", c.name, info.length, c.len);
    ++g_fail;
    return;
  }
  if (c.len == 0) return;   // a refusal carries no other guarantees
  if (info.ripRelative != c.rip || info.relative != c.rel ||
      info.rel8Branch != c.rel8) {
    printf("FAIL decode %-26s flags rip=%d rel=%d rel8=%d (want %d/%d/%d)\n",
           c.name, info.ripRelative, info.relative, info.rel8Branch, c.rip,
           c.rel, c.rel8);
    ++g_fail;
  }
  if (c.rip || (c.rel && !c.rel8)) {
    if (info.relocOffset != c.relocOff || info.relocSize != 4) {
      printf("FAIL decode %-26s reloc off=%zu size=%zu (want %zu/4)\n", c.name,
             info.relocOffset, info.relocSize, c.relocOff);
      ++g_fail;
    }
  }
}

}  // namespace

int main() {
  const std::vector<Case> cases = {
      // --- single-byte, no operand -------------------------------------
      {"ret",          {0xC3}, 1},
      {"int3",         {0xCC}, 1},
      {"nop",          {0x90}, 1},
      {"push rbp",     {0x55}, 1},
      {"leave",        {0xC9}, 1},
      {"cdq",          {0x99}, 1},

      // --- REX + ModRM --------------------------------------------------
      {"mov rbp,rsp",  {0x48, 0x89, 0xE5}, 3},
      {"sub rsp,0x20", {0x48, 0x83, 0xEC, 0x20}, 4},
      {"mov rax,[rbp-8]", {0x48, 0x8B, 0x45, 0xF8}, 4},
      {"mov rax,[rbp+d32]", {0x48, 0x8B, 0x85, 0x00, 0x00, 0x00, 0x00}, 7},
      {"mov rax,[sib abs]", {0x48, 0x8B, 0x04, 0x25, 0x00, 0x00, 0x00, 0x00}, 8},
      {"nop dword [rax+rax]", {0x0F, 0x1F, 0x44, 0x00, 0x00}, 5},
      {"endbr64",      {0xF3, 0x0F, 0x1E, 0xFA}, 4},

      // --- immediates by operand size (bug 2) ---------------------------
      {"push imm32",   {0x68, 0x34, 0x12, 0x00, 0x00}, 5},
      {"push imm16",   {0x66, 0x68, 0x34, 0x12}, 4},
      {"push imm8",    {0x6A, 0x05}, 2},
      {"imul imm32",   {0x69, 0xC0, 0x34, 0x12, 0x00, 0x00}, 6},
      {"imul imm16",   {0x66, 0x69, 0xC0, 0x34, 0x12}, 5},
      {"imul imm8",    {0x6B, 0xC0, 0x05}, 3},
      {"mov eax,imm32", {0xB8, 0x34, 0x12, 0x00, 0x00}, 5},
      {"mov rax,imm64", {0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8}, 10},
      {"mov rax,imm32", {0x48, 0xC7, 0xC0, 0x34, 0x12, 0x00, 0x00}, 7},
      {"add eax,imm32", {0x81, 0xC0, 0x34, 0x12, 0x00, 0x00}, 6},
      {"add eax,imm8", {0x83, 0xC0, 0x05}, 3},

      // --- group 3: imm only for /0 and /1 ------------------------------
      {"not eax",      {0xF7, 0xD0}, 2},
      {"test eax,imm32", {0xF7, 0xC0, 0x34, 0x12, 0x00, 0x00}, 6},
      {"test al,imm8", {0xF6, 0xC0, 0x05}, 3},

      // --- relative branches --------------------------------------------
      {"call rel32",   {0xE8, 0, 0, 0, 0}, 5, false, true, false, 1},
      {"jmp rel32",    {0xE9, 0, 0, 0, 0}, 5, false, true, false, 1},
      {"jmp rel8",     {0xEB, 0x05}, 2, false, true, true},
      {"je rel8",      {0x74, 0x05}, 2, false, true, true},
      {"je rel32",     {0x0F, 0x84, 0, 0, 0, 0}, 6, false, true, false, 2},
      {"jne rel32",    {0x0F, 0x85, 0, 0, 0, 0}, 6, false, true, false, 2},
      {"loop rel8",    {0xE2, 0x05}, 2, false, true, true},

      // --- RIP-relative operands (bug 1: reloc field may not be last) ----
      {"lea rcx,[rip+d32]", {0x48, 0x8D, 0x0D, 0, 0, 0, 0}, 7, true, false, false, 3},
      {"cmp dword[rip],imm8", {0x83, 0x3D, 0, 0, 0, 0, 0x05}, 7, true, false, false, 2},
      {"mov byte[rip],imm8", {0xC6, 0x05, 0, 0, 0, 0, 0x01}, 7, true, false, false, 2},
      {"mov eax,[rip+d32]", {0x8B, 0x05, 0, 0, 0, 0}, 6, true, false, false, 2},

      // --- refusals: the decoder must fail closed ------------------------
      {"vex c5",       {0xC5, 0xF8, 0x77}, 0},
      {"vex c4",       {0xC4, 0xE2, 0x79, 0x90, 0x04, 0x24}, 0},
      {"evex 62",      {0x62, 0xF1, 0x7C, 0x48, 0x28, 0xC0}, 0},
      {"truncated 0f", {0x0F}, 0},
      {"truncated mov", {0x48, 0x8D, 0x0D, 0x00}, 0},
  };

  for (const Case& c : cases) Run(c);

  // X86InsnLength is a thin wrapper over X86DecodeInsn: same answer, and a
  // refusal reports 0.
  CHECK(X86InsnLength((const u8*)"\xC3", 1) == 1);
  {
    const u8 vex[3] = {0xC5, 0xF8, 0x77};
    CHECK(X86InsnLength(vex, sizeof vex) == 0);
  }
  CHECK(X86InsnLength(nullptr, 16) == 0);

  if (g_fail == 0) printf("test_x86_length: ALL OK\n");
  return g_fail;
}
