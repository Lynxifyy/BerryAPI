#pragma once
// Replay container: a versioned, compressed, seekable file of the client's own
// world stream.
//
// `record.h` holds an in-memory tape and serialises it as one JSON document.
// That is the right shape for a bug report and a test fixture, but it is the
// wrong shape for a replay: a JSON tape of a few minutes is tens of megabytes,
// it must be parsed from the front, and it has no index, so seeking to
// 03:12 means decoding everything before it. This is the on-disk format.
//
// Design:
//
//   * **A fixed binary header** with a magic, a format version, and the build
//     fingerprint the recording was made against. A replay is only meaningful
//     for the build whose packet parsers it feeds, so the fingerprint is
//     checked on load and a mismatch is refused (like a signature pack), not
//     played back against the wrong build.
//   * **LZ4 block compression**, one compressed block per keyframe or delta
//     section. LZ4 is chosen because it decompresses at gigabytes per second,
//     which is what seeking needs: a seek decodes one keyframe, not the file.
//   * **A seek index**: for each keyframe, the byte offset of its block and the
//     tick it starts at. Seeking finds the last keyframe at or before a tick
//     and starts there, so the cost of a seek is one keyframe, whatever the
//     replay's length.
//   * **Keyframes and deltas**, exactly the record layer's model: a keyframe is
//     a full state sample, a delta is what changed since the previous section.
//     A replay therefore needs no keyframe to *store* a tick, only to *seek*
//     to one.
//   * **Bounds on everything.** A replay is untrusted input: it is a file
//     someone else can hand you, and it feeds the game's packet parsers. Every
//     count, offset and length is checked against a cap before it is used to
//     allocate or seek, and every read is bounds-checked. A malformed file is
//     refused wholesale, never half-loaded.
//
// The container stores records, not engine state. Writing a record never
// touches game memory; reading one produces data, never a pointer. What a
// record *means* (a packet, a chunk, an entity spawn) is the record's own kind;
// the container is deliberately agnostic, so a new record kind does not change
// the file format.
//
// Client-side only: a replay is what this client saw. Nothing here can send a
// byte anywhere — the container is a file, and the loopback server that plays
// one back is a separate, local-only object (`replay_server.h`).

#include "berry/export.h"
#include "berry/types.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace Berry {

/// The magic at the front of a replay file: "BRPL" (Berry ReP Lay).
inline constexpr char kReplayMagic[4] = {'B', 'R', 'P', 'L'};

/// The replay container format version. A file with a different version is
/// refused rather than guessed at.
inline constexpr u32 kReplayFormatVersion = 1;

/// What a section is: a full state sample (a keyframe, the anchor a seek starts
/// from) or the change since the previous section (a delta).
enum class ReplaySectionKind : u32 {
  Keyframe = 0,
  Delta = 1,
};

/// Why a replay file was refused. Every one of these leaves the caller's replay
/// untouched; a load either succeeds completely or changes nothing.
enum class ReplayError : u32 {
  Ok = 0,
  BadMagic,          // not a replay file
  BadVersion,        // a format version this build does not know
  Truncated,         // a read ran past the end of the file
  BadHeader,         // a header field is out of range (count, size, tick)
  BadFingerprint,    // the recording's build does not match the active build
  BadIndex,          // the seek index is inconsistent (offset past the file)
  BadBlock,          // a block does not decompress to its declared size
  BadRecord,         // a record inside a section is malformed
  TooManySections,   // more sections than a replay may carry
  TooManyRecords,    // more records in a section than it may carry
  TooLarge,          // a declared size exceeds a cap
  Corrupt,           // the file's own checksum does not match
  IoError,           // the sink could not be written / the source read
};

const char* ReplayErrorName(ReplayError e);

/// A build fingerprint: the identity of the client build a replay was recorded
/// against. Two recordings are compatible only when these match, because the
/// packet ids and struct layouts they carry are a property of the build.
struct BERRY_API ReplayFingerprint {
  u32 buildId = 0;        // the build's numeric id, 0 = unknown
  std::string label;      // a human label, e.g. "1.21.0.3", bounded length
  u32 protocolVersion = 0;  // the network protocol version

  bool operator==(const ReplayFingerprint& o) const {
    return buildId == o.buildId && protocolVersion == o.protocolVersion &&
           label == o.label;
  }
  bool operator!=(const ReplayFingerprint& o) const { return !(*this == o); }
  bool known() const { return buildId != 0 || !label.empty(); }
};

/// What kind of payload a record carries. The container does not interpret the
/// bytes; it only stores the kind so a consumer can route a record. Kinds are
/// additive: a new one does not change the file format, and an unknown kind is
/// preserved on load rather than dropped, so an older reader can copy a newer
/// file without losing records it does not understand.
enum class ReplayRecordKind : u32 {
  Unknown = 0,
  Packet = 1,        // a raw packet: the id and its payload bytes
  Chunk = 2,         // chunk / sub-chunk data or a block update
  EntitySpawn = 3,   // an entity appeared
  EntityMove = 4,    // an entity moved
  EntityMetadata = 5,// an entity's metadata changed
  Equipment = 6,     // an entity's equipment changed
  Skin = 7,          // a player's skin / appearance
  TimeWeather = 8,   // level time or weather
  PlayerInput = 9,   // the local player's own input for a tick
  Event = 10,        // a Berry script/combat event (a timeline marker)
  EntityLeave = 11,  // an entity left the client's view
  Count,
};

const char* ReplayRecordKindName(ReplayRecordKind k);
/// Parses a kind name ("packet", "chunk", …). False for an unknown name.
bool ReplayRecordKindFromName(const std::string& name, ReplayRecordKind* out);

/// One record: a tick, a kind, a direction/tag, and an opaque payload. The
/// payload is bytes, never a pointer, so a record is portable across builds and
/// carries nothing that could be dereferenced on playback.
struct BERRY_API ReplayRecord {
  u64 tick = 0;
  ReplayRecordKind kind = ReplayRecordKind::Unknown;
  /// A kind-specific tag: for a packet, the protocol id; for a chunk, the
  /// dimension; 0 when unused.
  u32 tag = 0;
  /// Which way it travelled, for a packet (0 = inbound, 1 = outbound). Stored
  /// as a plain byte so the container need not depend on the net layer.
  u8 dir = 0;
  std::string payload;   // opaque bytes
};

/// One section of a replay: a keyframe or a delta, its tick span, and its
/// records.
struct BERRY_API ReplaySection {
  ReplaySectionKind kind = ReplaySectionKind::Keyframe;
  u64 startTick = 0;     // the first tick this section covers
  u64 endTick = 0;       // the last tick it covers (inclusive)
  std::vector<ReplayRecord> records;
};

/// One seek-index entry: where a keyframe's block lives and what tick it starts
/// at. A seek finds the last entry whose `startTick` is at or before the target
/// and decodes from there.
struct BERRY_API ReplayIndexEntry {
  u64 startTick = 0;
  u64 fileOffset = 0;    // byte offset of the keyframe's block in the file
  u32 recordCount = 0;
};

/// Caps. A replay is untrusted input, so every one of these is enforced before
/// an allocation or a seek is performed.
struct BERRY_API ReplayLimits {
  size_t maxSections = 100000;
  size_t maxRecordsPerSection = 65536;
  size_t maxPayloadBytes = 8u * 1024 * 1024;   // one record's payload
  /// One section's encoded size. Bounded well below 2 GiB because a block is
  /// handed to LZ4, whose API takes an `int` length; a producer that exceeds
  /// this splits the span into more sections.
  size_t maxSectionBytes = 64u * 1024 * 1024;
  size_t maxUncompressedBytes = 512u * 1024 * 1024;  // whole replay, decoded
  size_t maxLabelBytes = 128;
  /// How many trailing sections the ring-buffer mode keeps ("last N seconds").
  /// 0 means "keep everything".
  size_t ringSections = 0;

  static ReplayLimits Default() { return ReplayLimits{}; }
};

/// A replay: an ordered list of sections plus the seek index over its
/// keyframes. Built by `ReplayWriter`, read by `ReplayReader`. The in-memory
/// shape and the on-disk shape are the same, so a load followed by a save is
/// byte-stable.
class BERRY_API ReplayFile {
 public:
  ReplayFile() = default;

  ReplayFingerprint fingerprint;
  std::vector<ReplaySection> sections;
  /// Rebuilt from `sections` whenever the file is written; read from the file's
  /// own index when one is loaded, so a reader trusts the file's index rather
  /// than recomputing (and can detect an inconsistent one).
  std::vector<ReplayIndexEntry> index;

  size_t sectionCount() const { return sections.size(); }
  bool empty() const { return sections.empty(); }
  void Clear() { sections.clear(); index.clear(); fingerprint = ReplayFingerprint{}; }

  /// The total decoded payload bytes across every record, for a size report.
  size_t payloadBytes() const;
  /// The first and last tick covered, or 0 when empty.
  u64 firstTick() const;
  u64 lastTick() const;

  /// Appends a section. A section must not start before the previous one ends
  /// (a replay is ordered), must carry at most `limits.maxRecordsPerSection`
  /// records, and no record payload may exceed `limits.maxPayloadBytes`.
  /// Returns the error (Ok on success) and leaves the file untouched on error.
  ReplayError AddSection(const ReplaySection& section,
                         const ReplayLimits& limits = ReplayLimits::Default());

  /// Rebuilds the seek index from `sections`. Called after editing; the writer
  /// calls it again as it lays out blocks.
  void RebuildIndex();

  /// The index entry a seek to `tick` should start from: the last keyframe at
  /// or before `tick`, or index 0 when there is none. Returns false when there
  /// is no index.
  bool SeekEntry(u64 tick, ReplayIndexEntry* out) const;
};

//----------------------------------------------------------------------------
// Writer / reader
//----------------------------------------------------------------------------

/// Serialises a `ReplayFile` to the container format. Every block is LZ4
/// compressed; a block whose compressed form is not smaller than the raw bytes
/// is stored raw with a flag, so incompressible data does not grow.
class BERRY_API ReplayWriter {
 public:
  ReplayWriter() = default;

  /// Encodes `file` into `out` (appended to, so a caller can write a header
  /// first). Rebuilds the index as a side effect. Returns the error, and on
  /// error `out` may hold a partial document (the caller discards it).
  ReplayError Write(const ReplayFile& file, std::string* out,
                    const ReplayLimits& limits = ReplayLimits::Default());

  /// Encodes only the trailing `ringSections` sections (the "last N seconds"
  /// mode). 0 keeps everything. The fingerprint and the index are written for
  /// the kept sections only.
  ReplayError WriteRing(const ReplayFile& file, size_t ringSections,
                        std::string* out,
                        const ReplayLimits& limits = ReplayLimits::Default());

  /// The number of blocks written by the last `Write`, for a test.
  size_t lastBlockCount() const { return lastBlockCount_; }

 private:
  size_t lastBlockCount_ = 0;
};

/// Parses the container format into a `ReplayFile`. A file that fails any check
/// is refused as a whole; `out` is left as it was.
class BERRY_API ReplayReader {
 public:
  ReplayReader() = default;

  /// Parses `data`. When `requireFingerprint` is true, a file whose fingerprint
  /// does not equal `expected` is refused with `BadFingerprint` (the playback
  /// path uses this; a pure inspection does not).
  ReplayError Read(const std::string& data, ReplayFile* out,
                   const ReplayLimits& limits = ReplayLimits::Default());

  ReplayError Read(const std::string& data, const ReplayFingerprint& expected,
                   bool requireFingerprint, ReplayFile* out,
                   const ReplayLimits& limits = ReplayLimits::Default());

  /// The number of blocks the last `Read` decompressed, for a test.
  size_t lastBlockCount() const { return lastBlockCount_; }

 private:
  size_t lastBlockCount_ = 0;
};

//----------------------------------------------------------------------------
// Incremental recorder (ring buffer)
//----------------------------------------------------------------------------

/// Collects sections as they are produced and keeps only the trailing window
/// when a ring size is set, so "record the last 60 seconds" needs no unbounded
/// buffer. Sections are dropped from the front as new ones arrive, which is
/// exactly what a keyframe-anchored replay needs: the oldest keyframe is still
/// a valid anchor for the sections that follow it.
class BERRY_API ReplayRingBuffer {
 public:
  /// `ringSections` of 0 keeps everything.
  explicit ReplayRingBuffer(size_t ringSections = 0,
                            const ReplayLimits& limits = ReplayLimits::Default());

  /// Appends a section, evicting the oldest when the ring is full.
  ReplayError Push(const ReplaySection& section);

  size_t sectionCount() const { return file_.sections.size(); }
  /// How many sections were evicted since construction.
  u64 evicted() const { return evicted_; }
  bool empty() const { return file_.sections.empty(); }
  const ReplayFile& file() const { return file_; }
  void Clear();

  void SetFingerprint(const ReplayFingerprint& fp) { file_.fingerprint = fp; }
  const ReplayFingerprint& fingerprint() const { return file_.fingerprint; }

  void SetRingSections(size_t n) { ringSections_ = n; }

 private:
  ReplayFile file_;
  ReplayLimits limits_;
  size_t ringSections_ = 0;
  u64 evicted_ = 0;
};

}  // namespace Berry
