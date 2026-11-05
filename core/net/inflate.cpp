// A compact DEFLATE/zlib/gzip inflater. See include/berry/inflate.h.
#include "berry/inflate.h"

#include <cstring>

namespace Berry {
namespace {

/// A bit reader over the compressed stream.
struct Bits {
  const u8* p;
  size_t n;
  size_t pos = 0;    // bit position
  bool bad = false;

  bool Need(size_t bits) {
    if (bad) return false;
    if (pos + bits > n * 8) { bad = true; return false; }
    return true;
  }
  u32 Get(size_t bits) {
    if (!Need(bits)) return 0;
    u32 v = 0;
    for (size_t i = 0; i < bits; ++i) {
      const size_t bit = pos + i;
      v |= (u32)((p[bit >> 3] >> (bit & 7)) & 1u) << i;
    }
    pos += bits;
    return v;
  }
  void AlignByte() { pos = (pos + 7) & ~(size_t)7; }
  bool AtEnd() const { return pos >= n * 8; }
};

/// A canonical Huffman decoder built from a code-length list.
struct Huff {
  // Count of codes per length (1..15) and the sorted symbols.
  u16 count[16] = {};
  std::vector<u16> symbol;

  bool Build(const u8* lengths, size_t num) {
    for (int i = 0; i < 16; ++i) count[i] = 0;
    for (size_t i = 0; i < num; ++i) ++count[lengths[i]];
    if (count[0] == num) return true;   // no codes: legal, decodes nothing
    count[0] = 0;
    u16 left = 1;
    for (int len = 1; len <= 15; ++len) {
      left = (u16)(left << 1);
      if (count[len] > left) return false;   // over-subscribed
      left = (u16)(left - count[len]);
    }
    // Offsets of each length in `symbol`. The array is 17 long because the
    // final iteration reads/writes offs[16] (len+1 for len == 15).
    u16 offs[17];
    offs[0] = 0;
    offs[1] = 0;
    for (int len = 1; len <= 15; ++len) offs[len + 1] = (u16)(offs[len] + count[len]);
    symbol.assign(num, 0);
    for (size_t i = 0; i < num; ++i) {
      if (lengths[i]) symbol[offs[lengths[i]]++] = (u16)i;
    }
    return true;
  }

  /// Decodes one symbol, or -1 on a bad stream.
  int Decode(Bits* b) const {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; ++len) {
      if (!b->Need(1)) return -1;
      code |= (int)b->Get(1);
      const int c = count[len];
      if (code - first < c) return symbol[index + (code - first)];
      index += c;
      first = (first + c) << 1;
      code <<= 1;
    }
    return -1;
  }
};

// Length and distance base/extra tables (RFC1951 §3.2.5).
const u16 kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                          67,83,99,115,131,163,195,227,258};
const u8 kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const u16 kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,
                           513,769,1025,1537,2049,3073,4097,6145,8193,12289,
                           16385,24577};
const u8 kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,
                           11,11,12,12,13,13};

bool InflateBlockData(Bits* b, const Huff& lit, const Huff& dist,
                      std::string* out, size_t maxOut) {
  for (;;) {
    const int sym = lit.Decode(b);
    if (sym < 0) return false;
    if (sym < 256) {
      if (out->size() >= maxOut) return false;
      out->push_back((char)sym);
      continue;
    }
    if (sym == 256) return true;   // end of block
    const int li = sym - 257;
    if (li >= 29) return false;
    if (!b->Need(kLenExtra[li])) return false;
    const size_t length = kLenBase[li] + b->Get(kLenExtra[li]);
    const int dsym = dist.Decode(b);
    if (dsym < 0 || dsym >= 30) return false;
    if (!b->Need(kDistExtra[dsym])) return false;
    const size_t distance = kDistBase[dsym] + b->Get(kDistExtra[dsym]);
    if (distance == 0 || distance > out->size()) return false;   // invalid backref
    if (out->size() + length > maxOut) return false;
    for (size_t i = 0; i < length; ++i) {
      out->push_back((*out)[out->size() - distance]);
    }
  }
}

bool InflateFixed(Bits* b, std::string* out, size_t maxOut) {
  u8 litLen[288];
  for (int i = 0; i < 144; ++i) litLen[i] = 8;
  for (int i = 144; i < 256; ++i) litLen[i] = 9;
  for (int i = 256; i < 280; ++i) litLen[i] = 7;
  for (int i = 280; i < 288; ++i) litLen[i] = 8;
  Huff lit, dist;
  if (!lit.Build(litLen, 288)) return false;
  u8 dlen[30];
  for (int i = 0; i < 30; ++i) dlen[i] = 5;
  if (!dist.Build(dlen, 30)) return false;
  return InflateBlockData(b, lit, dist, out, maxOut);
}

bool InflateDynamic(Bits* b, std::string* out, size_t maxOut) {
  const u32 hlit = b->Get(5) + 257;
  const u32 hdist = b->Get(5) + 1;
  const u32 hclen = b->Get(4) + 4;
  if (b->bad || hlit > 286 || hdist > 30) return false;
  static const int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
  u8 clLen[19] = {};
  for (u32 i = 0; i < hclen; ++i) clLen[order[i]] = (u8)b->Get(3);
  if (b->bad) return false;
  Huff cl;
  if (!cl.Build(clLen, 19)) return false;

  u8 lengths[288 + 30] = {};
  const u32 total = hlit + hdist;
  for (u32 i = 0; i < total;) {
    const int sym = cl.Decode(b);
    if (sym < 0) return false;
    if (sym < 16) {
      lengths[i++] = (u8)sym;
    } else if (sym == 16) {
      if (i == 0 || !b->Need(2)) return false;
      const u8 prev = lengths[i - 1];
      const u32 rep = 3 + b->Get(2);
      for (u32 k = 0; k < rep && i < total; ++k) lengths[i++] = prev;
    } else if (sym == 17) {
      if (!b->Need(3)) return false;
      const u32 rep = 3 + b->Get(3);
      for (u32 k = 0; k < rep && i < total; ++k) lengths[i++] = 0;
    } else if (sym == 18) {
      if (!b->Need(7)) return false;
      const u32 rep = 11 + b->Get(7);
      for (u32 k = 0; k < rep && i < total; ++k) lengths[i++] = 0;
    } else {
      return false;
    }
  }
  Huff lit, dist;
  if (!lit.Build(lengths, hlit)) return false;
  if (!dist.Build(lengths + hlit, hdist)) return false;
  return InflateBlockData(b, lit, dist, out, maxOut);
}

bool InflateCore(Bits* b, std::string* out, size_t maxOut) {
  bool last = false;
  while (!last) {
    if (!b->Need(1)) return false;
    last = b->Get(1) != 0;
    const u32 type = b->Get(2);
    if (b->bad) return false;
    if (type == 0) {
      // Stored block.
      b->AlignByte();
      if (!b->Need(32)) return false;
      const u32 len = b->Get(16);
      const u32 nlen = b->Get(16);
      if ((len ^ 0xFFFF) != nlen) return false;
      if (!b->Need((size_t)len * 8)) return false;
      if (out->size() + len > maxOut) return false;
      for (u32 i = 0; i < len; ++i) out->push_back((char)b->Get(8));
    } else if (type == 1) {
      if (!InflateFixed(b, out, maxOut)) return false;
    } else if (type == 2) {
      if (!InflateDynamic(b, out, maxOut)) return false;
    } else {
      return false;   // reserved block type
    }
  }
  return true;
}

}  // namespace

bool InflateRaw(const u8* data, size_t len, std::string* out, size_t maxOut) {
  if (!data || !out) return false;
  out->clear();
  Bits b{data, len};
  return InflateCore(&b, out, maxOut);
}

bool InflateAuto(const u8* data, size_t len, std::string* out, size_t maxOut) {
  if (!data || !out || len < 2) return false;
  // gzip: 1f 8b. zlib: the low nibble of the first byte is 8 (deflate) and the
  // 16-bit value is a multiple of 31.
  if (data[0] == 0x1f && data[1] == 0x8b) {
    if (len < 18) return false;   // header + trailer
    const u8 flg = data[3];
    size_t off = 10;
    if (flg & 0x04) {   // FEXTRA
      if (off + 2 > len) return false;
      const u16 xlen = (u16)(data[off] | (data[off + 1] << 8));
      off += 2 + xlen;
    }
    if (flg & 0x08) { while (off < len && data[off]) ++off; ++off; }   // FNAME
    if (flg & 0x10) { while (off < len && data[off]) ++off; ++off; }   // FCOMMENT
    if (flg & 0x02) off += 2;                                          // FHCRC
    if (off > len) return false;
    const size_t bodyLen = len - off - 8;   // minus CRC32 + ISIZE
    if (bodyLen > len) return false;
    return InflateRaw(data + off, bodyLen, out, maxOut);
  }
  // zlib's FCHECK is over the 16-bit big-endian header: (CMF<<8 | FLG) % 31 == 0.
  const u16 hdr = (u16)((data[0] << 8) | data[1]);
  if (((data[0] & 0x0F) == 8) && ((hdr % 31) == 0)) {
    // FDICT is bit 5 of FLG (the second byte), not of CMF. A preset dictionary
    // is not supported, so refuse it rather than mis-decode.
    if ((data[1] & 0x20) != 0) return false;
    if (len < 6) return false;
    const size_t bodyLen = len - 2 - 4;   // minus zlib header + Adler-32
    return InflateRaw(data + 2, bodyLen, out, maxOut);
  }
  // No framing: try raw deflate.
  return InflateRaw(data, len, out, maxOut);
}

}  // namespace Berry
