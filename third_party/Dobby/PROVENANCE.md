# Dobby — vendored source

This directory is a vendored copy of [jmpews/Dobby](https://github.com/jmpews/Dobby),
the inline-hook engine the on-device (Android) build uses. It is compiled from
source by `tools/build-android.sh` when `BERRY_DOBBY=1` (the default), so the
injected library carries a matching hook backend without a prebuilt `.so`.

## Baseline

- Upstream branch: `dev` (the maintained line; `master` predates the Android
  fixes below and does not build for `aarch64-linux-android`).
- Files are otherwise unmodified from upstream. The two local changes are
  marked `BERRY` in the source so they can be reapplied on a future bump.

## Local changes (why this is not a verbatim copy)

1. `common/os_arch_features.h` — upstream's `android::make_memory_readable()`
   calls `OSMemory::SetPermission`, which is declared in a header this file
   cannot include without a cycle (`OSMemory`/`kReadExecute` end up
   undeclared). Replaced with a direct `mprotect(..., PROT_READ | PROT_EXEC)`
   over the containing page, plus the `<unistd.h>`/`<sys/mman.h>`/`<stdint.h>`
   includes it needs. Same effect, no include cycle.

2. `source/TrampolineBridge/ClosureTrampolineBridge/arm64/closure_bridge_arm64.asm`
   — the arm64 closure bridge loaded the handler with a direct `adrp`/`add`
   pair. That relocation (`R_AARCH64_ADR_PREL_PG_HI21` against a default-
   visibility symbol) is illegal in a shared object, so the link failed with
   "recompile with -fPIC". The ELF path now loads the address through the GOT
   (`adrp :got:` / `ldr :got_lo12:`), which is position-independent. The Apple
   path is unchanged.

Both changes are under an architecture/`__APPLE__` guard, so they do not affect
the host (x86-64) build, which never compiles this vendored tree.

## Bumping

Re-diff against upstream `dev`, keep the two changes above, and confirm
`tools/build-android.sh` still produces `build-android/<abi>/libBerryAPI.so`.
