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
make test            # host unit tests (math, modules, net, dlopen ABI)
make lib             # builds build/libBerryAPI.so
```

- Host build: `g++ -std=c++17 -Wall -Wextra -O2 -g -Iinclude`, POSIX sockets,
  pthreads, `-ldl`.
- Shared build: `-fPIC -fvisibility=hidden -DBERRY_BUILD_SHARED`; only
  `BERRY_API` symbols export.
- The `.so` ELF constructor auto-attaches on dlopen; it is compiled out
  (`BERRY_BUILD_SHARED` guard) for test binaries where `main()` drives the
  lifecycle.

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