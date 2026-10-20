# BerryAPI Design

## Motivation

Behavior packs can only do what the Minecraft Bedrock engine exposes through
`@minecraft/server`. BerryAPI skips that layer entirely: it is a **native
`.so` injected into the game process** that hooks real game code, scans for
symbols by signature, and lets modules drive the engine directly. It exists
for performance debugging, private tooling, and understanding how the engine
works — not for distribution.

## Non-goals

- No `manifest.json`, no GameTest framework, no behavior-pack scripting.
- No bytecode VM. Everything is native C++ running in the game process.
- No distribution / marketplace support. Personal use only.

## Architecture

```
                      ┌─────────────────────────────────────────┐
                      │              libBerryAPI.so             │
 ┌────────────────┐   │  ┌────────┐──── ELF ctor ──┐           │
 │ hooks          │   │  │ hooks  │  BERRY_Inject │           │
 │ Dobby/shim     │──▶│  └────┬───┘   │           │           │
 │ VTablePatch    │   │       ▼         ▼           ▼           │
 │ Hook<T>        │   │  ┌────────┐ ┌──────┐ ┌──────────┐     │
 └────────────────┘   │  │ SigScan │ │Event │ │ Scheduler │     │
                      │  └────────┘ │ Bus  │ └──────────┘     │
                      │             └──────┘                   │
                      │  ┌───────────────────────────────────┐ │
                      │  │ Engine (singleton)                │ │
                      │  │  - ModuleRegistry (strict)        │ │
                      │  │  - attach/detach lifecycle        │ │
                      │  └───────────────────────────────────┘ │
                      │  ┌──────────┐ ┌─────────┐ ┌─────────┐  │
                      │  │Combat    │ │ World   │ │ Render  │  │
                      │  │Network   │ │ (seams) │ └─────────┘  │
                      │  └──────────┘ └─────────┘               │
                      └─────────────────────────────────────────┘
```

### Injection surface

`Engine::Bootstrap()` + `Engine::Attach()` are the canonical lifecycle. When
compiled with `BERRY_BUILD_SHARED`, an ELF constructor auto-attaches on
`dlopen`. The C ABI (`BERRY_Inject`, `BERRY_Detach`, …) gives loader tools an
idempotent entry point.

### Strict module system

Each `BerryModule` has:

- A unique `Berry::`-namespaced name (`Berry::Combat` …).
- A **capability mask** (`kCapRender | kCapWorld`) controlling what subsystems
  it may touch.
- An explicit `Preload → Attach → OnTick → Detach` lifecycle.
- A single typed **service object** (`Service<T>()`) reachable by sibling
  modules through `CombatFrom(reg)`, `RendererFrom(reg)`, `WorldFrom(reg)`.

The registry rejects duplicate names and enforces the namespace prefix, which
keeps module boundaries strict even in-process.

### Seams

The module-facing world is abstracted behind seams:

- `LevelSeam` — read-only entity/block/time access. World state through the
  public API is never mutated (v2 rule 0.2); test shims keep their own
  mutators for fake-state setup.
- `DamageSeam` — the damage pipeline boundary (observation in v2).
- `RenderSeam` — the draw batch consumer (device GLES or capture).

On the host, `tests/shim_level.h` implements a `ShimLevel` + `CaptureRenderSeam`
so every module is fully exercised headlessly.

### Render

`Renderer` is immediate-mode: `BeginFrame(screen) → draw* → EndFrame()`.
`EndFrame` presents to a `RenderSeam`. On device,
`core/modules/render_backend.cpp` provides a runtime-`dlsym` GLES2 overlay that
can be driven from an `eglSwapBuffers` hook. On the host it degenerates to a
null backend (`name() == "null"`).

## Host vs target

| Concern | Host (tests) | Target |
|---|---|---|
| Hook backend | `BerryShim` (x86-64 relative jump patch) | Dobby / And64InlineHook |
| Level | `ShimLevel` | Real `Level` via signature-resolved pointers |
| Render | `CaptureRenderSeam` | `GlesOverlayBackend` |
| Networks | local test sockets | game process sockets |