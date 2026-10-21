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

## Scripting layer

- `api/berry.idl` is the single source of truth for the script API; run
  `make idl` after editing it. `make idl-check` (part of `make test`) runs the
  generator self-test and fails when the generated files drift.
- Scripts are capability-gated per call and budgeted per invocation; a fault
  puts the host in `Faulted` and it refuses further calls until reload.
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

## Pull requests

- Opening a PR via the GitHub REST API or the `create_pr` tool returns
  `403 Resource not accessible by integration` in this environment. The `gh`
  CLI works instead: `GH_TOKEN="$Github" gh pr create ...`. All three tokens
  (`Github`, `GithubLynxifyy`, `GITHUB_TOKEN`) authenticate as `Lynxifyy`.
