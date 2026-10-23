# LZ4 (vendored)

Source: https://github.com/lz4/lz4
Release: v1.10.0 (tarball `v1.10.0.tar.gz`)
License: BSD 2-Clause (the `lib/` directory; see `LICENSE` in this directory).

Only the block compressor used by the replay file format is vendored:

- `lib/lz4.c`, `lib/lz4.h` — the block format (`LZ4_compress_default`,
  `LZ4_decompress_safe`).
- `lib/lz4hc.c`, `lib/lz4hc.h` — the high-compression encoder used for the
  keyframe blocks, where ratio matters more than encode speed.
- `LICENSE`.

The upstream `programs/`, `tests/` and `examples/` trees are GPL-2.0-or-later
and are deliberately **not** vendored; only `lib/` (BSD-2-Clause) is included,
which is the license upstream intends for embedding in third-party
applications.

The frame format (`lib/lz4frame.c`) is **not** vendored. The replay container
owns its own framing (block sizes, offsets, a seek index), so it uses the
block API directly and needs no LZ4 frame header, checksum or dictionary
handling of its own. Every decompress call is `LZ4_decompress_safe`, which
refuses to write past the caller's buffer, so a corrupt length in an untrusted
replay file cannot overflow a read.
