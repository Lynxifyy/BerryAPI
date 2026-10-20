# Building

## Prerequisites

- Host: `g++` (C++17), `cc`, `make`, POSIX (`-pthread`, `-ldl`).
- Android: Android NDK (r23+), `cmake`, plus Dobby/ImGui (vendored).

## Host build (tests + injectable lib)

```sh
make test          # build + run all host suites + dlopen ABI check
make test-sanitize # same suites under ASan+UBSan (build/san/)
make lib           # build/build/libBerryAPI.so
make clean
```

Targets:

| Target | Output |
|---|---|
| `make test` | `build/test_test_*` + `build/dlopen_test` |
| `make test-sanitize` | `build/san/test_test_*` (`-fsanitize=address,undefined`) |
| `make lib` | `build/libBerryAPI.so` |

Flags used for the shared lib:

```
-fPIC -shared -fvisibility=hidden -DBERRY_BUILD_SHARED
```

Only the C ABI symbols are visible (see README).

## Android cross build

`tools/build-android.sh` drives the NDK clang toolchain and builds Dobby from
source via CMake, then links `libBerryAPI.so`:

```sh
./tools/build-android.sh ~/Android/Sdk/ndk/27.0.12077973
BERRY_ABI=armeabi-v7a ./tools/build-android.sh "$NDK"
```

Output: `build-android/<ABI>/libBerryAPI.so`.

Key defines on Android:

```
-DBERRY_PLATFORM_ANDROID=1 -DBERRY_HOOK_ENGINE_DOBBY=1
```

`render_backend.cpp` compiles its GLES overlay path only under
`BERRY_PLATFORM_ANDROID && !BERRY_RENDER_BACKEND_NULL`; otherwise it is a null
stub. `backend_dobby.cpp` is inert unless `BERRY_HOOK_ENGINE_DOBBY` is set.

## Vendored deps

- `third_party/Dobby` — build with CMake (`DOBBY_GENERATE_SHARED=ON`), link the
  produced `libdobby.so`.
- `third_party/ImGui` — application-level. The Render module does not hard-depend
  on ImGui; the seam/GLES path is ImGui-free by design.

## Testing

Test binaries:

| Suite | Covers |
|---|---|
| `test_math` | math/Mat4 helpers |
| `test_modules` | engine bootstrap, World+Combat+Render services, ESP |
| `test_net` | HTTP, WebSocket echo, JSON-RPC |
| `test_render` | camera projection, draw batching, backend factory |
| `test_sigscan` | signature parse/match, build-hash gate, fail-closed rules |
| `test_thread` | ThreadModel role routing + assertions |
| `test_fuzz` | deterministic fuzzers: WS `ParseWsFrame` + `SigScanner` |

Add a new suite by adding to `TEST_SRC` + the `test:` loop + the
`test-sanitize` prerequisites (it shares `$(TEST_SRC)`).

`dlopen_test` ensures the `.so` auto-attaches on load, that Inject/Detach are
idempotent, and re-inject works.

## Expoerted symbols

Verify the on-device export surface stays minimal:

```sh
nm -D --defined-only build/libBerryAPI.so
```