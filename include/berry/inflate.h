#pragma once
// A small, dependency-free DEFLATE / zlib / gzip decoder (improvement 34).
//
// HTTP responses may be gzip- or deflate-encoded. Rather than vendor zlib, this
// is a compact RFC1951 inflate with RFC1950 (zlib) and RFC1952 (gzip) framing,
// bounded by a decompressed-size cap so a zip bomb cannot exhaust memory.
//
// It never trusts the framing: a malformed stream returns false and writes
// nothing past the cap.

#include "berry/export.h"
#include "berry/types.h"

#include <string>

namespace Berry {

/// The maximum decompressed size accepted, so a small compressed body cannot
/// expand without bound.
constexpr size_t kInflateMaxOutput = 64u * 1024 * 1024;

/// Inflates a raw DEFLATE stream (RFC1951). Returns false on a malformed
/// stream or when the output would exceed `maxOut`.
BERRY_API bool InflateRaw(const u8* data, size_t len, std::string* out,
                          size_t maxOut = kInflateMaxOutput);

/// Inflates a zlib stream (RFC1950: 2-byte header, optional preset dictionary,
/// Adler-32 trailer) or a gzip stream (RFC1952: 10-byte header, optional
/// fields, CRC-32 + ISIZE trailer), detected from the magic bytes. Returns
/// false on a malformed stream or over the cap.
BERRY_API bool InflateAuto(const u8* data, size_t len, std::string* out,
                           size_t maxOut = kInflateMaxOutput);

}  // namespace Berry
