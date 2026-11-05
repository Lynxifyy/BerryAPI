# BerryAPI — project memory

## Build / test
- `make test` rebuilds vendored mbedTLS + Luau from scratch the first time: ~4-6 min on 4 cores. Run it in the background and poll a log file.
- `make test-sanitize` (ASan+UBSan) takes ~9-10 min cold. Both must stay green.
- Every `tests/test_*.cpp` is its own binary `build/test_test_*`; build/run one with `make build/test_test_<name> && ./build/test_test_<name> test`.
- Adding a test file requires a `tools/sources.mk` TEST_SRC entry (`make check-sources` fails otherwise).
- Header dep tracking: the Makefile `-include`s `*.d`; `build/tools/*.d` for berry-run/berry-relay/berry-sigcheck must be listed too, or a header change leaves a stale tool object that silently mismatches the core (fixed in this branch).
- `check-run` (part of `make test`) drives `build/berry-run` end to end.

## Conventions learned
- Git author for this repo's work: `Lynxifyy <297587416+Lynxifyy@users.noreply.github.com>` (repo default was `openhands`).
- PRs: `create_pr` / GitHub REST returns 403; `GH_TOKEN="$Github" gh pr create` works.
- Commits on this branch use no Co-authored-by (user directive).

## Landmines
- The shim hotpatch backend is x86-64 only; a `jmp rel32` patch and its trampoline must be within +/-2 GiB, so the test must keep target, detour and any RIP-referenced global in one mmap'd page.
- `Berry::u8/u16/u32/u64` are in namespace Berry; a .cpp in the global namespace needs explicit `using Berry::u16;` etc.
- `Signature::Parse` now rejects a lone hex digit and supports half-nibble / IDA / \x styles; `wild` holds the per-byte fixed-bit mask.
- `ResolveAdrpAdd(pc, rd, maxInsns)` — always pass the caller's remaining instruction count so the look-ahead cannot read past a buffer.
- `core/net/net.cpp` includes `berry/voice_crypto.h` for `VoiceRandomBytes` (the CSPRNG behind WS keys/masks).
