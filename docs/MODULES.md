# Berry modules

BerryAPI ships exactly four strict modules. All are registered by
`CreateDefaultModules()` in dependency order (World → Combat → Render →
Network) so the registry attach/detach cycle is safe.

## `Berry::World` (`services.h` / `core/modules/world.cpp`)

Typed service: `WorldHandle`. **Read-only.** World access never mutates the
game; any former setter (entity position/velocity/health, block writes,
time writes, chat messages, applied damage) is removed from the public API.
The only writable surface is the module-local KV store.

- `SetSeam / Seam` — bind a `LevelSeam` (shim on host, engine pointers on device).
- `RefreshCache / CachedEntities` — entity id cache refreshed every 20 ticks.
- `GetEntity(id) → EntitySnapshot` — position, velocity, rotation, health,
  alive flag, name.
- `GetBlock`, `GetTime` — read-only block + time access.
- `SetValue / GetValue / HasValue` — small per-module KV store (module-local,
  not game state).

## `Berry::Combat` (`combat.h` / `core/modules/combat.cpp`)

Typed service: `Combat`. **Read-only (v2, 0.3).** The module observes damage
that the engine already applied. It cannot apply, cancel, or change damage;
the rules pipeline and the `Kill`/`AreaDamage` helpers were removed.

- `Observe(CombatEvent)` — feed an engine-applied hit into metrics +
  observers. Invalid events (zero victim, non-finite/negative damage) are
  ignored — fail closed.
- `RecordHit / RecordKill / Metrics(attacker)` — rolling DPS metrics
  (windowed, at least 1s before DPS is reported).
- `RecentKills` — kill attribution window.
- `OnApplied` — called for every observed hit.
- `DamageSeam::OnDamage` — the read-only adapter: modules use
  `Combat::Observe`, which forwards to the seam after validation.
- The `berry:combat` event bus emits every observed hit.

## `Berry::Render` (`render.h` / `core/modules/render.cpp`)

Typed service: `Renderer`.

- Immediate-mode draw gear (line/text/rect/circle + centered text) between
  `BeginFrame` and `EndFrame`.
- Camera: `Mat4` (perspective/look-at), `CameraState::WorldToScreen`,
  `Renderer::EspBoxForEntity` → ESP boxes for entity snapshots.
- Stats: `frameCount`, `frameMs`, `primitives`.
- Platform backend: `IRenderBackend` (see `render_backend.h`). On Android the
  GLES overlay is compiled in when `BERRY_PLATFORM_ANDROID` is defined and the
  host harness uses the null stub.

## `Berry::Network` (`net.h` / `core/net/net.cpp`)

Typed service: `Network`. **Never touches the render thread.**

Types: `TcpSocket`, `HttpRequest`/`HttpResponse`, `WebSocket` (RFC6455 client +
frame handling), `JsonRpc` (2.0 parse/build), `RateLimiter`.

- HTTP/1.1 client with headers/body/timeouts.
- WebSocket client: handshake, masked client frames, unmasked server frames;
  `SendFrame`/`RecvFrame` match RFC6455 byte order (see the fix history for
  the mask-key byte-order bug).
- `ParseWsFrame` — pure in-memory frame parser (fuzz-tested in
  `tests/test_fuzz.cpp`): reserved bits/opcodes, malformed control frames,
  oversized frames and truncated headers fail closed with
  `WsFrameStatus::{Invalid,Oversized,NeedMore}`.
- JSON-RPC 2.0: `ParseRequest` handles `method`, `params` object, `id`.

## Capability matrix

| Module | Caps | Touches |
|---|---|---|
| World | `kCapWorld` | Level seam, blocks, time |
| Combat | `kCapCombat` | Damage seam, rules |
| Render | `kCapRender \| kCapWorld` | Renderer, world for ESP |
| Network | `kCapNetwork` | sockets, JSON-RPC |

## Client services (not strict modules)

The client-facing features are services that draw through `Berry::Render`; they
are deliberately **not** added to the strict module set. See
[SCRIPTING.md](SCRIPTING.md) for the full walkthrough.

- `Hud` (`hud.h`) — retained-mode widget tree with anchors, flex flow, theming
  and tweens.
- `InputObserver` (`input.h`) — read-only local input and click-rate tracking.
- `PvpOverlay` (`pvp.h`) — local-only CPS/keystrokes/combo/armor/ping HUD.
- `PerformanceGovernor` (`perf.h`) — adaptive FPS governor with thermal
  awareness and a `CullPolicy` for the render hooks' keep/cull decisions.