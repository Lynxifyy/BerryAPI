# BerryAPI

A dangerously-powerful **native injected C++ shared library** (`libBerryAPI.so`)
for Minecraft Bedrock modding and performance debugging.

> **Personal use only.** BerryAPI is an injection framework — it modifies a
> running Bedrock process from native code. It is **not** a behavior pack: no
> `manifest.json`, no `@minecraft/server`. Use at your own risk; this project
> exists to push engine-level modding / debugging.

## What it is

| Layer | What you get |
|---|---|
| **Core** | Fingerprint-based **signature scanning** (survives client updates), memory helpers, **Dobby / shim hook backend**, event bus, scheduler, profiler, logger. |
| **Modules** | Strict, sandboxed namespace units — `Berry::Combat`, `Berry::Render`, `Berry::World`, `Berry::Network` — each with a typed service and a capability mask. |
| **Injector** | `libBerryAPI.so` with a small C ABI (`BERRY_Inject`, `BERRY_Detach`, …) and an auto-attach ELF constructor. |
| **Host harness** | The whole thing unit-tests **headlessly** on x86-64 via a `LevelSeam` shim — no game binary needed. |

## Repository layout

```
core/            engine, modules, hooks, sigscan, network
include/berry/   public headers (the SDK surface)
util/            logger + memory utilities
tests/           host test suite + shims + dlopen ABI test
third_party/     Dobby, ImGui (vendored, plain files)
tools/           build-android.sh (NDK cross build)
docs/            design, module, hooking, signature, build guides
samples/         example module
```

## Quick start (host, no game required)

```sh
make test          # builds + runs all host suites + dlopen ABI check
make lib           # builds build/libBerryAPI.so (injectable shared lib)
make clean
```

Requirements: `g++`/`clang++` (C++17), `cc`, `make`, POSIX (`-pthread`, `-ldl`).
No external dependencies; Dobby/ImGui are vendored.

## Target build (Android aarch64)

```sh
./tools/build-android.sh ~/Android/Sdk/ndk/27.0.12077973
# -> build-android/arm64-v8a/libBerryAPI.so
```

See [docs/BUILD.md](docs/BUILD.md) for NDK details, Dobby CMake, and hook flags.

## Docs

- [Design](docs/DESIGN.md) — architecture, strict module system, seams
- [Modules](docs/MODULES.md) — per-module API walkthrough
- [Hooking](docs/HOOKING.md) — shim vs Dobby backends, VTable patches
- [Signatures](docs/SIGNATURES.md) — writing signature tables, AArch64 xrefs
- [Threading](docs/THREADING.md) — tick/render/script threads and assertions
- [Build](docs/BUILD.md) — host build, NDK cross build, exports
- [samples/sample_hitmarker.cpp](samples/sample_hitmarker.cpp) — minimal module

## Exported C ABI

`libBerryAPI.so` exports exactly these symbols (C, `extern "C"`):

```c
bool        BERRY_Inject();
bool        BERRY_InjectWithConfig(const BerryConfigC* cfg);
void        BERRY_Detach();
bool        BERRY_IsAttached();
const char* BERRY_Version();
BERRY_PatchedFn BERRY_ShimPatchInPlace(void* target, void* detour);
int         BERRY_ShimUnpatch(void* target);
bool        BERRY_ShimCanPatch();
```

`BerryConfigC` is a **plain C struct** (see `include/berry/config.h`) that starts
with `uint32_t size; uint32_t abi_version;`. The library validates both and
rejects mismatches at attach time (`BERRY_ABI_VERSION` in `version.h`).

Everything else is compiled with `-fvisibility=hidden`.

## License

See [LICENSE](LICENSE).