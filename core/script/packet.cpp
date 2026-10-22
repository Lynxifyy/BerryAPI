// Read-only packet inspection. See include/berry/packet.h for the design.
#include "berry/packet.h"

#include <algorithm>

namespace Berry {

const char* PacketDirName(PacketDir d) {
  switch (d) {
    case PacketDir::Inbound: return "inbound";
    case PacketDir::Outbound: return "outbound";
    default: return "unknown";
  }
}

bool PacketLog::Record(PacketDir dir, u32 id, u32 payloadLen, u64 timeMs,
                       const std::string& name) {
  if (dir != PacketDir::Inbound && dir != PacketDir::Outbound) return false;
  // Fail closed on a length that cannot be real: a hostile header must not be
  // able to inflate the byte totals with a 4 GiB claim.
  if (payloadLen > kMaxPayloadLen) return false;

  std::lock_guard<std::mutex> lk(mutex_);
  PacketRecord rec;
  rec.seq = nextSeq_++;
  rec.dir = dir;
  rec.id = id;
  rec.payloadLen = payloadLen;
  rec.timeMs = timeMs;
  rec.name = name;

  if (dir == PacketDir::Inbound) {
    ++totals_.inbound;
    totals_.inboundBytes += payloadLen;
  } else {
    ++totals_.outbound;
    totals_.outboundBytes += payloadLen;
  }

  ring_.push_back(std::move(rec));
  while (ring_.size() > kMaxRecords) {
    ring_.erase(ring_.begin());
    ++totals_.dropped;
  }
  return true;
}

std::vector<PacketRecord> PacketLog::Recent(size_t max) const {
  std::lock_guard<std::mutex> lk(mutex_);
  const size_t want = (max == 0 || max > ring_.size()) ? ring_.size() : max;
  std::vector<PacketRecord> out;
  out.reserve(want);
  // Newest first: walk the ring backwards.
  for (size_t i = 0; i < want; ++i) {
    out.push_back(ring_[ring_.size() - 1 - i]);
  }
  return out;
}

PacketTotals PacketLog::totals() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return totals_;
}

std::vector<PacketIdStat> PacketLog::Summary(PacketDir dir) const {
  std::lock_guard<std::mutex> lk(mutex_);
  // Aggregate the retained ring by (dir, id). A key of dir*1e9 + id keeps the
  // two directions distinct without a nested map.
  std::vector<PacketIdStat> stats;
  for (const PacketRecord& r : ring_) {
    if (dir != PacketDir::Count && r.dir != dir) continue;
    auto it = std::find_if(stats.begin(), stats.end(), [&](const PacketIdStat& s) {
      return s.id == r.id && s.dir == r.dir;
    });
    if (it == stats.end()) {
      PacketIdStat s;
      s.id = r.id;
      s.name = r.name;
      s.dir = r.dir;
      s.count = 1;
      s.bytes = r.payloadLen;
      stats.push_back(std::move(s));
    } else {
      ++it->count;
      it->bytes += r.payloadLen;
      if (it->name.empty()) it->name = r.name;
    }
  }
  std::sort(stats.begin(), stats.end(), [](const PacketIdStat& a, const PacketIdStat& b) {
    if (a.count != b.count) return a.count > b.count;
    if (a.id != b.id) return a.id < b.id;
    return (u8)a.dir < (u8)b.dir;
  });
  if (stats.size() > kMaxSummaryIds) stats.resize(kMaxSummaryIds);
  return stats;
}

u64 PacketLog::CountOf(u32 id) const {
  std::lock_guard<std::mutex> lk(mutex_);
  u64 n = 0;
  for (const PacketRecord& r : ring_) {
    if (r.id == id) ++n;
  }
  return n;
}

bool PacketLog::Last(PacketRecord* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (ring_.empty()) return false;
  if (out) *out = ring_.back();
  return true;
}

size_t PacketLog::size() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return ring_.size();
}

void PacketLog::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  ring_.clear();
  totals_ = PacketTotals{};
  nextSeq_ = 1;
}

}  // namespace Berry
