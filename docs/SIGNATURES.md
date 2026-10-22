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
## OTA signature packs (no client rebuild)

A hardcoded table means a game update needs a new client build. An **OTA
signature pack** turns that into a data update: one JSON document carries the
signature rows and the struct layouts for a build, and applying it registers
both into the registries that already exist. Shipping a new pack file is enough;
the library is unchanged. See `include/berry/sigpack.h`.

A pack names the build it was written against and is gated on it, exactly like
the table it replaces:

```json
{
  "format": 1,
  "name": "berry-1.20.50",
  "build": "1a2b3c4d",
  "signatures": [
    { "id": "combat:damageFn", "pattern": "E8 ?? ?? ?? ?? 48 83 C4 20 5B C3" },
    { "id": "world:levelTick", "pattern": "F0 ?? ?? ?? ?? 4C 8D 05", "from_end": 4 }
  ],
  "layouts": [
    {
      "name": "Actor", "version": "1.20", "build": "1a2b3c4d", "size": 16,
      "fields": [
        { "name": "health", "type": "f32", "offset": 0 },
        { "name": "id", "type": "u64", "offset": 8 }
      ]
    }
  ]
}
```

Rules that make this safe to ship over the air:

- **Build-gated, fail closed.** `ApplySignaturePack` refuses the whole pack when
  its `build` does not equal the active fingerprint, and an empty active build
  refuses every pack. A pack for another build can never resolve to addresses in
  the live build — which would be worse than resolving nothing.
- **Atomic.** Every pattern is parsed and every layout validated at parse time,
  before anything registers. One bad row means the pack contributes nothing;
  the registries are never left half-updated.
- **Strict.** An unknown key, a duplicate signature id, a layout targeting a
  different build, or an over-cap document is refused as a whole.
- **Seam-read.** Packs load through `IScriptFileSystem`, so a device build
  routes reads through the game's own file layer and the host tests need no disk.

```cpp
MemoryScriptFileSystem fs;              // or RealScriptFileSystem on device
fs.Write("packs/1.20.50.sigpack.json", packJson);

LayoutRegistry layouts;                 // owned by the engine
SignaturePackLoader loader(&fs, "packs");
SigPackLoadReport rep = loader.LoadDirectory("1a2b3c4d",   // active fingerprint
                                             SigCache::Instance(),
                                             &layouts);
```

Only `*.sigpack.json` files are discovered; unrelated JSON in the directory is
ignored. A failed pack does not stop the others, and the report carries a
per-pack reason. The loaded layouts answer through `LayoutRegistry::Find`, which
already gates on the active build, so a decoded field read is bound to the same
fingerprint that gated the pack.
