# Script API versioning and deprecation policy

The script surface (`api/berry.idl` → `include/berry/generated_bindings.h`) is a
contract with every script a user has installed. Scripts outlive library
releases, so the surface needs a version and a policy for retiring parts of it
without breaking a script that still calls them.

There are three separate versions in this project. Do not conflate them:

| Version | Where | What it describes | Bumped when |
| --- | --- | --- | --- |
| `BERRY_VERSION_MAJOR/MINOR/PATCH` | `include/berry/version.h` | The library release | Any release |
| `BERRY_ABI_VERSION` | `include/berry/version.h` | The layout of the exported C config structs (`BerryConfigC`, …) | The C structs change |
| **script API version** | `api/berry.idl` `api_version` | The set of bindings, their names, arities and capabilities | The script surface changes |

Only the third one is what a *script* cares about, and only the third one is
what this document is about.

## The version

`api/berry.idl` opens with a single declaration:

```
api_version 1.0.0
```

`tools/idlgen.py` reads it and emits `Berry::api::ScriptApiVersion()`. That is
the one place the build learns its own script API version; nothing hard-codes it
twice. `make idl-check` fails the build when the IDL and the generated header
drift, so the two can never disagree.

The version follows SemVer:

- **major** — a breaking change to the surface. Removing or renaming a binding,
  or changing its arity, is a major bump.
- **minor** — a backward-compatible addition. A new binding, or a new module.
- **patch** — a documentation or capability-tag fix that does not change what a
  script can call.

## A script's requirement

A manifest may declare the minimum script API it needs:

```json
{
  "name": "cps-counter",
  "version": "1.2.0",
  "entry": "main.luau",
  "api_version": "1.1.0",
  "capabilities": ["render", "hud", "input"]
}
```

The field is optional. A manifest with no `api_version` is the pre-versioning
behaviour: it loads on any build. That is deliberate — it keeps every
already-published script working, and their existing signatures valid.

When present, the requirement is enforced at load:

- The loader compares it against `ScriptManifest::BuildApiVersion()` with the
  ordinary SemVer total order. A requirement `<=` the build's version is
  satisfied.
- A requirement newer than the build is **refused** with
  `LoadError::ApiVersionUnsupported` and a message naming both versions. The
  script never loads, so a call to a binding that does not exist cannot fault
  mid-game.
- A malformed requirement (`"1.2"`, a number, an empty string) is refused as
  `ManifestError::BadApiVersion`. A typo fails closed rather than reading as "no
  requirement".

The check runs in `VerifyManifestAndSource`, the one path both `Load` and
`VerifyOnly` (and the per-poll re-verification in the reloader) go through, so
it cannot be bypassed by a caller that skips the loader's front door.

### The requirement is signed

`api_version` is part of `ScriptManifest::CanonicalPayload()`, so it is covered
by the signature exactly like the capability list and the source hash. A script
cannot raise its requirement to make an older client refuse it, and cannot lower
it to slip past a check — either edit changes the payload and invalidates the
signature.

The line is emitted **only when the manifest declares a requirement**, and it is
appended last. A manifest written before `api_version` existed keeps the exact
bytes it was signed with, so an already-published signature still verifies. The
Python signer (`tools/berry-sign.py`) mirrors this rule, and
`tools/check-signer.py` pins the interop vector.

## Deprecation

A binding is retired in stages, never removed in one step.

### 1. Mark it deprecated

Add a `deprecated` line above the `fn` in `api/berry.idl`, with the migration
advice a script author needs:

```
  deprecated "Use `world.block_at`, which returns the same numeric id. ..."
  fn block_id_at(x: number, y: number, z: number) -> number
```

`make idl` regenerates the C++ table (a `deprecated` field on `BindingSpec`),
the TypeScript definitions (`@deprecated` JSDoc), the Luau definitions
(`-- @deprecated`), and `docs/API.md` (a "Deprecated." note in the table).
Deprecation is part of the IDL, so all four surfaces say the same thing.

### 2. It keeps working

A deprecated binding still resolves, still enforces its arity, and still gates
on its capability. Deprecation is a notice, never a security downgrade: the test
suite asserts every deprecated binding is still a grantable, gated binding.

The runtime warns **once per script per binding**, from inside
`ScriptBindings::Invoke` — the single choke point every native call passes
through (the VM's `DoNative`, `ScriptHost::CallBinding`, and the script bus all
end up there). The warning names the script and the binding and repeats the
advice. It is emitted only for a call that got past the capability gate, so a
denied call does not produce a misleading notice. The "seen" set lives on the
`ScriptContext`, which lives as long as the script, so a hot reload starts a
fresh set and a warning is always attributable to the script that made the call.

### 3. Remove it at the next major bump

A deprecated binding is removed only when the script API major version is
bumped. Removal is a breaking change by definition, and a major bump is what
tells a script author "re-read the migration notes".

The removal procedure is:

1. Bump `api_version` in `api/berry.idl` to the next major.
2. Delete the binding's `fn` line (and its `deprecated` line).
3. Run `make idl` and `make test`.
4. Note the removal in the release notes.

Because every manifest that declared a requirement on the previous major is
refused at load with `ApiVersionUnsupported`, a user sees a clear reason instead
of a binding that faults at run time.

## Checklist for a surface change

| Change | Version bump | Notes |
| --- | --- | --- |
| Add a binding | minor | Add the `fn`; `make idl`. |
| Add a capability | minor | Add the kind to `CapKind` **and** `CAPABILITY_ENUM` in `idlgen.py`; a manifest may now request it. |
| Deprecate a binding | patch | Add the `deprecated` line; keep the `fn`. |
| Change a binding's capability | minor | A manifest that already grants the old capability may need the new one; treat as a review-worthy change. |
| Remove or rename a binding | major | See the removal procedure above. |
| Change a binding's arity | major | A script that calls it with the old argument count now fails closed. |

`tests/test_idl.cpp` pins that the generated table is well formed, that the
script API version is a released SemVer, and that the deprecation table is
consistent (every marker has non-empty advice; a marked binding is still
gated). `tests/test_manifest.cpp` and `tests/test_loader.cpp` pin the payload
and the gate.
