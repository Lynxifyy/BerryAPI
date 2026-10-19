# Signature scanning

BerryAPI resolves engine symbols **by byte pattern** rather than hardcoded
offsets so modules survive client updates. The scanner lives in
`include/berry/sigscan.h` / `core/hooks/sigscan.cpp`.

## Pattern format

A `Signature` takes a hex string with `?` wildcards:

```cpp
Signature sig("mypattern", "48 8D 0D ? ? ? ? E8 ? ? ? ? C3", /*fromEnd*/0);
```

- `xx` one byte, `?` wildcard byte.
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

Example: resolving a global `g_playerList`:

```
adrp x0, #page         ; x0 = g_playerList page
add  x0, x0, #lo12     ; x0 = &g_playerList
```

```cpp
uptr ptr = arm64::ResolveAdrpAdd(pcOfAdrp, 0);
```

## Writing a signature table

Keep a versioned table (id → hex pattern) in one place per module. When a
game update breaks lookups, update only the table; modules stay unchanged.

```cpp
static SignatureTable kV1_20_50[] = {
  { "combat:damageFn", "E8 ?? ?? ?? ?? 48 83 C4 20 5B C3" },
  { "world:levelTick", "F0 ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? C6 45" },
};
```

Then in the module `Preload()`:

```cpp
for (auto& e : kV1_20_50)
  SigCache::Instance().Register(e.id, e.sig);
```