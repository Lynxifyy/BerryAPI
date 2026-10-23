# BerryAPI

"Dangerously powerful" native C++ shared library (`libBerryAPI.so`) for Minecraft
Bedrock modding & performance debugging. **Not** a behavior pack: no
`manifest.json`, no `@minecraft/server`.

## Architecture

- **Strict module set** (only these): `Berry::Combat`, `Berry::Render`,
  `Berry::World`, `Berry::Network`.
- **Injectables**: `BERRY_Inject` / `BERRY_Detach` / `BERRY_IsAttached` /
  `BERRY_Version` (C ABI, see `include/berry/export.h`).
- **Hook backends**: Dobby / And64InlineHook on device; `ShimBackend`
  (`core/hooks/backend_shim.cpp`) is a self-contained x86-64 hotpatch detour
  used for host tests.
- Modules attach through a thin service layer (`include/berry/services.h`,
  `core/modules/services.cpp`) that owns the `Combat`/`Renderer`/`WorldModule`/
  `Network` instances and registers them with `Engine`.

## Build

```sh
make test            # host unit tests (math, modules, net, script, hud, governor, dlopen ABI)
make test-sanitize   # the same suites under ASan + UBSan
make idl             # regenerate bindings/types/docs from api/berry.idl
make lib             # builds build/libBerryAPI.so
```

- Host build: `g++ -std=c++17 -Wall -Wextra -O2 -g -Iinclude`, POSIX sockets,
  pthreads, `-ldl`.
- Shared build: `-fPIC -fvisibility=hidden -DBERRY_BUILD_SHARED`; only
  `BERRY_API` symbols export.
- The `.so` ELF constructor auto-attaches on dlopen; it is compiled out
  (`BERRY_BUILD_SHARED` guard) for test binaries where `main()` drives the
  lifecycle.
- Object files carry header dependencies (`-MMD -MP`, `-include $(DEPS)`), so
  editing a header rebuilds every translation unit that includes it. Without
  this a stale object can link against a changed struct layout and crash.

## Scripting layer

- `api/berry.idl` is the single source of truth for the script API; run
  `make idl` after editing it. `make idl-check` (part of `make test`) runs the
  generator self-test and fails when the generated files drift.
- Scripts are capability-gated per call and budgeted per invocation; a fault
  puts the host in `Faulted` and it refuses further calls until reload.
- The capability gate fails closed. A binding's requirement defaults to
  `CapKind::Unknown`, which is **not** a grant and **not** grantable; a binding
  that needs no capability must declare `Capability::NoCap()` (the IDL
  `nocap`). `CapabilitySet::Add`/`Covers` reject non-grantable kinds, so
  "author forgot to declare a capability" can never read as "no capability".
  `test_idl` asserts no binding is capability-free by accident.
- The async layer resumes on the script worker thread; keep the scheduler's
  predicate evaluation outside its lock.

## Client modules

- `Hud`/`InputObserver`/`PvpOverlay`/`PerformanceGovernor` are client-side
  services that draw through `Berry::Render`; they are deliberately **not** new
  strict modules (the strict set stays Combat/Render/World/Network).
- These modules are local-only: they must never reveal information the client
  was not already sent (no ESP/reach/aim assist).
- `Hud::Present` re-lays out only dirty subtrees; call `SetText`/`SetSize`/
  `SetOffset` (which dirty the right nodes) rather than poking resolved
  geometry.
- The render frame path is the pipeline in `pipeline.h`. `Renderer::BeginFrame`
  rebuilds the frustum from the camera, adopts the governor's `RenderSettings`
  and derives the LOD tiers; `EndFrame` counts the frame and accounts its
  batch. World-space draws (`DrawBillboard`) cull against that frustum before
  projecting. A camera that is not set leaves the frustum invalid, which culls
  nothing — never cull blindly. `ScriptApi::PublishFrame` is what keeps the
  pipeline following the governor: it adopts the settled settings and feeds the
  frame time to the resolution controller. Cross-thread reads go through the
  locked `Renderer::pipelineStats`/`LodFor`/`Visible`/`lastBatchCounts`, not the
  raw `pipeline()` reference (which is for a caller that owns the frame).
- Draw primitives (`Rgba`, `Rect2D`, `Line2D`, `Text2D`, `Circle2D`,
  `Billboard3D`, `CameraState`, `RenderSeam`) live in `draw.h`, which both
  `render.h` and `pipeline.h` include. Keep the pipeline header free of
  `render.h` to avoid a cycle.
- `CosmeticService` resolves a loadout into draw items through the
  `CosmeticSkeletonSource`/`CosmeticRenderSeam` seams; never assume engine
  pointers in the core. A resolve is cached per entity and only reused while the
  skeleton seam reports an unchanged pose revision — a seam that cannot report
  one (`noRevision`) disables the cache so a stale attach transform is never
  reused. A seam swap clears the cache.
- Cosmetic model packs (`pack.h`) are data, loaded through the loader's
  `IScriptFileSystem` seam, and applied atomically via
  `CosmeticRegistry::RegisterAll`: a pack that is malformed, names an unknown
  key/slot/bone, or repeats an id registers nothing. `LoadDirectory()` reads only
  `*.pack.json`. `RealScriptFileSystem`/`MemoryScriptFileSystem` both implement
  `ListDir`; a seam without a listing simply finds no packs.
- `LanCosmeticTransport` (`lan_transport.h`) is the concrete
  `CosmeticSyncTransport` for a LAN: one UDP socket on a private multicast
  group, messages framed into MTU-sized fragments and reassembled per sender.
  The fragment header is 24 bytes and carries the fragment payload size, so a
  receiver with a different MTU computes the right offsets instead of guessing;
  the sender tag is assigned at `Open` (not at `Announce`), so a client skips
  its own multicast loopback copy even before it announces. It moves bytes only
  — the channel still verifies before anything is applied.
- `LanDiscovery` (`lan_discovery.h`) is a minimal mDNS (`_berry._udp.local`)
  responder+browser: it publishes PTR/SRV/TXT/A and caches peers with a TTL. It
  follows compression pointers on read and never writes one. Discovery carries
  no trust; a found peer is still verified by the channel. `SendTo` sends
  straight to a discovered peer. There is no chat seam in the library, so an
  in-game-chat signaling fallback belongs to the embedder (wrap the one-virtual
  `Send` seam), not to the transport.
- `Animator` (`animator.h`) is a client-side keyframed clip player, not a strict
  module. It owns no clock: `Advance(dtSeconds)` is the whole input, and the
  `ScriptApi` frame hook is what calls it (the `animator.advance` binding does
  not exist, because a second clock would double-step a clip). Keys are kept
  sorted by `(bone, time)`; sampling binary-searches one bone's run and lerps
  the bracketing keys. `AnimatedSkeletonSource` is the seam that makes it real:
  it adds the sampled pose to a base `CosmeticSkeletonSource`'s transform, so
  the cosmetic render path draws the animated rig. While a clip plays it reports
  no pose revision, which disables the cosmetic resolve cache for that entity.
  `Save`/`Load` use the versioned `berry-animator` document; `Load` refuses a
  wrong version, a duplicate name, an unknown bone or an out-of-range key, and
  parses into a scratch list so a bad document never disturbs the live clips.
- `ReplayCamera` (`replay_camera.h`) is the client feature on top of the tape:
  a keyframed camera timeline over replay ticks. Like the animator it owns no
  clock (`Advance(dtSeconds)` is the only time input), and the `ScriptApi`
  frame hook is its real caller — while `active()` it builds the frame's
  `CameraState` and hands it to the renderer *before* any overlay draws, so
  world-to-screen/3D geometry project through the replay pose. Sampling is
  `Linear` or `Smooth` (Catmull-Rom position; angles/FOV always a shorter-arc
  eased lerp, never a spline, so aim never overshoots). Keys are unique per
  tick and kept sorted. `ExportBegin`/`ExportStep` walk the timeline at a fixed
  fps so an offline render has a deterministic length; the frame hook steps it
  instead of wall time. `SetFollowTape(true)` makes the playhead the tape's own
  tick (the frame hook seeks to `recorder->ReplayTick()`), so a key at tick N
  shows at tick N of the recording — the mode a replay mod wants. Everything is
  bounded (`kMaxKeys`, FOV `[1,179]`, `kMaxSpeed`) and every bad value is
  refused, not clamped.

## Conventions

- Modules expose seams (`DamageSeam`, `RenderSeam`, `LevelSeam`) that get
  bound at runtime; use `BindSeam()` to swap a seam after module attach
  (e.g. in tests) — `Attach()` refuses double-attach.
- Keep modules strict: no entity/level pointer assumptions in the core; all
  world access goes through the seam/`WorldHandle`.
- WebSocket client frames are masked big-endian per RFC6455.

## Debugging

- `tests/shim_net.h` contains a loopback HTTP server and a WS echo server.
- Run a single suite: `make build/test_test_modules && ./build/test_test_modules`.
- `make test` and `make test-sanitize` (ASan+UBSan) must both stay green; the
  sanitizer build is slow (several minutes) — run it in the background and poll
  a log file rather than blocking the terminal.
- Header dependency tracking is real but fragile: the `-include` of the
  generated `*.d` files must stay *below* `include tools/sources.mk`, because
  `CORE_SRC`/`TEST_SRC` are empty above it and the include then expands to
  nothing (a silent no-op that leaves stale objects). Every compile rule passes
  `-MMD -MP`.
- `WsClientPool` callbacks capture a `weak_ptr<Conn>`, never a `shared_ptr`: the
  connection owns its socket, so a strong capture is a cycle that leaks the
  connection and its socket. Keep the same shape in any new owner/callback pair.

## Pull requests

- Opening a PR via the GitHub REST API or the `create_pr` tool returns
  `403 Resource not accessible by integration` in this environment. The `gh`
  CLI works instead: `GH_TOKEN="$Github" gh pr create ...`. All three tokens
  (`Github`, `GithubLynxifyy`, `GITHUB_TOKEN`) authenticate as `Lynxifyy`.
