# Threading model

Written as part of Phase 0.5. This defines **which thread runs what** and the
assertions that keep it that way. Every feature that touches game or render
state must go through `Engine::OnGameTick` / `Engine::OnFrame`, which stamp the
thread role in `ThreadModel`.

## Roles

| Role | Runs | Located in |
|------|------|------------|
| `GameTick` | `Engine::OnGameTick`: scheduler, module `OnTick`, world cache refresh, combat `OnTick` | game simulation thread (20 Hz default in Bedrock) |
| `Render` | `Engine::OnFrame`: event bus `berry:frame`, overlay draw, render backend | render thread — the `eglSwapBuffers` / `vkQueuePresentKHR` hook |
| `Scripts` | script VMs (Phase 1): `on_tick`, `on_frame`, any user code | script worker thread(s); **never** the render thread |
| `Detach` | `Engine::Detach`, loader/unloader tooling | caller thread |
| `Unknown` | pre-assert or detached | any |

## Invariants

1. **Ticks run on the game thread only.** `AssertTick()` guards scheduler +
   module ticks. Wrong thread → logged error, and in debug/test builds
   `__builtin_trap()`.
2. **Render runs on the render thread only.** `AssertRender()` guards
   `OnFrame`. The render thread has a hard per-frame time budget; nothing
   blocking, no allocation spikes, no script execution.
3. **Scripts never run on the render thread.** The script runtime lives on
   its own worker; each script gets an instruction budget per tick and a wall
   clock timeout. `AssertNotRender()` fires if user code is ever invoked on
   the render thread.
4. **Game reads are validated and fail closed.** On a thread-model violation
   the affected feature is disabled (see SIGNATURES.md `SigState::Missing`).

## Assertions

Header: `include/berry/thread.h`, implementation: `core/lifecycle/thread.cpp`.

```cpp
ThreadModel& tm = ThreadModel::Instance();
tm.Enter(ThreadRole::GameTick);   // engine does this around OnGameTick
tm.AssertTick();
tm.Leave();
```

Host/test builds compile with `-DBERRY_THREAD_ASSERT`, which turns violations
into hard traps (they fail the test). Device builds log the violation and keep
the frame alive, then disable the offending feature.

## Scripts

Phase 1 defines the script runtime on a dedicated worker:

- `ScopedScriptTimeslice` bounds one script callback (instruction budget,
  memory cap, wall-clock timeout).
- A crashing script is unloaded; the game keeps running. Error isolation is a
  hard requirement — see the Phase 1 acceptance criteria.