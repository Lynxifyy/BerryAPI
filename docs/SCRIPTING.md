# Scripting API

BerryAPI's scripting layer runs untrusted scripts inside the game process
without giving them the process. Scripts are capability-gated, budgeted,
hot-reloadable and coroutine-native, and the whole surface is described once in
`api/berry.idl`.

## Layers

| Layer | Header / source | What it does |
|---|---|---|
| Manifests | `manifest.h`, `core/script/manifest.cpp` | Declared capabilities, SHA-256 script signing, grants |
| Runtime host | `script.h`, `core/script/{runtime,vm_ref,host}.cpp` | One script → one VM → one grant, with budgets and error isolation |
| Hot reload | `reload.h`, `core/script/reload.cpp` | Reload with state handoff and an in-game error overlay |
| Async | `async.h`, `core/script/async.cpp` | `await` over ticks, events and HTTP, resuming on the right thread |
| IDL codegen | `api/berry.idl`, `tools/idlgen.py` | C++ binding table + TS/Luau types + docs from one description |

## Capability manifests and signing

A script declares what it needs; the user approves once; the host enforces it
per call.

```cpp
ScriptManifest m;
m.name = "my-mod";
m.capabilities = {
  Capability{CapKind::Render, "", "render"},
  Capability{CapKind::Network, "api.example.com", "network:api.example.com"},
};
m.signature = Sha256Hex(scriptBytes);   // script signing
```

- Capabilities are `(kind, scope)` pairs. A network grant scoped to a host only
  covers that host; `CoversCap` compares both kind and scope.
- The runtime refuses a call whose binding needs a capability the script was not
  granted (`ScriptStatus::CapabilityDenied`). It fails closed: an unknown
  binding and an ungranted binding are both denials, never an allow.

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

## The intent layer

`World` and `Combat` are read-only. Writes go through intents that a policy
layer validates: allow what normal player input could do or what stays
client-side (zoom, freelook, FOV, keybinds, HUD, sounds, perspective). Server-
aware policy profiles can auto-disable capability groups on servers that ban
them. Everything above is local-only by construction, which is what keeps users
unbanned.
