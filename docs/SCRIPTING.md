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
- **TLS is not implemented yet.** Because the egress policy requires `https`
  for public hosts, a real public request currently fails closed: the client
  refuses `https://` rather than opening a plaintext socket to port 443. Only
  the harness path (loopback, where the policy is relaxed to permit `http`)
  completes end to end. This is deliberate — a silent downgrade would send the
  grant's traffic in the clear — and it is the reason `network.get` and
  `network.post` report a `status` of 0 with an `error` rather than raising.
- The response body is capped (4 MiB by default); a larger body is clipped and
  the `truncated` field is set, so a streaming server cannot exhaust the
  script worker's memory.

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

#### Read-only local reads (`Player`, `World`, `Entity`, `Perf`)

The read-only surface exposes state the local client already holds, and nothing
else. `Player.*` resolves the *local* player through the World seam; every read
fails closed (raises an error) when no local player is bound, so a script sees
"no player" rather than another entity's state or a fabricated vector. `World.*`
adds `entity_count`, `time`, `is_day` and `is_raining`, all through the same
seam, plus three richer reads:

- `world.entities_within(within, type)` returns a table keyed `"0"`, `"1"`, … of
  entity tables (`id`, `runtime`, `type`, `x/y/z`, `vx/vy/vz`, `yaw/pitch`,
  `health`, `max_health`, `alive`, `on_ground`, `name`, `distance`), sorted by id
  so the order is stable across ticks, with a `count` field. `type` is `""`,
  `"any"` or a name; an unknown name is refused rather than matching everything.
- `world.raycast_block(max_dist)` / `world.raycast_entity(max_dist)` cast from
  the local player's eye (feet + 1.62) along its look direction and return
  `{hit, …}`.
- `Entity.*` reads one entity by its **raw** id: `get`, `type_name`, `position`,
  `velocity`, `health`, `alive`, `distance_to`, and `nearest(type)` (the closest
  alive entity of a type, never the local player). A zero or unknown id, or an
  unbound seam, fails closed — a stale id yields "absent", never another
  entity's state.

`Perf.*` adds `stats` (avg/p95/p99/min/max/FPS), `quality` and
`set_platform`. Coordinates a script hands to the renderer are sanitised: a NaN
or infinity becomes 0 before it can reach the draw list.

## Client modules

The client-facing modules are services that draw through `Berry::Render`, not
new strict modules — the strict module set stays
`Combat`/`Render`/`World`/`Network`.

### Sandboxed file store (`fs.h`)

A script gets its own directory and nothing else. `ValidateScriptPath` is the
whole sandbox and it is a pure function of the path: no absolute path, no
backslash, no drive letter, no control character, no empty segment, no `.` and
no `..`, a bounded length and depth. On top of that sit caps — per file, in
total and on the file count — so a write loop cannot exhaust memory. Every
`fs.*` call fails closed (false / empty) rather than throwing.

### Audio cues (`audio.h`)

`audio.play` plays a named cue; the device backend decides what the name means.
A cue is a sound the local player hears, so nothing here is authoritative and
nothing leaves the client. A malformed name is refused, a muted play is dropped
rather than queued, and volume/pitch are clamped. With no backend bound, a play
reports false.

### Script modules + settings (`scriptmod.h`)

A script ships a MODULE: a named unit with metadata and a set of typed settings
(toggle, slider, select, keybind, text, button). The registry is pure data — it
never reads game memory — so it runs headless, and it is what the UI renders.
A setting's type is fixed at declaration, so a later write is validated against
it: a toggle accepts only 0/1, a slider rejects an out-of-range value, a select
rejects an out-of-range index, and a slider whose `max` is below its `min`, a
select with no options, or a duplicate/illegal key is refused outright. Nothing
clamps silently. A Button press is a one-shot edge, consumed by the read, so a
polling caller cannot see the same press twice. The `mod.*` bindings expose all
of this to a script; the module overlay (off by default) draws the list through
`Berry::Render`.

### HUD (`hud.h`)

A retained-mode widget tree: `Panel`, `Label`, `Bar`, `Graph`, `Row`, `Column`,
with nine anchors, flex-style flow, a theme with semantic colour roles, and
easing/tween support. Geometry is cached — only dirty subtrees are re-laid out
each frame. Tweening retargets from the current animated value, so a
fast-changing number (DPS, health) slides instead of jumping. Off-screen widgets
are culled before draw, and since an off-screen parent implies off-screen
children, a whole subtree is skipped in one step; `HudStats.widgetsCulled`
counts what was skipped.

### Input (`input.h`)

Read-only observation of the local player's input: WASD, jump, sneak, sprint
and mouse buttons, plus a rolling clicks-per-second window and held durations.
This is data a normal client can already see. Unknown key names are rejected.

### PvP overlay (`pvp.h`)

CPS counter with graph, keystrokes with fade, combo counter, armor and effect
rows, ping/TPS/FPS line, target panel, session stats, floating damage numbers,
hit flash and a low-health vignette. Every widget renders state the local
client already owns, so the overlay cannot become an ESP.

The `pvp.*` bindings (capability `hud`) expose the read-outs — `cps`, `combo`,
`session`, `armor`, `effects`, `net`, `target`, `health` — and the section
switches. `pvp.set_config` fully describes the config from a newline list of
enabled keys, so a key that is not named is off; an unknown key is refused and
leaves the config unchanged rather than silently dropping a section.

### Packet inspection (`packet.h`)

A read-only log of packet metadata: a sequence number, direction, protocol id,
a name, a byte length and a timestamp. It never stores payload bytes, and it is
built so it cannot — a packet's body is other players' data, and the useful
questions (am I being packet-spammed, is this id new since the update, how much
am I sending) are all answered by the header.

The log is bounded: a fixed ring of the most recent records plus running
totals, so a flood cannot grow it without limit. An oversized payload length is
refused rather than counted, so a hostile header cannot inflate the byte totals.

The `packets.*` bindings carry the distinct `packets` capability — inspecting
packets is a different authority from sending them, so it is not reachable with
a `network` grant:

- `packets.recent(max)` — newest first, `max` of 0 means all held; each entry is
  `{seq, dir, id, name, bytes, time_ms}`.
- `packets.last()` — the most recent entry, or nil.
- `packets.totals()` — `{inbound, outbound, inbound_bytes, outbound_bytes,
  dropped, total}`.
- `packets.summary(dir)` — per-id totals, highest count first, capped; `dir` is
  `"inbound"`, `"outbound"` or `""` for both.
- `packets.count_of(id)`, `packets.size()`, `packets.clear()`.

### Performance governor (`perf.h`)

The adaptive FPS governor: it watches rolling frame time and trades quality for
frames through `QualityLevel`, mapped onto concrete settings by a
`QualityProfile`. It spends quality in a fixed priority order (particles first,
render distance last) and restores in reverse, so the cheap knobs are exhausted
before an expensive one is touched and reduce/restore cannot ping-pong. Restore
is slower than reduce, with a dead band that absorbs jitter, and the governor
reacts to the p99 frame time (not just the average) so a smooth average cannot
hide regular stutter.

Thermal awareness is what keeps it stable. A device that is already hot is
throttling itself, so the governor clamps to a low ceiling on
`Serious`/`Critical` and refuses to restore until the device cools; a `Fair`
report (warm, not yet throttling) clamps early to a Fair ceiling. Every clamp
arms a dwell window during which restoration is blocked, because a device that
just throttled reports fast frames for a moment and restoring then would heat it
straight back up. `SetPlatform` selects a `QualityProfile` and thermal ceiling
for a device class (`MobileLow`/`MobileHigh`/`Desktop`/`Generic`), and
`CullPolicy` turns the current settings into the render hooks' cull/keep,
animate/don't and spawn/don't decisions, including per-category particle caps
and a screen-size cull for ambient particles. `FrameStats` reports p99 and
`FrameHistogram` shows the shape of the tail.

### Cosmetics (`cosmetic.h`, `cosmetic_sync.h`, `pack.h`)

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

A seam is the device backend, not the on-screen path. To actually draw, a
cosmetic goes through `Berry::Render` like every other overlay consumer:
`DrawLocal`/`DrawRemote` resolve the loadout and queue each item as a
world-space `Billboard3D`, and `Renderer::DrawBillboard` projects it with the
renderer's camera into the same 2D batch that presents through the one
`RenderSeam`. So a worn asset costs one projected quad per item and needs no new
3D state in the backend; a billboard the camera cannot see (invalid camera,
behind the camera, off-frustum, non-positive scale) is refused rather than drawn
at a bogus spot. `ScriptApi`'s frame hook calls `DrawLocal` after the HUD, so a
script that equips an asset sees it the same frame.

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

A signature proves *who* sent a message, not that sending it again is welcome,
so the channel defends against a valid-but-hostile peer as well. After the
signature verifies, `Receive` applies, in order: revocation, a per-peer token
bucket, and a replay window. A peer id on the revocation list (`RevokePeer`) is
refused outright, without rotating the shared key. The token bucket lets a peer
burst `rateBurst` messages and refills at `ratePerSecond`, so a stream of
individually-valid loadouts cannot flood the channel; the peer table is bounded
by `maxPeers`, evicting the least-recently-seen peer so a flood of distinct ids
cannot grow it without limit. The replay window remembers the digest of every
accepted message for `replayTtlMs` (bounded by `replayWindow`), so the same
signed loadout cannot be re-applied and re-broadcast after the fact. All three
run *after* the signature check, so an unauthenticated message never consumes a
token or fills the window. `Verify` stays a pure signature/shape check with no
side effects; the defences live in `Receive`. Limits are set through
`CosmeticSyncConfig`, and the clock is injectable so the TTL and the limiter are
deterministic in tests.

Scripts reach the local loadout through the `cosmetic.*` bindings (see
[API.md](API.md)): `equip`, `unequip`, `equipped`, `list`, `save_preset`,
`apply_preset`, `presets` and `set_local_only`. All are client-side state — no
world write and no other player affected — and gated on the `cosmetic`
capability.

Assets can also be authored as data. A **model pack** (`pack.h`) is a JSON
document — `format`, `name`, `assets` — that describes assets (id, slot, bone,
geometry/texture handles, offset, rotation, scale, tint, trail, tags); the
`CosmeticPackLoader` parses one through the loader's `IScriptFileSystem` seam
(never the host filesystem directly) and registers it. Loading is atomic: a pack
whose document is malformed, names an unknown key/slot/bone, or repeats an id
registers nothing, because the whole document is validated before any asset is
applied (`CosmeticRegistry::RegisterAll`). `LoadDirectory()` reads only
`*.pack.json` files, one failed pack does not stop the others, and the counts
are capped so a runaway directory cannot exhaust memory. The
`cosmetic.reload_packs()` binding triggers a directory load and returns how many
packs loaded cleanly.

## The intent layer

`World` and `Combat` are read-only. Writes go through intents that a policy
layer validates: allow what normal player input could do or what stays
client-side (zoom, freelook, FOV, keybinds, HUD, sounds, perspective). Server-
aware policy profiles can auto-disable capability groups on servers that ban
them. Everything above is local-only by construction, which is what keeps users
unbanned.

`intent.h` makes that concrete. An intent is a named request, classified once in
a static table (`ClassifyIntent`, pure — no per-call-site drift):

* **ClientSide** — what normal player input could do or what stays on this
  screen: `zoom`, `freelook`, `fov`, `perspective`, `keybind`, `hud`, `nametag`,
  `cosmetic`, `sound`. Allowed.
* **WorldWrite** — anything that would mutate the world or the player's
  authoritative state: `place_block`, `break_block`, `attack`, `interact`,
  `move`, `set_time`, `set_weather`. Denied. The client is not a second
  authority on the world.
* **Unknown** — an action the table does not name. Denied, so a typo or a future
  name fails closed.

Intents are grouped (Camera, Input, Hud, Audio, Cosmetic, World). A
`ServerPolicyProfile` matches a server address (`"*"`, an exact host, or a `*.`
suffix wildcard) and disables whole groups. `IntentPolicy::ForServer` picks the
most specific match — exact host beats a longer wildcard beats `"*"` — and
`IntentValidator::SelectFor` installs it for the session, so the profile follows
the server the client connects to. `BuiltinProfiles()` supplies an open default
plus the documented strict template.

The two layers are independent: classification says what may ever be done, the
profile says what may be done *here*. A profile cannot re-enable a world write —
`Validate` returns `WorldWrite` before it consults the mask — and a
classification of `Unknown` is denied before the profile too. `Validate(kind,
granted)` additionally requires the caller's grant set to cover the group's
capability, so the intent gate and the binding gate agree.

Scripts read the policy through `intent.*` (see [API.md](API.md)): `allowed`,
`classify`, `server`, `profile`, `disabled_groups` and `list`. The module carries
the `input` grant — the capability the client-side intent groups share — so a
script must already hold the capability whose policy it is querying. It only
reports what the policy decided; it cannot widen it. With no validator bound,
`allowed` fails closed (false) rather than reading as permitted.

## Script shaders and post-processing (`shader.h`)

A script can register a GLES fragment shader — a blur, a colour grade, a HUD
material — and put it on an ordered post-process chain. The design keeps the two
halves apart:

* **Validation is pure.** `ValidateShaderSource` needs no GL context, so the
  checks that matter run on the host and cannot be skipped by a device that
  fails to compile: bounded name (an identifier, at most `kMaxNameLen`), bounded
  source (`kMaxSourceBytes`), a `void main` entry point, no `#include` (which
  would pull in text that was never validated), and a bounded, well-formed
  uniform set. A refusal is a `ShaderReject`, so a script gets a reason rather
  than a silent no-op.
* **GL work is behind `IShaderCompiler`.** On the host the compiler is null and
  registration still succeeds — the chain can be built and inspected without a
  context. On device it is the GLES adapter.

**Hot reload is re-registration.** Registering an existing name replaces the
program in place: the chain position and every parameter whose uniform survives
the edit are kept, and parameters whose uniform disappeared are dropped. A
reload that fails validation leaves the old program untouched, so a broken edit
cannot blank the screen. A reload the *device* refuses (a compile error) also
keeps the previous program, and reports the reason through the error out-param;
that is deliberately not a validation error, because the source was well-formed.

The chain is the enabled effects in registration order (`Chain()`), which is
what the renderer walks. Only a registered shader can be enabled, and a
parameter must be a uniform the source actually declares, so a write that would
silently do nothing at draw time is refused up front.

Scripts drive it through `shader.*` (see [API.md](API.md)): `register`,
`unregister`, `has`, `list`, `uniforms`, `enable`, `chain`, `set_param`,
`param` and `clear`. The module carries the `render` grant, and every binding
fails closed when no registry is bound.

## Script packages (`package.h`)

A package is a JSON manifest plus an entry file. A script declares the packages
it needs and a version constraint for each; the resolver picks one version per
package, orders them so dependencies load first, and fails closed when no
consistent choice exists.

Three deliberate choices:

* **Semver is parsed, not string-compared.** `1.10.0` is newer than `1.9.0`, and
  a pre-release (`1.0.0-rc.1`) sorts before its release. Build metadata
  (`+sha.abc`) is accepted and dropped from the ordering, per the spec.
* **Constraints are normalised at parse time.** `^1.2.3` and `~1.2.3` become
  concrete `>=`/`<` pairs immediately, so satisfaction is a plain comparison and
  the parser and the resolver cannot drift. `,` is AND, `||` is OR, `*` is any.
* **Resolution is deterministic and bounded.** It is a fixpoint: recompute the
  constraints implied by the currently chosen versions, re-pick the highest
  version for each, repeat. If it does not stabilise within `kMaxRounds` it
  reports a conflict rather than guessing, and a cycle is reported as a cycle.

Nothing in the resolver touches the disk. A `PackageRegistry` is a set of
manifests handed to it, so resolution is testable without a filesystem and a
hostile manifest cannot cause I/O. Reading manifests is the loader's job.

The manifest is validated wholesale, the way a pack is: an unknown key, a
duplicate dependency, a bad name/version/entry or a malformed constraint
registers nothing. `entry` must be a safe relative path, the same rule the
script loader uses.

`ResolveRequire` maps a `require` path — `"pkg"` or `"pkg/sub/file.luau"` — to a
package and a file within it. A bare name resolves to the highest registered
version's entry; a sub-path must stay inside the package.

Scripts read the registry through `package.*` (see [API.md](API.md)): `add`,
`has`, `versions`, `resolve`, `require_path` and `clear`. The module carries the
`fileio` grant — it names packages that live on disk — and every binding fails
closed when no registry is bound.

## Struct layouts as data (`layout.h`)

A hooked function hands us a pointer to a game object, and reading a field of it
means knowing an offset, which changes whenever the game is rebuilt.
`StructLayout` moves that offset into a JSON table next to a signature, so a new
client build is a data update rather than a recompile.

The table is per-version and gated by a build fingerprint, exactly like the
signature table. A layout declares the `build` it was written against, and
`LayoutRegistry::Find` only returns a layout whose build matches the active
fingerprint. An empty fingerprint disables every layout, so the fail-closed
default is "read nothing", not "read a stale offset out of a foreign struct".
Reading the wrong offset is worse than reading nothing.

Decoding is deliberately narrow and explicit. A field declares its type, so a
read is a bounded, checked copy and never a blind reinterpret of memory:

* every read is bounds-checked against the layout's declared size *and* the
  number of bytes the caller supplied, so a truncated buffer fails rather than
  over-reading
* a pointer is only dereferenced by an explicit `FollowPointer` hop, one at a
  time, so a layout can never describe a dereference the caller did not ask for
* a string is copied with a cap, and a missing terminator is an error rather
  than a silent truncation
* a field must not overlap another, and no field may run past the struct end
* nothing here writes to game memory

Validation is wholesale, the way a pack or a package manifest is: an unknown
key, a duplicate or overlapping field, a bad type, a bad offset or a field past
the end registers nothing.

This is an internal subsystem, not a script module. Raw memory reads are not
exposed through `berry.*` on purpose: a script surface that read arbitrary
struct offsets would be an information leak and a crash waiting to happen. The
layout table is what a module uses internally to decode a hooked argument; a
script sees the decoded, already-vetted value.

## Reactive world queries (`world_query.h`)

Polling `world.query` every frame walks every entity every time. `SpatialGrid`
keeps a uniform grid keyed by cell, so a radius query touches only the cells the
radius overlaps, and a per-tick rebuild diffs against the previous tick.

It is a pure data structure: it never reads game memory, the caller feeds it
snapshots through `BeginTick`/`Add`/`EndTick`. That keeps `Berry::World`
read-only, makes the whole thing testable on the host with no game running, and
means the grid is a cache of what the seam already said, never a second source
of truth about the world.

Determinism matters as much as speed here. Query results come back sorted by id,
and the diff is computed against a stable previous set, so the same input always
produces the same events in the same order. `Updated` fires only when an
observable field actually changed, so an entity that merely moved within its
cell is not reported every tick.

`UpdateWorldGrid(grid, seam, tick, bus)` is the one call a module makes each
tick: it snapshots every entity the seam reports, ends the tick, and publishes
the diff. From a script, the diff arrives as events, no polling:

```
await Events.once("entityEntered")   -- "<id>:<type>"
await Events.once("entityLeft")      -- "<id>:<type>"
await Events.once("entityUpdated")   -- "<id>:<type>"
await Events.once("blockChanged")    -- "x,y,z,block"
```

`PublishWorldDiffToEvents` bridges the grid to the existing `Events` channels,
so no new script module is needed. The payload is a short copied string, never a
pointer into game memory.

A grid that fills to `kMaxEntities` refuses further adds rather than growing
without bound, and `overflowed()` reports it. A full grid is a bug or a broken
seam, not something to paper over.

## Record & replay (`record.h`)

The recorder sits between the seam and the world grid. Each tick it takes the
same `EntitySnapshot`s the grid would see, plus any script events a module
emits, and appends them to an in-memory tape. The tape serialises to one JSON
document, so a failing session can be saved, attached to a bug report, and
replayed on a developer machine.

Replay is a `LevelSeam` implementation (`ReplaySeam`). Feed it a tape and it
answers `AllEntities`/`GetEntity`/`GetTime`/`GetBlock`/raycasts from
the recorded frames, advancing one frame per `Step()`. Because it implements the
same seam every module already talks to, a script under test cannot tell it
apart from the live game, which is the whole point.

```
Recording tape;
Recorder rec(&tape);
rec.BeginFrame(tick, seam.GetTime());
rec.CaptureFrom(seam);           // every entity the seam reports
rec.AddEvent("entityEntered", "42:player");
rec.AddBlock(0, 64, 0, seam.GetBlock(Vec3(0, 64, 0)));
rec.CommitFrame();
...
Recording back;
Recording::FromJson(tape.ToJson(), &back);
ReplaySeam replay(&back);
while (replay.Step()) { /* the seam now answers frame by frame */ }
```

The tape is deliberately dumb and total:

* it stores snapshots, not pointers, so it is portable across builds and carries
  nothing that could be dereferenced on playback
* a malformed tape is rejected wholesale, like every other data document in
  BerryAPI; a partially-loaded tape never plays
* it is bounded (`kMaxFrames`, `kMaxEntitiesPerFrame`, `kMaxEventsPerFrame`,
  `kMaxBlocksPerFrame`), so a runaway recorder cannot exhaust memory
* ticks must strictly increase, so a tape always replays in order
* playback is read-only: `ReplaySeam` answers queries, it never writes to game
  memory and never mutates the tape

Before the first `Step` (and after the last frame) the seam reports an empty
world, so a consumer that runs past the tape ends up with nothing rather than a
stale frame. Blocks are recorded sparsely — only the coordinates a script
actually read — so a tape stays small and never tries to snapshot the world; a
replay raycasts only what was recorded and can never invent a block.

A tape records what this client saw. It is a debugging aid and a test fixture,
not a network protocol.

## Local RPC & REPL (`rpc.h`)

Tooling needs a way to talk to a running client: a VS Code extension, a
dashboard, an OBS overlay. `RpcServer` is a loopback JSON-RPC 2.0 endpoint for
exactly that.

The design splits cleanly in two, and the split is the point:

* `RpcServer` is the dispatcher. It maps a method name to a handler and turns a
  request frame into a response frame, with no sockets in it, so the whole
  protocol is testable by calling `HandleFrame` directly.
* `RpcServer::Start` wraps that dispatcher in a loopback TCP listener. The
  listener is the only part that touches the network.

The transport is a newline-delimited JSON-RPC stream over TCP. A WebSocket
server can wrap the same `HandleFrame`, so the protocol logic stays in one
place; the dispatcher does not know or care which transport carries it.

This is a local debugging surface, so the posture is deliberately narrow:

* it binds loopback only; a request to bind any other address is refused
* it is opt-in: nothing listens until `Start` is called
* frames are length-capped, methods are count-capped, and a per-connection rate
  limit answers a flood with a clear error instead of serving it silently
* a malformed frame is answered with a JSON-RPC parse error, never treated as a
  partial request
* an unknown method is a method-not-found error, never a silent success
* a notification (a request with no `id`) gets no reply, per the spec
* an `id` is echoed verbatim, so a number stays a number and a string stays a
  string
* a handler that throws becomes an internal error, never a crash

`RpcRepl` is a one-line front end over the same dispatcher:

```
add 1 2          ->  {"jsonrpc":"2.0","id":"repl","result":[1,2]}
world.query 20
methods
help
```

A bare number or a JSON literal is passed through as-is, so a handler can
receive a number; anything else becomes a quoted string. Every call goes through
`HandleFrame`, so the REPL cannot reach anything the RPC surface does not
already expose.
