# BerryAPI script examples

Each `*.luau` file here is a **runnable** example: it is also a `berry-run test`
suite, so `make check-examples` executes every snippet in CI. That keeps the
documentation honest — an example that stops compiling or starts failing fails
the build.

The examples are intentionally small and self-contained. A test named
`test_<name>` (or `tests.<name>`) is discovered and run; the assertion surface
is `test.eq`, `test.near`, `test.truthy`, `test.falsy`, `test.not_nil`,
`test.fail`.

Run them yourself:

```sh
make check-examples
# or a single one:
./build/berry-run test examples/hitmarker.luau
```
