# Luau (vendored)

Upstream: https://github.com/luau-lang/luau
Version: `0.643` (tag `0.643`, commit `c54f558b4d5748ab0658610b8ce0c432053e41eb`)
License: MIT (`LICENSE.txt`; the Lua 5.x-derived parts are under `lua_LICENSE.txt`)

## Why Luau

`api/berry.idl` already generates `.luau` type definitions, so Luau is the
language the scripting surface is described in. It also gives us, out of the
box, the three things the runtime layer needs:

* a sandbox mode (`luaL_sandbox`, `luaL_sandboxthread`, `lua_setsafeenv`),
* an interrupt callback (`lua_callbacks(L)->interrupt`) for instruction and
  time budgets,
* native coroutines, which the async layer maps onto.

## What is vendored

Only what the embedder needs, not the CLI or the test runner:

```
Common/include   headers only (no .cpp)
Ast/src          lexer + parser
Compiler/src     bytecode compiler
VM/src           the VM, libraries and host API
```

`CLI/`, `CodeGen/`, `Analysis/`, `EqSat/`, `bench/`, `fuzz/`, `tests/` and
`tools/` are deliberately excluded: the CLI carries `require`, `os`-level file
loading and a REPL, none of which may be reachable from a sandboxed script.

## How it is built

`tools/sources.mk` lists the Luau units in `LUAU_SRC`/`LUAU_AST_SRC`, so the
host `Makefile` and `tools/build-android.sh` compile the same set. Luau is
compiled with `-w` because upstream is warning-clean only under its own
`-Wall` set; our own code stays warning-clean under `-Wall -Wextra`.

Nothing in this directory is modified from upstream. Local changes belong in
`core/script/luau_vm.cpp`, not here, so a version bump is a clean re-copy.
