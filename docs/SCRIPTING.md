# Scripting API

BerryAPI's scripting layer runs untrusted scripts inside the game process
without giving them the process. Scripts are capability-gated, budgeted,
hot-reloadable and coroutine-native, and the whole surface is described once in
`api/berry.idl`.

## Layers

| Layer | Header / source | What it does |
|---|---|---|
| Manifests | `manifest.h`, `core/script/manifest.cpp` | Declared capabilities, Ed25519 signing, grants |
| Loader | `loader.h`, `core/script/loader.cpp` | The one path from a script directory to a running host: verify, then load |
| Runtime host | `script.h`, `core/script/{runtime,vm_ref,host}.cpp` | One script → one VM → one grant, with budgets and error isolation |
| Hot reload | `reload.h`, `core/script/reload.cpp` | Reload with state handoff and an in-game error overlay |
| Async | `async.h`, `core/script/async.cpp` | `await` over ticks, events and HTTP, resuming on the right thread |
| IDL codegen | `api/berry.idl`, `tools/idlgen.py` | C++ binding table + TS/Luau types + docs from one description |

## Capability manifests and signing

A script declares what it needs; the user approves once; the host enforces it
per call.

A manifest is a `manifest.json` next to the script:

```json
{
  "name": "my-mod",
  "version": "1.0.0",
  "entry": "main.luau",
  "author": "Lynxifyy",
  "capabilities": ["render", "network:api.example.com"],
  "source_sha256": "<hex sha256 of main.luau>",
  "payload_hash": "<hex sha256 of the canonical payload>",
  "signature_algo": "ed25519",
  "signature": "<hex ed25519 signature over the payload hash>"
}
```

- Capabilities are `(kind, scope)` pairs. A network grant scoped to a host only
  covers that host; `CoversCap` compares both kind and scope.
- The runtime refuses a call whose binding needs a capability the script was not
  granted (`ScriptStatus::CapabilityDenied`). It fails closed: an unknown
  binding and an ungranted binding are both denials, never an allow.

### What a signature covers

`ScriptManifest::CanonicalPayload` is the signed document: name, version,
entry, author, the sorted capability list and `source_sha256`. Description and
the signature itself are excluded, so cosmetic edits do not invalidate a
signature, and capability order does not change the hash.

`source_sha256` is the part that matters. Signing a capability list alone would
attest to what a script *says it needs* while leaving the code free to change,
so the loader refuses any manifest that does not bind a source hash and refuses
any entry file that does not match it. Swapping the script while keeping the
manifest is caught at load time.

Ed25519 is asymmetric, which is the point: the publisher holds the secret key,
the device is provisioned with the public key only, so a compromised client can
verify a script but cannot mint one. HMAC-SHA256 is still available as
`Sign`/`Verify` for local development, and the two algorithms are kept strictly
separate — an HMAC signature is never accepted where Ed25519 is required, or the
shared secret would let any verifier forge a signature.

The private half of the signing flow lives in `tools/berry-sign.py`:

```sh
python3 tools/berry-sign.py keygen --out berry-signing.key
python3 tools/berry-sign.py sign   --key berry-signing.key --dir scripts/my-mod
python3 tools/berry-sign.py verify --pub <hex> --dir scripts/my-mod
```

The canonical payload is implemented twice — in C++ (the verifier) and in Python
(the signer) — so a drift between them would reject every real script.
`tests/test_loader.cpp` pins a manifest that `tools/berry-sign.py` produced, and
`make check-signer` (part of `make test`) regenerates that vector and fails the
build if the two sides no longer agree.

### The loader

`ScriptLoader` is the only host factory. Nothing else constructs a `ScriptHost`,
which is what keeps "was this verified?" from depending on the call site:

```cpp
ScriptLoaderConfig cfg;
cfg.anchor = TrustAnchor::Ed25519Key(pubHex);
cfg.requireSignature = true;     // production
cfg.requireSourceHash = true;

ScriptLoader loader(cfg, &fileSystem);
LoadedScript script;
LoadOutcome outcome;
if (!loader.Load("scripts/my-mod", &script, &outcome, &ManifestStore::Instance())) {
  // outcome.error says why: bad-signature, source-hash-mismatch, not-approved, ...
}
```

- **Order of checks.** Parse, then the source binding, then the signature, then
  the approval gate. A manifest that carries no source hash reports
  `source-hash-missing` rather than a generic signature failure, so the two
  causes stay distinguishable.
- **The approval gate.** With a `ManifestStore`, a script runs with the
  *intersection* of what it declared and what the user granted, and a grant made
  for one version never applies to another — new code cannot ride an old
  approval.
- **Reload re-verifies.** The reloader re-reads and re-verifies on every poll,
  so an edited file that no longer matches its signed hash is refused and the
  last verified revision stays live.
- **Dev mode.** `ScriptLoaderConfig::DevMode()` turns both checks off for
  `make test` and for local iteration. It is a named constructor precisely so
  that the unsafe configuration is greppable.

### Network scope and the egress policy

A network grant names a host, and the host is enforced on every request:

```json
"capabilities": ["network:api.example.com"]
```

- The scope is a host, never a URL, a wildcard or a subdomain rule.
  `network:api.example.com` does not cover `evil.example.com`, does not cover
  `sub.api.example.com`, and a bare `network` (no scope) grants nothing.
  `network:*` is the only wildcard.
- The legacy `network:domain=api.example.com` spelling is accepted and
  normalized to the same host, so grants written either way behave the same.
- `http_get` only reaches **https** hosts, never an IP literal, and never a
  loopback, link-local, private, CGNAT or otherwise reserved address — the
  addresses an SSRF probe looks for, including cloud metadata
  (`169.254.169.254`). Internal names (`localhost`, `intranet`, `box.local`)
  are refused as names.
- Redirects are followed by the caller, not the client, and every hop is
  re-checked against both the policy and the grant. A granted host cannot
  bounce a request to a host the script was never granted.

The policy is a pure function (`UrlPolicy::Check`), so it is unit-testable and
reproducible; the harness relaxes it for loopback only.

## Runtime host, budgets and error isolation

`ScriptHost` owns one VM and one grant. Every hook invocation runs under
`ScriptLimits`:

| Limit | Default | Meaning |
|---|---|---|
| `instructionBudget` | 200 000 | VM instructions per invocation |
| `memoryLimitBytes` | 8 MiB | VM-managed bytes the script may hold |
| `wallClockMs` | 100 ms | Wall-clock guard for native calls |

Exceeding any limit, hitting an error, or being interrupted faults the host
(`ScriptHostState::Faulted`). A faulted host refuses further calls until it is
reloaded — one bad script cannot take down the frame loop or corrupt another
script's VM.

Scripts run on the dedicated script worker thread (`ScriptWorker`), stamped
`ThreadRole::Scripts`. Everything is serialized on that one thread, so scripts
never run concurrently with each other and never touch the render thread.
Blocking native calls are allowed there and nowhere else.

## Hot reload with state handoff

`ScriptReloader` swaps a script's source and carries its state across:

1. `SerializeState()` snapshots the script-visible state (a plain string).
2. The new source is loaded into a fresh VM.
3. `RestoreState()` replays the snapshot into it.

If the new source fails to load or fails to restore, the reload is rejected and
the old script keeps running. `ScriptErrorOverlay` collects the error and
`DrawErrorOverlay(renderer, overlay, screen)` paints it in-game, so a broken
edit is visible immediately instead of silently stopping the mod.

## Coroutine-native async

`await` suspends the invocation; the scheduler resumes it on the script thread
with the awaited value.

```
await async.wait(ticks(20))          // 1 second
local evt = await async.events_once("hit")
local res = await async.http_get("https://api.example.com/state", 2000)
```

- `TaskScheduler::Poll` evaluates predicates outside its lock, snapshots the
  pending set, and resumes in ascending-id order, so resumption is
  deterministic and cannot deadlock.
- A task that never becomes ready is bounded by its own timeout and faults the
  host rather than leaking a suspended invocation.

## IDL codegen

`api/berry.idl` is the single source of truth. `make idl` regenerates:

- `include/berry/generated_bindings.h` — the binding table (name, capability,
  arity, module) the runtime consults,
- `generated/berry.d.ts` and `generated/berry.luau` — typed definitions for
  autocomplete and compile errors,
- `docs/API.md` — the reference.

`make idl-check` (part of `make test`) runs the generator's self-test and fails
the build when the generated files drift from the IDL, so the code and the docs
cannot disagree.

### Binding the surface to services (`api_bindings.h`)

The table lists what scripts may call; `ScriptApi` is what makes it callable.
`InstallBindings` walks the generated table and registers one handler per
function, taking the capability and arity from the spec — a name that is not in
the table is refused, so an undocumented (and therefore ungated) binding cannot
be added by accident. The runtime still enforces the capability on every call;
`ScriptApi` only supplies the implementation.

Two threads are involved, and the split is deliberate. Scripts run on the
script worker (see `docs/THREADING.md`); the `Renderer`, `Hud` and governor run
on the render thread. A binding that draws, mutates a widget or reconfigures the
governor posts a command to a bounded `RenderCommandQueue` and returns
immediately; the queue is drained on the render thread, once per frame, by the
`berry:frame` listener (the same event `Engine::OnFrame` emits). A full queue
drops the newest command and counts it rather than growing without bound, so a
runaway script cannot exhaust memory. Reads that the render thread publishes —
screen size and frame time — are answered from atomics, so a script never
touches render state directly. Widget creation returns a script-side handle at
once; the real `WidgetId` is filled in when the create command drains, and every
later call resolves handle to id at drain time so the script's call order is the
order they execute. Invalid arguments (a non-positive zoom, an out-of-range
target FPS, an unknown tween property) fail the call instead of silently
clamping.

## Client modules

The client-facing modules are services that draw through `Berry::Render`, not
new strict modules — the strict module set stays
`Combat`/`Render`/`World`/`Network`.

### HUD (`hud.h`)

A retained-mode widget tree: `Panel`, `Label`, `Bar`, `Graph`, `Row`, `Column`,
with nine anchors, flex-style flow, a theme with semantic colour roles, and
easing/tween support. Geometry is cached — only dirty subtrees are re-laid out
each frame. Tweening retargets from the current animated value, so a
fast-changing number (DPS, health) slides instead of jumping.

### Input (`input.h`)

Read-only observation of the local player's input: WASD, jump, sneak, sprint
and mouse buttons, plus a rolling clicks-per-second window and held durations.
This is data a normal client can already see. Unknown key names are rejected.

### PvP overlay (`pvp.h`)

CPS counter with graph, keystrokes with fade, combo counter, armor and effect
rows, ping/TPS/FPS line, target panel, session stats, floating damage numbers,
hit flash and a low-health vignette. Every widget renders state the local
client already owns, so the overlay cannot become an ESP.

### Performance governor (`perf.h`)

The adaptive FPS governor: it watches rolling frame time and trades quality for
frames through `QualityLevel`, mapped onto concrete settings by a
`QualityProfile`. It reduces one knob at a time (particles first, render
distance last) and restores more slowly, with a dead band that absorbs jitter.

Thermal awareness is what keeps it stable: a device that is already hot is
throttling itself, so the governor clamps to a low ceiling on
`Serious`/`Critical` and refuses to restore until the device cools. `CullPolicy`
turns the current settings into the render hooks' cull/keep, animate/don't and
spawn/don't decisions.

### Cosmetics (`cosmetic.h`, `cosmetic_sync.h`)

A cosmetic is data attached to a slot on the player skeleton: capes, wings,
hats, backpacks, auras, trails, hit effects and emotes. `CosmeticSlot` says
where an asset may sit and `Bone` says what it hangs from; a `CosmeticAsset`
carries the geometry/texture ids and the attach transform. Assets live in the
`CosmeticRegistry` (with a small built-in set from `RegisterBuiltins()`), and a
`CosmeticLoadout` holds at most one asset per slot. `CosmeticWardrobe` keeps
named presets so a player can switch loadouts.

Rendering is local-only by default. `CosmeticService` resolves a loadout into
draw items through a `CosmeticSkeletonSource` (a world transform per bone) and
submits them to a `CosmeticRenderSeam`; `ComposeAttach` scales the asset offset
by the bone and rotates it by the bone's yaw so a cape stays behind the back as
the player turns. Nothing is drawn for a remote player until a verified loadout
arrives.

Attached assets follow the player's motion. A `CosmeticAsset` carries a `trail`
factor (0 rigid .. 1 fully slack); `ComposeAttach` folds in a trailing pose —
lean back and lag opposite the motion, saturating at `CosmeticTrailFullSpeed`.
Motion comes from the bone itself, or, when the skeleton seam reports none, from
a `LocalVelocitySource` the service injects. A rigid asset, a still player, or a
non-finite velocity yields an all-zero pose (no fabricated sway). Scripts read
the same vector as `input.velocity()`.

Resolves are cached per entity. A cached entry is reused only while the skeleton
seam reports an unchanged pose revision (`CosmeticSkeletonSource::PoseRevision`),
the loadout and the injected velocity are all unchanged; a seam that cannot
report a revision (`noRevision`) disables the cache, so a stale pose is never
reused. The cache is bounded and LRU-evicted, and a seam swap clears it.
`CosmeticStats::cacheHits`/`cacheMisses` expose the policy.

Seeing another player's cosmetics needs a channel, and that channel is the
security boundary. `CosmeticSyncChannel` signs a canonical `(player, loadout)`
payload (Ed25519 in production, HMAC for local development), frames it with a
versioned envelope and hands it to a `CosmeticSyncTransport` (a WebSocket on
device). Receiving fails closed at every step — bad framing, unknown or
mismatched algorithm, a signature that does not verify over the exact bytes, or
a loadout body that does not parse all drop the message with no state change.
Even a validly signed loadout is applied through
`CosmeticService::SetRemoteLoadout`, which keeps only assets the named player
provably owns (`CosmeticOwnership`), so a compromised peer cannot wear someone
else's cosmetics.

Scripts reach the local loadout through the `cosmetic.*` bindings (see
[API.md](API.md)): `equip`, `unequip`, `equipped`, `list`, `save_preset`,
`apply_preset`, `presets` and `set_local_only`. All are client-side state — no
world write and no other player affected — and gated on the `cosmetic`
capability.

## The intent layer

`World` and `Combat` are read-only. Writes go through intents that a policy
layer validates: allow what normal player input could do or what stays
client-side (zoom, freelook, FOV, keybinds, HUD, sounds, perspective). Server-
aware policy profiles can auto-disable capability groups on servers that ban
them. Everything above is local-only by construction, which is what keeps users
unbanned.
