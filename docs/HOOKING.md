# Hooking

BerryAPI hooks game code through a pluggable backend behind a single interface,
`IHook`. Modules use the typed `Hook<FnPtr>` RAII wrapper or `VTablePatch`.

## Backends

| Backend | Platform | Impl |
|---|---|---|
| `BerryShim` | x86-64 host | `core/hooks/backend_shim.cpp` — patches 5-byte relative jumps, keeps trampolines, used by all tests |
| `Dobby` | Android arm64 | `core/hooks/backend_dobby.cpp` — wraps `DobbyHook`/`DobbyDestroy` behind `IHook` |

Selection is `HookManager::Backend()`. On the host it returns the shim backend;
on Android, compile with `-DBERRY_HOOK_ENGINE_DOBBY` and call
`Hooks().UseDobby()` (declared when `BERRY_HOOK_HAVE_DOBBY`).

```cpp
Hook<MyFn> h(&Hooks().Backend(), target, detour);
h.original()(args...);            // call the real function
```

`HookSymbol` carries `target`, `detour`, and `trampoline` (relocated original
bytes). `Hook::original()` returns the trampoline when present, else the
target.

## VTable patching

`VTablePatch(obj, index, replacement)` replaces one virtual slot. Useful for
intercepting C++ virtual calls (e.g. entity update methods).

```cpp
VTablePatch p(entityObj, 3, &MyOverride);
if (p.Apply()) { /* … */ }
p.Revert();
```

## Shim backend details

`BERRY_ShimPatchInPlace(target, detour)` overwrites the first 5 bytes of
`target` with a `jmp rel32` to `detour`, relocating the original bytes to a
trampoline. `BERRY_ShimUnpatch(target)` restores them. Only for host tests —
never used on device.

## Dobby backend details

`backend_dobby.cpp` is compiled only when `BERRY_HOOK_HAVE_DOBBY` is set
(Android + `BERRY_HOOK_ENGINE_DOBBY`). It:

- calls `DobbyHook(target, detour, &tramp)` for `Install`,
- `DobbyDestroy(target)` for `Uninstall`,
- tracks active targets in a local map for idempotent `IsHooked`.

To build on Android, the build script links the vendored Dobby CMake product
(`libdobby.so`) and adds `-DBERRY_HOOK_ENGINE_DOBBY`. `eglSwapBuffers` can be
hooked this way to drive the render overlay per frame.

## Tracking

`HookManager::Track / Untrack` records all live hooks; `ActiveCount()` is used
by the detach path to verify cleanup.