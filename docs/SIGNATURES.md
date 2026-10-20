# Signature scanning

BerryAPI resolves engine symbols **by byte pattern** rather than hardcoded
offsets so modules survive client updates. The scanner lives in
`include/berry/sigscan.h` / `core/hooks/sigscan.cpp`.

> **Preferred path (AArch64): ADRP-xref resolution.** Bedrock on arm64 refers
> to globals with `adrp/add` pairs; prefer resolving a symbol through
> `arm64::ResolveAdrpAdd` / `arm64::FindAdrpXrefs` (a *semantic* reference)
> over a raw byte pattern. Byte patterns have no semantics: a rearrangement
> of nearby instructions invalidates them silently, and you cannot tell what
> the bytes mean. The x86 example below is kept only as a fallback for
> non-arm64 tooling.

## Pattern format

A `Signature` takes a hex string where each byte is `xx` and `?` stands for a
single wildcard byte:

```cpp
Signature sig("mypattern", "48 8D 0D ? ? ? ? E8 ? ? ? ? C3", /*fromEnd*/0);
```

- `xx` one byte, `?` one wildcard byte.
- `fromEndOffset` — once the match is found, resolve the pointer *at that
  many bytes from the end* (e.g. a nearby load/lea or the address stored in a
  literal pool).

```cpp
uptr Match(const u8*) const;    // does pattern match at data?
uptr Resolve(uptr scanStart);   // scan + return pointer at scanStart+len-1-offset
```

## Scanning ranges

`ProcessImages::EnumExecutable()` reads `/proc/self/maps`, returns merged,
executable ranges. `MainModuleBase()` finds `libminecraftpe.so`.

```cpp
auto ranges = ProcessImages::EnumExecutable();
SigScanner scanner(ranges);
uptr p = scanner.Find(sig, &index);
```

`SigCache` is the central registry:

```cpp
SigCache& sc = SigCache::Instance();
sc.Register("player:takeDamage", Signature(...));
// at attach time:
sc.Resolve("player:takeDamage");
sc.ResolveAndRegister("level:tick", Signature(...));
```

Resolutions are cached with their occurrence index for multi-instance symbols.

## Build/library hash gate

Every signature table is written against a **concrete build**. Record the
fingerprint once at Preload; rules only resolve when the live module matches:

```cpp
BuildLibrary build = BuildLibrary::Fingerprint("libminecraftpe.so");

BuildLibrary expected;
expected.name = "libminecraftpe.so";
expected.base = 0x...;          // release base address
expected.textHash = 0x...;      // BuildLibrary::HashSection over the exported .text
expected.fileSize = ...;
expected.verified = true;
SigCache::Instance().SetExpectedBuild(expected);
```

`Fingerprint()` hashes the mapped executable ranges of the module
(FNV-1a 64; deterministic per build). When the runtime fingerprint does not
match the table's expected value, every rule goes to `SigState::Missing` and
`Resolve()` returns 0 — dependent features **disable themselves** instead of
scanning a foreign build.

## Rule-not-found state (fail closed)

`SigState` is the gate for every feature that uses a signature:

- `Unknown` — registered, not resolved yet.
- `Missing` — resolution failed, build not verified, or the table is closed.
- `Found` — resolved to a verified address.

Feature code must never guess. On `Missing` the feature stays hidden/off:

```cpp
uptr addr = SigCache::Instance().Resolve("world:levelTick");
if (SigCache::Instance().State("world:levelTick") != SigState::Found)
  return;   // feature off; do not attempt a wrong read
// safe to use addr
```

## AArch64 helpers

The engine (aarch64) heavily uses ADRP+ADD pairs to reference globals:

- `arm64::DecodeAdrp(insn, pc)` — returns the page the ADRP loads.
- `arm64::DecodeAddImm(insn)` — decodes the `add ×d, ×n, #imm12`.
- `arm64::ResolveAdrpAdd(pc, rd)` — follows `adrp x0, #page; add x0,x0,#mm` to
  a concrete 64-bit address.
- `arm64::ResolveAdrpAddForward(adrpPc, rd)` — scans forward for the add.
- `arm64::FindAdrpXrefs(moduleBase, size, target)` — finds all references to a
  global (vtables, caches).
- `arm64::BranchTarget(insn, pc)` — decodes `b`/`bl` targets (28-bit offset).

### arm64 walkthrough (preferred)

Resolving a global `g_playerList` (the bulletproof route):

```
adrp x0, #page            // x0 = page of g_playerList
add  x0, x0, #lo12        // x0 = &g_playerList
```

```cpp
// 1. Find the adrp PC with the scanner (or a dedicated signature row).
//    Register the row as usual; when it is Found, feed its address here.
uptr adrpPc = 0;              // from SigCache::Resolve("world:g_playerList") etc.
if (SigCache::Instance().State("world:g_playerList") != SigState::Found)
  return;                     // fail closed
// 2. Resolve the page+offset into the concrete global address.
uptr g_playerList = arm64::ResolveAdrpAdd(adrpPc, 0);
```

Xref-style resolution — "everything that references this global":

```cpp
// Every adrp+add pair that loads g_playerList, as instruction PCs.
auto refs = arm64::FindAdrpXrefs(moduleBase, moduleSize, targetAddr, 32);
```

Because the pair directly encodes the 21-bit page + 12-bit offset, the result
is the *address the code uses*, not a guess about what a byte pattern might
mean. Prefer this over a literal-pool `Resolve` when you control the table.

### x86 example (fallback only)

```cpp
Signature sig("mypattern", "48 8D 0D ? ? ? ? E8 ? ? ? ? C3", 0);
// lea rcx,[rip+disp32] + call — resolve the displacement into an address.
```

On x86 the pattern is only meaningful together with a `fromEndOffset`-based
pointer resolved from the embedded disp32; prefer the arm64 ADRP route on
device.

## Writing a signature table

Keep a versioned, **build-fingerprinted** table (id → hex pattern) in one
place per module. When a game update breaks lookups, update the fingerprint
and the table together; modules stay unchanged.

```cpp
static SignatureRow kV1_20_50[] = {
  { "combat:damageFn", "E8 ?? ?? ?? ?? 48 83 C4 20 5B C3" },
  { "world:levelTick", "F0 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? C6 45" },
};
```

Then in the module `Preload()`:

```cpp
for (auto& e : kV1_20_50)
  SigCache::Instance().Register(e.id, e.sig);
```

Resolution failures are logged with the rule id and put the rule in
`SigState::Missing`; the dependent feature disables itself.