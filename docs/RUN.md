# berry-run — the developer CLI

`berry-run` is the host-side command-line front end for the scripting layer. It
is a plain host binary (no game, no Android, no NDK) built from
`tools/berry-run.cpp` and linked against the portable core:

```sh
make berry-run
./build/berry-run --help
```

It has three working modes plus a `version` mode. Every mode runs the same code
paths the device does — the real client bindings, the real verified loader, the
real bounded replay reader — so a result here means the same thing on a phone.

## `berry-run test <file-or-dir>`

Runs a script's tests through `ScriptTestRunner` against the *real* client
bindings (the IDL surface plus the async layer). A test that calls a binding
exercises that binding; a binding whose service is not bound reports
"service unavailable" exactly as it would in the game.

```sh
berry-run test ./scripts/mymod/main.luau
berry-run test ./scripts/mymod                 # every *.luau under the directory
berry-run test ./scripts --filter inventory    # only names containing "inventory"
berry-run test ./scripts --max 50              # cap how many run
berry-run test ./scripts --list                # discover and print names, run none
berry-run test ./scripts --ref-vm              # use the reference VM, not Luau
berry-run test ./scripts --no-fixtures         # skip on_load/before_each/after_each
```

Exit code is 0 only when every test passes. An `xfail_` that fails counts as a
pass; an `xfail_` that unexpectedly passes counts as a failure, because the
marker is now stale.

### Test conventions

Discovery finds these exports, at the top level or under a `tests` table:

| Name | Meaning |
|---|---|
| `test_*` | a normal test; passes only if its body completes without a failed assertion |
| `xfail_*` | an expected failure; a failure is a pass, a pass is a failure |
| `skip_*` | skipped; the body never runs |

The `tests` namespace is the only one searched, so a module's helpers are not
mistaken for tests. Both VMs are supported: the reference VM exposes flat
exports, Luau exposes a `tests` table.

Lifecycle hooks run around each test:

| Hook | When |
|---|---|
| `on_load` | once per test, right after the module loads |
| `before_each` | before each test body |
| `after_each` | after each test body, only while the host is still loaded |

Each test gets a fresh host, so a fault in one test cannot take the rest of the
suite down.

### Assertions

The runner installs these bindings (no capability required — this is developer
tooling):

| Binding | Fails when |
|---|---|
| `test.assert(cond, msg?)` | `cond` is falsy |
| `test.truthy(v)` | `v` is falsy |
| `test.falsy(v)` | `v` is truthy |
| `test.not_nil(v)` | `v` is nil |
| `test.eq(a, b, msg?)` | `a ~= b` |
| `test.ne(a, b, msg?)` | `a == b` |
| `test.near(a, b, eps, msg?)` | `abs(a - b) > eps` |
| `test.fail(msg?)` | always |

`test.truthy`/`test.falsy` are so named because `true`/`false` are Luau
keywords.

### A test that awaits fails

A test that calls `async.wait` (or another await) suspends its VM. The runner
drives no scheduler, so a still-suspended test is reported as a failure rather
than as a pass — a test that has not finished is not a test that succeeded.

## `berry-run run <script-dir>`

Loads a script directory through the one verified path (`ScriptLoader`) and
drives it headlessly for a number of ticks and frames. This is the "does my
script load and tick" loop, with the loader's refusal reason printed verbatim
when it does not.

```sh
berry-run run ./scripts/mymod --anchor <hex-ed25519-public-key>   # production
berry-run run ./scripts/mymod --dev                               # skip verification
berry-run run ./scripts/mymod --anchor <key> --ticks 100 --frames 60 --verbose
```

Outside `--dev`, the loader requires a verified manifest signature, so an anchor
key is required; the CLI says so up front rather than letting the loader refuse
with a less actionable reason. The anchor is the same Ed25519 public key
`berry-sign.py keygen` prints; `berry-sign.py sign` writes the matching
signature into `manifest.json`.

On success it prints the manifest identity, the entry file and the effective
capability set, then drives `Engine::OnGameTick` and `Engine::OnFrame` the way
the real engine does. A script that faults during the run is reported and exits
non-zero.

## `berry-run replay <file>`

Reads a replay container through the same bounded reader the client uses and
prints its structure. A malformed file is refused rather than parsed
optimistically.

```sh
berry-run replay ./captures/fight.replay
berry-run replay ./captures/fight.replay --expect-build 4242 --expect-proto 748
berry-run replay ./captures/fight.replay --expect-label 1.21.0.3
```

Output is the fingerprint, the section count, the tick span, the decoded payload
size, the keyframe index size, and a record histogram by kind — so a capture
that only carried entity state (and no chunk/block packets) is visible at a
glance.

Giving any `--expect-*` option turns on the fingerprint check, so a replay
recorded against another build/protocol is refused with `replay build mismatch`.

## `berry-run version`

Prints the library version, the C ABI version, the script API version, the
replay format version and the signature-pack format version. Useful for
confirming which build a bug report is against.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | success (all tests passed / script loaded and ran / replay read) |
| 1 | a test failed, a script was refused or faulted, or a replay was refused |
| 2 | a usage or configuration error (bad flag, missing anchor, missing path) |

## Building

`make berry-run` builds `build/berry-run`. It links the shipping (client-only)
mbedTLS objects, so it is a host binary with no Android dependency. `make
test-sanitize` covers the framework it drives; the CLI itself is thin argument
parsing over tested units.
