# ed25519 (vendored)

Source: https://github.com/orlp/ed25519
Commit: b1f19fab4aebe607805620d25a5e42566ce46a0e
License: zlib-style (see LICENSE.txt) — "permission is granted to anyone to
use this software for any purpose ... and to alter and redistribute it
freely".

Local changes:

- `seed.c` is not vendored. Key generation here is deterministic: the caller
  supplies the seed, so nothing reads `/dev/urandom` from inside the injected
  library. Compile with `-DED25519_NO_SEED` so the header matches.
- The library is compiled as C, with warnings on; `sha512.c` is used as the
  hash rather than pulling in another dependency.
- `fe.c` and `sc.c` shift signed carries left (`carry0 << 21`), which is
  undefined behaviour when the carry is negative — and it is negative on every
  borrow, so `make test-sanitize` (UBSan) rejects the code as shipped. Each
  such shift is now `(int64_t)((uint64_t)carry << n)`: the carry is small
  enough that the result fits in `int64_t`, so the value is identical and the
  UB is gone. `ge.c` computes `|b|` as `b - (((-bnegative) & b) << 1)`, which
  shifts a negative value left for every negative table entry; it is now the
  plain `bnegative ? -b : b`. The RFC 8032 vectors in `tests/test_manifest.cpp`
  pin the behaviour after both changes.

Only `ed25519_create_keypair`, `ed25519_sign` and `ed25519_verify` are used
(see `core/script/ed25519.cpp`). `add_scalar.c` and `key_exchange.c` are not
vendored.
