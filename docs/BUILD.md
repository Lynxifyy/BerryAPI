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
| `make check-sources` | fails if a `core/`/`util/` unit is missing from `tools/sources.mk` |
| `make test-sanitize` | `build/san/test_test_*` (`-fsanitize=address,undefined`) |
| `make lib` | `build/libBerryAPI.so` |

Flags used for the shared lib:

```
-fPIC -shared -fvisibility=hidden -DBERRY_BUILD_SHARED
```

Only the C ABI symbols are visible (see README).

## Source manifest

`tools/sources.mk` is the single source of truth for the translation units:
`CORE_SRC` (portable units, built for both the host tests and the injectable
library), `ANDROID_EXTRA_SRC` (device-only units such as the Dobby glue) and
`TEST_SRC`. The host `Makefile` includes it, and `tools/build-android.sh` parses
it, so the two builds cannot drift apart — a unit added for tests is
automatically in the `.so`.

`make check-sources` (part of `make test`) fails when a `.cpp` under `core/` or
`util/` is not listed.

## Android cross build

`tools/build-android.sh` drives the NDK clang toolchain and builds Dobby from
source via CMake, then links `libBerryAPI.so`:

```sh
./tools/build-android.sh ~/Android/Sdk/ndk/27.0.12077973
BERRY_ABI=armeabi-v7a ./tools/build-android.sh "$NDK"
```

Output: `build-android/<ABI>/libBerryAPI.so` (a stripped AArch64/ARMv7 ELF that
exports only `BERRY_*` and `Berry::*`, per `tools/berry.exports.map`).

Key defines on Android:

```
-DBERRY_PLATFORM_ANDROID=1 -DBERRY_HOOK_ENGINE_DOBBY=1
```

`render_backend.cpp` compiles its GLES overlay path only under
`BERRY_PLATFORM_ANDROID && !BERRY_RENDER_BACKEND_NULL`; otherwise it is a null
stub. `backend_dobby.cpp` is inert unless `BERRY_HOOK_ENGINE_DOBBY` is set.

`--sources-only` prints the exact unit set the build would compile and exits
without touching the NDK. `make check-android-sources` (part of `make test`) uses
it to assert, on a plain host, that the Android build and `tools/sources.mk`
agree: every portable `core/`+`util/` unit is in the list, the device-only Dobby
glue is present, and the subsystems a device needs — mbedTLS, lz4, ed25519, the
replay container, the client host and the secure voice path — are all there. A
full `make check-android` (with an NDK on `PATH` or `$ANDROID_NDK_HOME`) builds
the `.so` and reports it.

## Vendored deps

- `third_party/Dobby` — build with CMake (`DOBBY_GENERATE_SHARED=ON`), link the
  produced `libdobby.so`.
- `third_party/ImGui` — application-level. The Render module does not hard-depend
  on ImGui; the seam/GLES path is ImGui-free by design.

### mbedTLS

mbedTLS is pinned to the latest 3.6 LTS patch (the release, its sha256 and why
that line and patch were chosen are in `third_party/mbedtls/PROVENANCE.md`). It
is a security dependency, so the pin is not allowed to drift silently:

- `make check-mbedtls` (part of `make test`) asserts that the version agrees in
  all three places it is written (the check script, `build_info.h` and
  `PROVENANCE.md`), that the vendored config is the trimmed client-only one
  (server role, DTLS, legacy ciphers and the PSK key exchanges are off) and that
  every vendored unit is either compiled or a documented empty-under-this-config
  exclusion. A half-applied upgrade, a config that leaked the server role, or a
  point release whose new unit nobody added to `tools/sources.mk` all fail the
  build.
- `make check-mbedtls-upstream` additionally asks GitHub for the newest release
  in the line and prints an advisory when the pin is behind it. It needs the
  network, so it is not part of `make test`; run it when triaging a security
  release. When it reports a newer patch, review that release's advisories,
  re-vendor the tree (headers, `library/*.c` and `library/*.h`, keeping the
  excluded set), regenerate `mbedtls_config.h` with upstream's own
  `scripts/config.py` and the same trim list, and bump the three version
  strings together.

## Testing

Test binaries:

| Suite | Covers |
|---|---|
| `test_math` | math/Mat4 helpers |
| `test_modules` | engine bootstrap, World+Combat+Render services, ESP |
| `test_net` | HTTP, WebSocket echo, JSON-RPC |
| `test_lan_transport` | UDP/multicast socket, LAN fragmentation+reassembly, mDNS discovery, signed-envelope end to end |
| `test_render` | camera projection, draw batching, backend factory |
| `test_sigscan` | signature parse/match, build-hash gate, fail-closed rules |
| `test_thread` | ThreadModel role routing + assertions |
| `test_fuzz` | deterministic fuzzers: WS `ParseWsFrame` + `SigScanner` |
| `test_scripttest` | the script test framework: discovery, assertions, outcome classification, fixtures, both VMs |

Add a new suite by adding it to `TEST_SRC` in `tools/sources.mk` and to the
`test:` loop; `test-sanitize` picks it up automatically (it derives from
`$(TEST_SRC)`).

`make test` also runs `check-run`, which drives the `berry-run` developer CLI
(see `docs/RUN.md`) end to end: every mode plus a refusal case per mode. The CLI
is thin argument parsing over the units the suites above cover, so this is a
smoke test of the wiring, not a second copy of the unit tests.

`dlopen_test` ensures the `.so` auto-attaches on load, that Inject/Detach are
idempotent, and re-inject works.

## Exported symbols

Verify the on-device export surface stays minimal:

```sh
nm -D --defined-only build/libBerryAPI.so
```