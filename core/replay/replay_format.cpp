// Replay container format. See include/berry/replay_format.h for the design.
//
// Everything here is bounds-checked and little-endian on the wire, so a file
// written on one machine reads on another and a malformed file is refused
// rather than trusted. No read ever allocates from an unchecked length: a
// declared count is compared against a cap before it is used.
#include "berry/replay_format.h"

#include <algorithm>
#include <climits>
#include <cstring>

#include "lz4.h"
#include "lz4hc.h"

namespace Berry {

namespace {

//--- little-endian primitives ------------------------------------------------

void PutU32(std::string* out, u32 v) {
  out->push_back((char)(v & 0xff));
  out->push_back((char)((v >> 8) & 0xff));
  out->push_back((char)((v >> 16) & 0xff));
  out->push_back((char)((v >> 24) & 0xff));
}

void PutU64(std::string* out, u64 v) {
  PutU32(out, (u32)(v & 0xffffffffu));
  PutU32(out, (u32)(v >> 32));
}

/// Reads a little-endian u32 from raw bytes. The caller guarantees 4 bytes.
u32 LoadU32(const char* p) {
  const unsigned char* b = (const unsigned char*)p;
  return (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
}

/// A bounds-checked cursor over a byte string. Every read either succeeds and
/// advances, or fails and leaves the cursor alone, so a caller can parse
/// optimistically and abandon on the first failure.
class Cursor {
 public:
  Cursor(const char* data, size_t size) : data_(data), size_(size) {}
  explicit Cursor(const std::string& s) : data_(s.data()), size_(s.size()) {}

  size_t pos() const { return pos_; }
  size_t remaining() const { return size_ - pos_; }
  bool atEnd() const { return pos_ >= size_; }

  bool ReadU32(u32* out) {
    if (remaining() < 4) return false;
    *out = LoadU32(data_ + pos_);
    pos_ += 4;
    return true;
  }
  bool ReadU64(u64* out) {
    u32 lo = 0, hi = 0;
    if (!ReadU32(&lo) || !ReadU32(&hi)) return false;
    *out = (u64)lo | ((u64)hi << 32);
    return true;
  }
  bool ReadU8(u8* out) {
    if (remaining() < 1) return false;
    *out = (u8)data_[pos_++];
    return true;
  }
  /// Reads a raw byte span. Bounded by what is left, so a declared length past
  /// the end fails instead of reading out of bounds.
  bool ReadBytes(size_t n, std::string* out) {
    if (n > remaining()) return false;
    out->assign(data_ + pos_, n);
    pos_ += n;
    return true;
  }
  /// Seeks to an absolute offset. False when past the end.
  bool Seek(size_t off) {
    if (off > size_) return false;
    pos_ = off;
    return true;
  }

 private:
  const char* data_;
  size_t size_;
  size_t pos_ = 0;
};

//--- CRC-32 (IEEE 802.3), used for the header and each block ----------------
//
// A plain bitwise implementation: it runs on a header and on block payloads,
// never in a hot inner loop, so a lookup table would be complexity without
// benefit. It is an integrity check, not a security primitive.
u32 Crc32(const void* data, size_t len) {
  const unsigned char* p = (const unsigned char*)data;
  u32 crc = 0xffffffffu;
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) {
      const u32 mask = (u32)-(int)(crc & 1u);
      crc = (crc >> 1) ^ (0xedb88320u & mask);
    }
  }
  return crc ^ 0xffffffffu;
}

//--- layout constants -------------------------------------------------------

// Header: magic(4) version(4) buildId(4) protocol(4) labelLen(4) sectionCount(4)
//         indexCount(4) flags(4) blocksOffset(8) indexOffset(8)
//         totalUncompressed(8) crc(4) reserved(4) = 64 bytes.
constexpr size_t kHeaderSize = 64;
constexpr size_t kHeaderCrcOffset = 56;   // crc covers bytes [0, 56)
constexpr size_t kBlockHeaderSize = 48;
constexpr u32 kBlockMagic = 0x424c4b31u;   // "BLK1"
constexpr u32 kBlockFlagCompressed = 1u;

struct Header {
  u32 version = kReplayFormatVersion;
  u32 buildId = 0;
  u32 protocolVersion = 0;
  u32 labelLen = 0;
  u32 sectionCount = 0;
  u32 indexCount = 0;
  u32 flags = 0;
  u64 blocksOffset = 0;
  u64 indexOffset = 0;
  u64 totalUncompressed = 0;
};

void EncodeHeader(const Header& h, std::string* out) {
  std::string body;
  body.reserve(kHeaderSize);
  body.append(kReplayMagic, 4);
  PutU32(&body, h.version);
  PutU32(&body, h.buildId);
  PutU32(&body, h.protocolVersion);
  PutU32(&body, h.labelLen);
  PutU32(&body, h.sectionCount);
  PutU32(&body, h.indexCount);
  PutU32(&body, h.flags);
  PutU64(&body, h.blocksOffset);
  PutU64(&body, h.indexOffset);
  PutU64(&body, h.totalUncompressed);
  // The checksum covers the 56 bytes before it, so an edited or truncated
  // header is detected before any field is used.
  PutU32(&body, Crc32(body.data(), body.size()));
  PutU32(&body, 0);   // reserved
  *out = std::move(body);
}

/// Decodes the header fields. The caller has already verified the CRC.
ReplayError DecodeHeaderFields(Cursor* c, Header* out) {
  std::string m;
  if (!c->ReadBytes(4, &m)) return ReplayError::Truncated;
  if (std::memcmp(m.data(), kReplayMagic, 4) != 0) return ReplayError::BadMagic;
  if (!c->ReadU32(&out->version)) return ReplayError::Truncated;
  if (out->version != kReplayFormatVersion) return ReplayError::BadVersion;
  if (!c->ReadU32(&out->buildId)) return ReplayError::Truncated;
  if (!c->ReadU32(&out->protocolVersion)) return ReplayError::Truncated;
  if (!c->ReadU32(&out->labelLen)) return ReplayError::Truncated;
  if (!c->ReadU32(&out->sectionCount)) return ReplayError::Truncated;
  if (!c->ReadU32(&out->indexCount)) return ReplayError::Truncated;
  if (!c->ReadU32(&out->flags)) return ReplayError::Truncated;
  if (!c->ReadU64(&out->blocksOffset)) return ReplayError::Truncated;
  if (!c->ReadU64(&out->indexOffset)) return ReplayError::Truncated;
  if (!c->ReadU64(&out->totalUncompressed)) return ReplayError::Truncated;
  u32 crc = 0, reserved = 0;
  if (!c->ReadU32(&crc)) return ReplayError::Truncated;
  if (!c->ReadU32(&reserved)) return ReplayError::Truncated;
  return ReplayError::Ok;
}

//--- section encode / decode ------------------------------------------------

/// Encodes one section's records into the pre-compression byte stream.
ReplayError EncodeSection(const ReplaySection& s, const ReplayLimits& limits,
                          std::string* out) {
  if (s.records.size() > limits.maxRecordsPerSection) {
    return ReplayError::TooManyRecords;
  }
  std::string body;
  PutU32(&body, (u32)s.records.size());
  for (const ReplayRecord& r : s.records) {
    if (r.payload.size() > limits.maxPayloadBytes) return ReplayError::TooLarge;
    PutU64(&body, r.tick);
    PutU32(&body, (u32)r.kind);
    PutU32(&body, r.tag);
    body.push_back((char)r.dir);
    PutU32(&body, (u32)r.payload.size());
    body.append(r.payload);
  }
  *out = std::move(body);
  return ReplayError::Ok;
}

/// Decodes one section's records. Every count and length is capped before use.
ReplayError DecodeSection(const std::string& raw, ReplaySectionKind kind,
                          u64 startTick, u64 endTick, const ReplayLimits& limits,
                          ReplaySection* out) {
  Cursor c(raw);
  u32 count = 0;
  if (!c.ReadU32(&count)) return ReplayError::BadBlock;
  if (count > limits.maxRecordsPerSection) return ReplayError::TooManyRecords;
  ReplaySection s;
  s.kind = kind;
  s.startTick = startTick;
  s.endTick = endTick;
  s.records.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    ReplayRecord r;
    u32 kindRaw = 0, plen = 0;
    if (!c.ReadU64(&r.tick)) return ReplayError::BadRecord;
    if (!c.ReadU32(&kindRaw)) return ReplayError::BadRecord;
    if (!c.ReadU32(&r.tag)) return ReplayError::BadRecord;
    if (!c.ReadU8(&r.dir)) return ReplayError::BadRecord;
    if (!c.ReadU32(&plen)) return ReplayError::BadRecord;
    if (plen > limits.maxPayloadBytes) return ReplayError::TooLarge;
    if (!c.ReadBytes(plen, &r.payload)) return ReplayError::Truncated;
    r.kind = (ReplayRecordKind)kindRaw;
    s.records.push_back(std::move(r));
  }
  // A section that carries fewer bytes than it declares is as broken as one
  // that carries more: a trailing byte is a sign the stream is not what the
  // header says.
  if (!c.atEnd()) return ReplayError::BadBlock;
  *out = std::move(s);
  return ReplayError::Ok;
}

/// Compresses `raw` into a block. Returns false only when `raw` is too large to
/// hand to LZ4 (an `int` length), which the caller reports as `TooLarge`.
bool CompressBlock(const std::string& raw, bool highCompression,
                   std::string* stored, bool* compressed) {
  if (raw.size() > (size_t)INT_MAX / 2) return false;
  const int srcLen = (int)raw.size();
  const int bound = LZ4_compressBound(srcLen);
  if (bound <= 0) return false;
  std::string buf;
  buf.resize((size_t)bound);
  const int n = highCompression
                    ? LZ4_compress_HC(raw.data(), buf.data(), srcLen, bound, 9)
                    : LZ4_compress_default(raw.data(), buf.data(), srcLen, bound);
  if (n <= 0 || (size_t)n >= raw.size()) {
    // Not compressible (or the encoder refused): store raw so a block never
    // grows. A zero-length section stores as zero bytes and is flagged raw.
    *stored = raw;
    *compressed = false;
    return true;
  }
  buf.resize((size_t)n);
  *stored = std::move(buf);
  *compressed = true;
  return true;
}

}  // namespace

const char* ReplayErrorName(ReplayError e) {
  switch (e) {
    case ReplayError::Ok: return "ok";
    case ReplayError::BadMagic: return "not a replay file";
    case ReplayError::BadVersion: return "unsupported replay version";
    case ReplayError::Truncated: return "truncated replay file";
    case ReplayError::BadHeader: return "bad replay header";
    case ReplayError::BadFingerprint: return "replay build mismatch";
    case ReplayError::BadIndex: return "bad replay seek index";
    case ReplayError::BadBlock: return "bad replay block";
    case ReplayError::BadRecord: return "bad replay record";
    case ReplayError::TooManySections: return "too many replay sections";
    case ReplayError::TooManyRecords: return "too many records in a section";
    case ReplayError::TooLarge: return "replay value exceeds a cap";
    case ReplayError::Corrupt: return "replay file is corrupt";
    case ReplayError::IoError: return "replay io error";
  }
  return "unknown";
}

const char* ReplayRecordKindName(ReplayRecordKind k) {
  switch (k) {
    case ReplayRecordKind::Unknown: return "unknown";
    case ReplayRecordKind::Packet: return "packet";
    case ReplayRecordKind::Chunk: return "chunk";
    case ReplayRecordKind::EntitySpawn: return "entity_spawn";
    case ReplayRecordKind::EntityMove: return "entity_move";
    case ReplayRecordKind::EntityMetadata: return "entity_metadata";
    case ReplayRecordKind::Equipment: return "equipment";
    case ReplayRecordKind::Skin: return "skin";
    case ReplayRecordKind::TimeWeather: return "time_weather";
    case ReplayRecordKind::PlayerInput: return "player_input";
    case ReplayRecordKind::Event: return "event";
    case ReplayRecordKind::Count: return "count";
  }
  return "unknown";
}

bool ReplayRecordKindFromName(const std::string& name, ReplayRecordKind* out) {
  for (u32 i = 0; i <= (u32)ReplayRecordKind::Event; ++i) {
    const ReplayRecordKind k = (ReplayRecordKind)i;
    if (name == ReplayRecordKindName(k)) { *out = k; return true; }
  }
  return false;
}

//----------------------------------------------------------------------------
// ReplayFile
//----------------------------------------------------------------------------

size_t ReplayFile::payloadBytes() const {
  size_t total = 0;
  for (const ReplaySection& s : sections) {
    for (const ReplayRecord& r : s.records) total += r.payload.size();
  }
  return total;
}

u64 ReplayFile::firstTick() const {
  if (sections.empty()) return 0;
  return sections.front().startTick;
}

u64 ReplayFile::lastTick() const {
  if (sections.empty()) return 0;
  return sections.back().endTick;
}

ReplayError ReplayFile::AddSection(const ReplaySection& section,
                                   const ReplayLimits& limits) {
  if (sections.size() >= limits.maxSections) return ReplayError::TooManySections;
  if (section.records.size() > limits.maxRecordsPerSection) {
    return ReplayError::TooManyRecords;
  }
  // A replay is ordered: a section may not start before the previous one ends,
  // so a seek can assume ticks increase across the file.
  if (!sections.empty() && section.startTick < sections.back().endTick) {
    return ReplayError::BadHeader;
  }
  if (section.endTick < section.startTick) return ReplayError::BadHeader;
  for (const ReplayRecord& r : section.records) {
    if (r.payload.size() > limits.maxPayloadBytes) return ReplayError::TooLarge;
  }
  sections.push_back(section);
  return ReplayError::Ok;
}

void ReplayFile::RebuildIndex() {
  index.clear();
  // One entry per keyframe. A delta is never a seek anchor, because decoding it
  // requires the keyframe it follows.
  for (const ReplaySection& s : sections) {
    if (s.kind != ReplaySectionKind::Keyframe) continue;
    ReplayIndexEntry e;
    e.startTick = s.startTick;
    e.recordCount = (u32)s.records.size();
    e.fileOffset = 0;   // filled in by the writer as it lays out blocks
    index.push_back(e);
  }
}

bool ReplayFile::SeekEntry(u64 tick, ReplayIndexEntry* out) const {
  if (index.empty()) return false;
  // The last keyframe at or before `tick`, else the first (a seek before the
  // first keyframe starts at the beginning).
  const ReplayIndexEntry* best = &index.front();
  for (const ReplayIndexEntry& e : index) {
    if (e.startTick <= tick) best = &e;
    else break;
  }
  *out = *best;
  return true;
}

//----------------------------------------------------------------------------
// ReplayWriter
//----------------------------------------------------------------------------

ReplayError ReplayWriter::Write(const ReplayFile& file, std::string* out,
                                const ReplayLimits& limits) {
  return WriteRing(file, 0, out, limits);
}

ReplayError ReplayWriter::WriteRing(const ReplayFile& file, size_t ringSections,
                                    std::string* out, const ReplayLimits& limits) {
  lastBlockCount_ = 0;
  if (file.sections.size() > limits.maxSections) {
    return ReplayError::TooManySections;
  }
  if (file.fingerprint.label.size() > limits.maxLabelBytes) {
    return ReplayError::TooLarge;
  }

  // The sections to keep: everything, or the trailing `ringSections`. The ring
  // keeps whole sections, so the oldest kept keyframe is still a valid anchor
  // for every section after it.
  size_t first = 0;
  if (ringSections > 0 && file.sections.size() > ringSections) {
    first = file.sections.size() - ringSections;
  }
  const size_t kept = file.sections.size() - first;

  std::string body;
  body.reserve(64 * 1024);

  // Blocks first, so the index can record real offsets.
  std::vector<ReplayIndexEntry> index;
  u64 totalUncompressed = 0;
  for (size_t i = first; i < file.sections.size(); ++i) {
    const ReplaySection& s = file.sections[i];
    std::string raw;
    ReplayError e = EncodeSection(s, limits, &raw);
    if (e != ReplayError::Ok) return e;
    if (raw.size() > limits.maxSectionBytes) return ReplayError::TooLarge;

    std::string stored;
    bool compressed = false;
    if (!CompressBlock(raw, s.kind == ReplaySectionKind::Keyframe, &stored,
                       &compressed)) {
      return ReplayError::TooLarge;
    }

    const u64 blockOffset = (u64)kHeaderSize + (u64)file.fingerprint.label.size() +
                            (u64)body.size();
    if (s.kind == ReplaySectionKind::Keyframe) {
      ReplayIndexEntry ie;
      ie.startTick = s.startTick;
      ie.fileOffset = blockOffset;
      ie.recordCount = (u32)s.records.size();
      index.push_back(ie);
    }

    PutU32(&body, kBlockMagic);
    PutU32(&body, compressed ? kBlockFlagCompressed : 0u);
    PutU32(&body, (u32)raw.size());
    PutU32(&body, (u32)stored.size());
    PutU32(&body, Crc32(stored.data(), stored.size()));
    PutU32(&body, (u32)s.kind);
    PutU32(&body, (u32)s.records.size());
    PutU32(&body, 0);   // reserved
    PutU64(&body, s.startTick);
    PutU64(&body, s.endTick);
    body.append(stored);
    ++lastBlockCount_;
    totalUncompressed += raw.size();
    if (totalUncompressed > limits.maxUncompressedBytes) {
      return ReplayError::TooLarge;
    }
  }

  const u64 blocksOffset = (u64)kHeaderSize + (u64)file.fingerprint.label.size();
  const u64 indexOffset = blocksOffset + (u64)body.size();

  std::string idx;
  idx.reserve(index.size() * 24);
  for (const ReplayIndexEntry& e : index) {
    PutU64(&idx, e.startTick);
    PutU64(&idx, e.fileOffset);
    PutU32(&idx, e.recordCount);
    PutU32(&idx, 0);
  }

  Header h;
  h.version = kReplayFormatVersion;
  h.buildId = file.fingerprint.buildId;
  h.protocolVersion = file.fingerprint.protocolVersion;
  h.labelLen = (u32)file.fingerprint.label.size();
  h.sectionCount = (u32)kept;
  h.indexCount = (u32)index.size();
  h.blocksOffset = blocksOffset;
  h.indexOffset = indexOffset;
  h.totalUncompressed = totalUncompressed;

  std::string header;
  EncodeHeader(h, &header);

  std::string result;
  result.reserve(header.size() + file.fingerprint.label.size() + body.size() +
                 idx.size());
  result += header;
  result += file.fingerprint.label;
  result += body;
  result += idx;
  *out += result;
  return ReplayError::Ok;
}

//----------------------------------------------------------------------------
// ReplayReader
//----------------------------------------------------------------------------

ReplayError ReplayReader::Read(const std::string& data, ReplayFile* out,
                               const ReplayLimits& limits) {
  return Read(data, ReplayFingerprint{}, false, out, limits);
}

ReplayError ReplayReader::Read(const std::string& data,
                               const ReplayFingerprint& expected,
                               bool requireFingerprint, ReplayFile* out,
                               const ReplayLimits& limits) {
  lastBlockCount_ = 0;
  if (data.size() < kHeaderSize) return ReplayError::Truncated;

  // --- header: magic and checksum first, so no field is trusted before it ---
  if (std::memcmp(data.data(), kReplayMagic, 4) != 0) {
    return ReplayError::BadMagic;
  }
  if (Crc32(data.data(), kHeaderCrcOffset) != LoadU32(data.data() + kHeaderCrcOffset)) {
    return ReplayError::Corrupt;
  }

  Cursor c(data);
  Header h;
  ReplayError e = DecodeHeaderFields(&c, &h);
  if (e != ReplayError::Ok) return e;

  if (h.labelLen > limits.maxLabelBytes) return ReplayError::TooLarge;
  if (h.sectionCount > limits.maxSections) return ReplayError::TooManySections;
  if (h.indexCount > limits.maxSections) return ReplayError::TooManySections;
  if (h.totalUncompressed > limits.maxUncompressedBytes) {
    return ReplayError::TooLarge;
  }

  std::string label;
  if (!c.ReadBytes(h.labelLen, &label)) return ReplayError::Truncated;

  ReplayFingerprint fp;
  fp.buildId = h.buildId;
  fp.protocolVersion = h.protocolVersion;
  fp.label = label;
  if (requireFingerprint && fp != expected) return ReplayError::BadFingerprint;

  if (h.blocksOffset != (u64)(kHeaderSize + h.labelLen)) {
    return ReplayError::BadHeader;
  }
  if (h.indexOffset < h.blocksOffset || h.indexOffset > data.size()) {
    return ReplayError::BadHeader;
  }

  ReplayFile file;
  file.fingerprint = fp;

  // --- blocks, read sequentially from blocksOffset to indexOffset ---
  u64 uncompressed = 0;
  size_t pos = (size_t)h.blocksOffset;
  while (pos < (size_t)h.indexOffset) {
    Cursor bc(data.data() + pos, (size_t)h.indexOffset - pos);
    u32 magic32 = 0, flags = 0, rawLen = 0, compLen = 0, crc = 0, kindRaw = 0,
        recordCount = 0, reserved = 0;
    u64 startTick = 0, endTick = 0;
    if (!bc.ReadU32(&magic32) || magic32 != kBlockMagic) {
      return ReplayError::BadBlock;
    }
    if (!bc.ReadU32(&flags)) return ReplayError::Truncated;
    if (!bc.ReadU32(&rawLen)) return ReplayError::Truncated;
    if (!bc.ReadU32(&compLen)) return ReplayError::Truncated;
    if (!bc.ReadU32(&crc)) return ReplayError::Truncated;
    if (!bc.ReadU32(&kindRaw)) return ReplayError::Truncated;
    if (!bc.ReadU32(&recordCount)) return ReplayError::Truncated;
    if (!bc.ReadU32(&reserved)) return ReplayError::Truncated;
    if (!bc.ReadU64(&startTick)) return ReplayError::Truncated;
    if (!bc.ReadU64(&endTick)) return ReplayError::Truncated;

    if (rawLen > limits.maxSectionBytes) return ReplayError::TooLarge;
    if (compLen > limits.maxSectionBytes) return ReplayError::TooLarge;
    if (recordCount > limits.maxRecordsPerSection) {
      return ReplayError::TooManyRecords;
    }
    if (endTick < startTick) return ReplayError::BadBlock;

    std::string stored;
    if (!bc.ReadBytes(compLen, &stored)) return ReplayError::Truncated;
    if (Crc32(stored.data(), stored.size()) != crc) return ReplayError::Corrupt;

    std::string raw;
    const bool compressed = (flags & kBlockFlagCompressed) != 0;
    if (compressed) {
      raw.resize(rawLen);
      if (rawLen > 0) {
        // `LZ4_decompress_safe` refuses to write past the destination, so a
        // corrupt length cannot overflow the buffer; it returns the bytes it
        // wrote, which must equal the declared raw length.
        const int n = LZ4_decompress_safe(stored.data(), raw.data(),
                                          (int)stored.size(), (int)rawLen);
        if (n < 0 || (u32)n != rawLen) return ReplayError::BadBlock;
      }
    } else {
      if (stored.size() != rawLen) return ReplayError::BadBlock;
      raw = std::move(stored);
    }
    uncompressed += raw.size();
    if (uncompressed > limits.maxUncompressedBytes) return ReplayError::TooLarge;

    ReplaySection s;
    e = DecodeSection(raw, (ReplaySectionKind)kindRaw, startTick, endTick,
                      limits, &s);
    if (e != ReplayError::Ok) return e;
    file.sections.push_back(std::move(s));
    ++lastBlockCount_;
    pos = (size_t)h.indexOffset - bc.remaining();
  }

  if (file.sections.size() != (size_t)h.sectionCount) {
    return ReplayError::BadHeader;
  }

  // --- index, validated against the blocks it points at ---
  Cursor ic(data);
  if (!ic.Seek((size_t)h.indexOffset)) return ReplayError::BadIndex;
  std::vector<ReplayIndexEntry> index;
  index.reserve(h.indexCount);
  for (u32 i = 0; i < h.indexCount; ++i) {
    ReplayIndexEntry ie;
    u32 recCount = 0, resv = 0;
    if (!ic.ReadU64(&ie.startTick)) return ReplayError::BadIndex;
    if (!ic.ReadU64(&ie.fileOffset)) return ReplayError::BadIndex;
    if (!ic.ReadU32(&recCount)) return ReplayError::BadIndex;
    if (!ic.ReadU32(&resv)) return ReplayError::BadIndex;
    ie.recordCount = recCount;
    if (ie.fileOffset < h.blocksOffset || ie.fileOffset >= h.indexOffset) {
      return ReplayError::BadIndex;
    }
    index.push_back(ie);
  }
  // Every entry must point at a keyframe whose start tick it names, so a
  // hand-edited index cannot make a seek land in the middle of a delta.
  size_t keyframes = 0;
  for (const ReplaySection& s : file.sections) {
    if (s.kind == ReplaySectionKind::Keyframe) ++keyframes;
  }
  if (index.size() != keyframes) return ReplayError::BadIndex;
  {
    size_t k = 0;
    for (const ReplaySection& s : file.sections) {
      if (s.kind != ReplaySectionKind::Keyframe) continue;
      if (index[k].startTick != s.startTick) return ReplayError::BadIndex;
      ++k;
    }
  }
  file.index = std::move(index);

  *out = std::move(file);
  return ReplayError::Ok;
}

//----------------------------------------------------------------------------
// ReplayRingBuffer
//----------------------------------------------------------------------------

ReplayRingBuffer::ReplayRingBuffer(size_t ringSections, const ReplayLimits& limits)
    : limits_(limits), ringSections_(ringSections) {}

ReplayError ReplayRingBuffer::Push(const ReplaySection& section) {
  // With a ring, the section cap is the ring size: the buffer holds only the
  // trailing window, so a long session never grows past it.
  ReplayLimits eff = limits_;
  if (ringSections_ > 0) {
    eff.maxSections = std::max<size_t>(ringSections_ + 1, 2);
  }
  ReplayError e = file_.AddSection(section, eff);
  if (e != ReplayError::Ok) return e;
  if (ringSections_ > 0) {
    while (file_.sections.size() > ringSections_) {
      file_.sections.erase(file_.sections.begin());
      ++evicted_;
    }
  }
  return ReplayError::Ok;
}

void ReplayRingBuffer::Clear() {
  file_.Clear();
  evicted_ = 0;
}

}  // namespace Berry
