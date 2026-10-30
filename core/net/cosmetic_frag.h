#pragma once
// Internal fragment framing shared by the cosmetic-sync transports.
//
// A signed cosmetic-sync envelope (cosmetic_sync.h) is larger than one datagram
// can carry, so a transport frames it into fragments and reassembles them on the
// far side before handing the whole envelope up. Two transports move it:
// `LanCosmeticTransport` sends the fragments to an IPv4 multicast group, and
// `CosmeticRelayTransport` sends them to a UDP relay (wrapped in the relay
// envelope). Both must speak byte-for-byte the same fragment format, so it is
// defined once here rather than copied into each transport.
//
// Wire header (24 bytes, big-endian):
//
//   offset  size  field
//   0       4     magic 'B','L','A','N'
//   4       1     version (1)
//   5       1     kind: 1 = message fragment, 2 = announce
//   6       2     msgId (per-sender, wraps)
//   8       2     fragment index (0-based)
//   10      2     fragment count
//   12      4     total message length in bytes
//   16      2     fragment payload size: how many message bytes each fragment
//                 carries. The sender states it, so a receiver with a different
//                 MTU still computes the right offset instead of guessing.
//   18      2     reserved (0)
//   20      4     sender tag: a per-instance value the sender stamps on every
//                 datagram, so a client ignores the copy of its own datagram
//                 that multicast loops back
//
// A message fragment's payload is the message slice for that index; an announce
// carries the sender's player id. A kind or field outside the contract is
// dropped. See include/berry/lan_transport.h for the multicast transport's
// threat model.

#include "berry/types.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {
namespace cosmeticfrag {

inline constexpr char kMagic[4] = {'B', 'L', 'A', 'N'};
inline constexpr u8 kVersion = 1;
inline constexpr u8 kKindMessage = 1;
inline constexpr u8 kKindAnnounce = 2;
inline constexpr size_t kHeaderBytes = 24;

inline void PutU16(u8* p, u16 v) {
  p[0] = (u8)(v >> 8);
  p[1] = (u8)v;
}
inline void PutU32(u8* p, u32 v) {
  p[0] = (u8)(v >> 24);
  p[1] = (u8)(v >> 16);
  p[2] = (u8)(v >> 8);
  p[3] = (u8)v;
}
inline u16 GetU16(const u8* p) { return (u16)(((u16)p[0] << 8) | p[1]); }
inline u32 GetU32(const u8* p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

/// True when `data` starts with our magic and version.
inline bool HasPrefix(const u8* data, size_t len) {
  return len >= kHeaderBytes && std::memcmp(data, kMagic, 4) == 0 &&
         data[4] == kVersion;
}

/// Writes the fixed header into `out` (which must have kHeaderBytes free).
inline void FillHeader(u8* out, u8 kind, u16 msgId, u16 index, u16 count,
                       u32 totalLen, u16 payloadPer, u32 tag) {
  std::memcpy(out, kMagic, 4);
  out[4] = kVersion;
  out[5] = kind;
  PutU16(out + 6, msgId);
  PutU16(out + 8, index);
  PutU16(out + 10, count);
  PutU32(out + 12, totalLen);
  PutU16(out + 16, payloadPer);
  PutU16(out + 18, 0);  // reserved
  PutU32(out + 20, tag);
}

/// The fields of one parsed fragment.
struct Header {
  u8 kind = 0;
  u16 msgId = 0;
  u16 index = 0;
  u16 count = 0;
  u32 totalLen = 0;
  u16 payloadPer = 0;
  u32 tag = 0;
};

/// Parses a fragment header. False (fail closed) on a bad magic/version or a
/// short datagram; the caller then drops it.
inline bool Parse(const u8* data, size_t len, Header* out) {
  if (!HasPrefix(data, len)) return false;
  out->kind = data[5];
  out->msgId = GetU16(data + 6);
  out->index = GetU16(data + 8);
  out->count = GetU16(data + 10);
  out->totalLen = GetU32(data + 12);
  out->payloadPer = GetU16(data + 16);
  out->tag = GetU32(data + 20);
  return true;
}

/// A completed message and the sender it came from.
struct Complete {
  std::string message;
  std::string host;
  u16 port = 0;
};

/// How `Reassembler::Add` treated one fragment.
enum class AddResult {
  Incomplete,  // stored; the message is not yet whole
  Complete,    // this fragment finished the message (see `out`)
  Dropped,     // malformed, out of range, or a shape mismatch
};

/// Receiver-side fragment reassembly, shared by both cosmetic transports. Holds
/// a bounded per-sender table of partial messages and enforces a fragment-count
/// bound and a TTL, so a peer that sends half a message (or a flood of partial
/// messages) cannot grow memory without limit. A fragment that disagrees with
/// the message's own declared shape poisons the whole reassembly rather than
/// splicing two messages together.
class Reassembler {
 public:
  /// `maxMessageBytes` caps a reassembled message; `maxPerPeer` caps how many
  /// partial messages one sender may have in flight; `ttlMs` bounds how long a
  /// partial is kept.
  Reassembler(size_t maxMessageBytes, size_t maxPerPeer, u64 ttlMs)
      : maxMessageBytes_(maxMessageBytes),
        maxPerPeer_(maxPerPeer),
        ttlMs_(ttlMs) {}

  /// Adds one fragment. `now` is the current monotonic time. On Complete, `out`
  /// is filled and the reassembly is removed. `dropped` (optional) is set true
  /// when the fragment was dropped rather than stored — for the caller's stats.
  AddResult Add(const std::string& host, u16 port, const u8* data, size_t len,
                u64 now, Complete* out, bool* dropped = nullptr) {
    if (dropped) *dropped = false;
    Header h;
    if (!Parse(data, len, &h) || h.kind != kKindMessage) {
      if (dropped) *dropped = true;
      return AddResult::Dropped;
    }
    const size_t payload = len - kHeaderBytes;
    if (h.count == 0 || h.index >= h.count || h.totalLen == 0 ||
        h.payloadPer == 0 || h.totalLen > maxMessageBytes_) {
      if (dropped) *dropped = true;
      return AddResult::Dropped;
    }
    const size_t per = h.payloadPer;
    const size_t offset = (size_t)h.index * per;
    if (offset >= h.totalLen) {
      if (dropped) *dropped = true;
      return AddResult::Dropped;
    }
    const size_t expected = (h.index + 1 == h.count) ? h.totalLen - offset : per;
    if (payload != expected || offset + payload > h.totalLen) {
      if (dropped) *dropped = true;
      return AddResult::Dropped;
    }

    const std::string peer = host + ":" + std::to_string(port);
    const std::string key = peer + "#" + std::to_string(h.msgId);
    Expire(peer, now);

    auto it = table_.find(key);
    if (it == table_.end()) {
      Reassembly r;
      r.peer = peer;
      r.host = host;
      r.port = port;
      r.firstMs = now;
      r.totalLen = h.totalLen;
      r.fragCount = h.count;
      r.data.assign(h.totalLen, 0);
      r.have.assign(h.count, 0);
      it = table_.emplace(key, std::move(r)).first;
    } else if (it->second.totalLen != h.totalLen ||
               it->second.fragCount != h.count) {
      // A fragment disagreeing with the message's own shape poisons the whole
      // reassembly: drop it rather than splice two different messages together.
      table_.erase(it);
      if (dropped) *dropped = true;
      return AddResult::Dropped;
    }

    Reassembly& r = it->second;
    if (!r.have[h.index]) {
      std::memcpy(&r.data[offset], data + kHeaderBytes, payload);
      r.have[h.index] = 1;
      ++r.received;
    }
    if (r.received == r.fragCount) {
      out->message.assign((const char*)r.data.data(), r.data.size());
      out->host = r.host;
      out->port = r.port;
      table_.erase(it);
      return AddResult::Complete;
    }
    return AddResult::Incomplete;
  }

  /// Drops partials older than the TTL. Returns how many were dropped, so the
  /// caller can account them. Call periodically even with no traffic.
  size_t ExpireAll(u64 now) {
    size_t dropped = 0;
    for (auto it = table_.begin(); it != table_.end();) {
      if (now > it->second.firstMs + ttlMs_) {
        it = table_.erase(it);
        ++dropped;
      } else {
        ++it;
      }
    }
    return dropped;
  }

  void Clear() { table_.clear(); }
  size_t size() const { return table_.size(); }

 private:
  struct Reassembly {
    std::string peer;   // host:port the fragments came from
    std::string host;
    u16 port = 0;
    u64 firstMs = 0;
    u32 totalLen = 0;
    u16 fragCount = 0;
    u16 received = 0;
    std::vector<u8> data;
    std::vector<u8> have;
  };

  /// Drops a peer's oldest partials past the per-peer cap, and any expired.
  void Expire(const std::string& peer, u64 now) {
    for (auto it = table_.begin(); it != table_.end();) {
      if (now > it->second.firstMs + ttlMs_) it = table_.erase(it);
      else ++it;
    }
    size_t count = 0;
    for (const auto& kv : table_) {
      if (kv.second.peer == peer) ++count;
    }
    while (count > maxPerPeer_) {
      auto oldest = table_.end();
      for (auto it = table_.begin(); it != table_.end(); ++it) {
        if (it->second.peer != peer) continue;
        if (oldest == table_.end() ||
            it->second.firstMs < oldest->second.firstMs) {
          oldest = it;
        }
      }
      if (oldest == table_.end()) break;
      table_.erase(oldest);
      --count;
    }
  }

  size_t maxMessageBytes_;
  size_t maxPerPeer_;
  u64 ttlMs_;
  std::unordered_map<std::string, Reassembly> table_;
};

/// Splits `message` into fragments of `capacity` message bytes each and calls
/// `sink(index, count, totalLen, payloadPer, chunkOffset, chunkLen)` for each.
/// Returns false (nothing sent) when the message is empty, over `maxMessageBytes`
/// or needs more than 0xffff fragments to describe.
template <typename Sink>
inline bool Fragment(const std::string& message, size_t capacity,
                     size_t maxMessageBytes, Sink&& sink) {
  if (message.empty() || message.size() > maxMessageBytes) return false;
  if (capacity == 0) return false;
  const size_t fragCount = (message.size() + capacity - 1) / capacity;
  if (fragCount > 0xffff) return false;
  for (size_t i = 0; i < fragCount; ++i) {
    const size_t off = i * capacity;
    const size_t chunk = std::min(capacity, message.size() - off);
    if (!sink((u16)i, (u16)fragCount, (u32)message.size(), (u16)capacity, off,
              chunk)) {
      return false;
    }
  }
  return true;
}

}  // namespace cosmeticfrag
}  // namespace Berry
