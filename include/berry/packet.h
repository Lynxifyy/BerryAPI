#pragma once
// Read-only packet inspection.
//
// A `PacketLog` records METADATA about packets this client sent and received:
// a monotonic sequence number, the direction, a protocol id and name, the byte
// length, and a timestamp. It never stores payload bytes.
//
// Why metadata only:
//
//   * A packet's payload is other players' data. Keeping it would turn a
//     diagnostics tool into a collection surface, so the log is built so it
//     cannot hold one.
//   * The useful questions ("am I being packet-spammed?", "is this packet id
//     new since the update?", "how much am I sending?") are all answered by
//     the header, not the body.
//
// The log is bounded: a fixed ring of recent entries plus running totals, so a
// flood cannot grow it without limit. Everything here is read-only to a script;
// the log is fed by the network layer, never written by script code.

#include "berry/export.h"
#include "berry/types.h"

#include <cstddef>
#include <mutex>
#include <string>
#include <vector>

namespace Berry {

/// Which way a packet travelled, from this client's point of view.
enum class PacketDir : u8 {
  Inbound = 0,
  Outbound,
  Count,
};

const char* PacketDirName(PacketDir d);

/// One observed packet, header fields only. `payloadLen` is the size, never the
/// bytes.
struct BERRY_API PacketRecord {
  u64 seq = 0;          // monotonic, unique per log
  PacketDir dir = PacketDir::Inbound;
  u32 id = 0;           // protocol packet id
  u32 payloadLen = 0;   // size in bytes (payload not retained)
  u64 timeMs = 0;       // when it was observed
  std::string name;     // resolved packet name, empty when unknown
};

/// Running totals, so a script can summarise without walking the ring.
struct BERRY_API PacketTotals {
  u64 inbound = 0;
  u64 outbound = 0;
  u64 inboundBytes = 0;
  u64 outboundBytes = 0;
  u64 dropped = 0;      // entries evicted from the ring, not lost packets

  u64 total() const { return inbound + outbound; }
};

/// A per-id summary, for "what is this connection actually made of".
struct BERRY_API PacketIdStat {
  u32 id = 0;
  std::string name;
  u64 count = 0;
  u64 bytes = 0;
  PacketDir dir = PacketDir::Inbound;
};

/// A bounded, read-only log of packet metadata.
///
/// Thread safety: the network layer feeds it from a worker thread while a script
/// reads from the script thread, so every access takes the lock. Snapshots are
/// returned by value; the log never hands out a pointer into its ring.
class BERRY_API PacketLog {
 public:
  /// How many recent records the ring keeps. Older ones are counted in the
  /// totals but not retained.
  static constexpr size_t kMaxRecords = 512;
  /// How many distinct ids `Summary()` will report, so a script cannot make the
  /// server build an unbounded table.
  static constexpr size_t kMaxSummaryIds = 64;

  /// Records one observed packet. Returns false when the record is refused
  /// (a payload larger than the sanity cap), so a bogus length cannot poison
  /// the totals.
  bool Record(PacketDir dir, u32 id, u32 payloadLen, u64 timeMs,
              const std::string& name = std::string());

  /// The most recent `max` records, newest first. A `max` of 0 means "all held".
  std::vector<PacketRecord> Recent(size_t max = 0) const;

  /// Running totals since construction (or the last Clear).
  PacketTotals totals() const;

  /// Per-id totals, highest count first, capped at kMaxSummaryIds. `dir` of
  /// `Count` means "both directions, one row each".
  std::vector<PacketIdStat> Summary(PacketDir dir = PacketDir::Count) const;

  /// Counts records matching an id, in the retained ring only.
  u64 CountOf(u32 id) const;

  /// The most recent record, if any.
  bool Last(PacketRecord* out) const;

  /// How many records the ring currently holds.
  size_t size() const;

  void Clear();

  /// Largest payload length accepted. A larger one is refused (a length field
  /// is attacker-influenced; a 4 GiB claim must not enter the totals).
  static constexpr u32 kMaxPayloadLen = 16u * 1024 * 1024;

 private:
  mutable std::mutex mutex_;
  std::vector<PacketRecord> ring_;   // newest at the back
  PacketTotals totals_;
  u64 nextSeq_ = 1;
};

}  // namespace Berry
