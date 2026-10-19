# Berry modules

BerryAPI ships exactly four strict modules. All are registered by
`CreateDefaultModules()` in dependency order (World → Combat → Render →
Network) so the registry attach/detach cycle is safe.

## `Berry::World` (`services.h` / `core/modules/world.cpp`)

Typed service: `WorldHandle`.

- `SetSeam / Seam` — bind a `LevelSeam` (shim on host, engine pointers on device).
- `RefreshCache / CachedEntities` — entity id cache refreshed every 20 ticks.
- `GetEntity(id) → EntitySnapshot` — position, velocity, rotation, health,
  alive flag, name.
- `GetBlock / SetBlock`, `GetTime / SetTime` — block access + time.
- `SendMessage` — chat/console message.
- `SetValue / GetValue / HasValue` — small per-module KV store.

## `Berry::Combat` (`combat.h` / `core/modules/combat.cpp`)

Typed service: `Combat`.

- `AddPreRule / AddPostRule / RemoveRule` — damage rules that can mutate or
  cancel a hit.
- `Damage(victim, attacker, amt, cause)` — runs the pipeline, returns
  `Applied | Cancelled`.
- `Kill(victim, attacker, cause)` — one-shot `Damage(…, 99999, …)`.
- `AreaDamage(center, radius, amt, cause, source)`.
- `RecordHit / RecordKill / Metrics(attacker)` — rolling DPS metrics.
- `RecentKills` — kill attribution window.
- `OnCancelled / OnApplied` — quest-style hooks.
- `DamageSeam::Apply` — the engine adapter: modules never call engine damage
  directly, they go through `Combat` → `DamageSeam`.

Rules returning `cancelled == true` block the hit. A pre-rule can alter
`finalDamage` before the engine applies it.

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

Types: `TcpSocket`, `HttpRequest`/`HttpResponse`, `WebSocket` (RFC6455 client +
frame handling), `JsonRpc` (2.0 parse/build), `RateLimiter`.

- HTTP/1.1 client with headers/body/timeouts.
- WebSocket client: handshake, masked client frames, unmasked server frames;
  `SendFrame`/`RecvFrame` match RFC6455 byte order (see the fix history for
  the mask-key byte-order bug).
- JSON-RPC 2.0: `ParseRequest` handles `method`, `params` object, `id`.

## Capability matrix

| Module | Caps | Touches |
|---|---|---|
| World | `kCapWorld` | Level seam, blocks, time |
| Combat | `kCapCombat` | Damage seam, rules |
| Render | `kCapRender \| kCapWorld` | Renderer, world for ESP |
| Network | `kCapNetwork` | sockets, JSON-RPC |