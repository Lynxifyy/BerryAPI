// LAN transport for the cosmetic sync channel. See include/berry/lan_transport.h
// for the wire format and the fail-closed rules.
#include "berry/lan_transport.h"
#include "berry/logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace Berry {

namespace {

constexpr u8 kVersion = 1;
constexpr u8 kKindMessage = 1;
constexpr u8 kKindAnnounce = 2;

u64 SteadyNowMs() {
  using namespace std::chrono;
  return (u64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

/// A per-instance sender tag. Unique enough that two clients on one subnet do
/// not share a tag, so a client recognises (and skips) the copy of its own
/// traffic that multicast loops back. Not a security value: a peer's datagram
/// still goes through signature verification before it is applied.
u32 MakeSelfTag() {
  std::random_device rd;
  u32 t = ((u32)rd() << 16) ^ (u32)rd() ^ (u32)SteadyNowMs();
  return t ? t : 1u;   // 0 means "no tag"; never hand that out
}

void PutU16(u8* p, u16 v) {
  p[0] = (u8)(v >> 8);
  p[1] = (u8)v;
}
void PutU32(u8* p, u32 v) {
  p[0] = (u8)(v >> 24);
  p[1] = (u8)(v >> 16);
  p[2] = (u8)(v >> 8);
  p[3] = (u8)v;
}
u16 GetU16(const u8* p) { return (u16)(((u16)p[0] << 8) | p[1]); }
u32 GetU32(const u8* p) {
  return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

std::string PeerKey(const std::string& host, u16 port) {
  return host + ":" + std::to_string(port);
}

}  // namespace

const char* LanCosmeticTransport::Magic() { return "BLAN"; }

void LanCosmeticTransport::FillHeader(u8* out, u8 kind, u16 msgId, u16 index,
                                      u16 count, u32 totalLen, u16 payloadPer,
                                      u32 tag) {
  std::memcpy(out, Magic(), 4);
  out[4] = kVersion;
  out[5] = kind;
  PutU16(out + 6, msgId);
  PutU16(out + 8, index);
  PutU16(out + 10, count);
  PutU32(out + 12, totalLen);
  PutU16(out + 16, payloadPer);
  PutU16(out + 18, 0);   // reserved
  PutU32(out + 20, tag);
}

LanCosmeticTransport::LanCosmeticTransport() { clock_ = &SteadyNowMs; }
LanCosmeticTransport::~LanCosmeticTransport() { Close(); }

bool LanCosmeticTransport::Open(const LanTransportConfig& cfg) {
  Close();
  // A fragment must fit one datagram with the header; anything smaller than the
  // header cannot carry a byte, so refuse rather than divide by zero below.
  if (cfg.mtu <= kHeaderBytes) {
    Log::Warn("lan: mtu too small for the fragment header");
    return false;
  }
  if (!sock_.Bind(cfg.port, "0.0.0.0", /*broadcast=*/true)) {
    Log::Warn("lan: bind failed: %s", sock_.LastError().c_str());
    return false;
  }
  cfg_ = cfg;
  open_ = true;
  // A fresh tag per open, so this instance skips the copy of its own datagrams
  // that multicast loops back. Assigned before any send, so Send is safe even
  // without an Announce.
  selfTag_ = MakeSelfTag();
  // Seed the message id from the clock so a restart does not reuse ids that a
  // peer may still hold a partial for. The id is 16 bits and wraps; the
  // reassembly TTL and the per-peer cap bound how long a stale partial can
  // live, so a wrap cannot splice two messages (and a shape mismatch drops the
  // reassembly anyway).
  msgSeq_ = (u16)(clock_() & 0xffff);
  // A failed group join is not fatal: a unicast-only socket still works between
  // two clients that know each other's address, and the caller can see why.
  joined_ = sock_.JoinMulticast(cfg.group.c_str(), cfg.iface.c_str());
  if (!joined_) {
    Log::Warn("lan: multicast join failed: %s", sock_.LastError().c_str());
  } else if (cfg.iface != "0.0.0.0") {
    // Pin the outgoing interface so a send to the group is delivered to a local
    // member (a host test on loopback depends on this).
    sock_.SetMulticastInterface(cfg.iface.c_str());
  }
  return true;
}

void LanCosmeticTransport::Close() {
  sock_.Close();
  std::lock_guard<std::mutex> lk(mutex_);
  open_ = false;
  joined_ = false;
  reassembly_.clear();
  peers_.clear();
}

u16 LanCosmeticTransport::localPort() const { return sock_.localPort(); }

bool LanCosmeticTransport::Send(const std::string& message) {
  return SendFramed(cfg_.group, cfg_.port, message);
}

bool LanCosmeticTransport::SendTo(const std::string& host, u16 port,
                                  const std::string& message) {
  return SendFramed(host, port, message);
}

bool LanCosmeticTransport::SendFramed(const std::string& host, u16 port,
                                      const std::string& message) {
  if (!open_) return false;
  if (message.empty() || message.size() > cfg_.maxMessageBytes) {
    ++stats_.messagesDropped;
    return false;
  }

  const size_t payloadPer = cfg_.mtu - kHeaderBytes;
  const size_t fragCount = (message.size() + payloadPer - 1) / payloadPer;
  if (fragCount > 0xffff) {   // cannot be described by the u16 count field
    ++stats_.messagesDropped;
    return false;
  }

  const u16 msgId = msgSeq_++;
  const u32 tag = selfTag_;   // our own tag: a receiver ignores its loopback copy
  std::vector<u8> datagram(cfg_.mtu);
  for (size_t i = 0; i < fragCount; ++i) {
    const size_t off = i * payloadPer;
    const size_t chunk = std::min(payloadPer, message.size() - off);

    FillHeader(&datagram[0], kKindMessage, msgId, (u16)i, (u16)fragCount,
               (u32)message.size(), (u16)payloadPer, tag);
    std::memcpy(&datagram[kHeaderBytes], message.data() + off, chunk);

    if (!sock_.SendTo(&datagram[0], kHeaderBytes + chunk, host.c_str(), port)) {
      Log::Warn("lan: sendto failed: %s", sock_.LastError().c_str());
      return false;
    }
    ++stats_.datagramsSent;
  }
  ++stats_.messagesSent;
  return true;
}

bool LanCosmeticTransport::Announce(const std::string& playerId) {
  if (!open_) return false;

  std::vector<u8> datagram(kHeaderBytes + playerId.size());
  FillHeader(&datagram[0], kKindAnnounce, 0, 0, 1, (u32)playerId.size(), 0,
             selfTag_);
  std::memcpy(&datagram[kHeaderBytes], playerId.data(), playerId.size());

  if (!sock_.SendTo(&datagram[0], datagram.size(), cfg_.group.c_str(),
                    cfg_.port)) {
    Log::Warn("lan: announce sendto failed: %s", sock_.LastError().c_str());
    return false;
  }
  ++stats_.datagramsSent;
  return true;
}

void LanCosmeticTransport::TouchPeerLocked(const std::string& host, u16 port,
                                           const std::string& playerId, u64 now) {
  const std::string key = PeerKey(host, port);
  auto it = peers_.find(key);
  if (it == peers_.end()) {
    if (peers_.size() >= cfg_.maxPeers) {
      // Evict the least-recently-seen peer so a flood of distinct addresses
      // cannot grow the table without bound.
      auto victim = std::min_element(
          peers_.begin(), peers_.end(),
          [](const auto& a, const auto& b) { return a.second.lastSeenMs < b.second.lastSeenMs; });
      if (victim != peers_.end()) peers_.erase(victim);
    }
    LanPeer p;
    p.host = host;
    p.port = port;
    p.playerId = playerId;
    p.lastSeenMs = now;
    peers_.emplace(key, std::move(p));
    return;
  }
  it->second.lastSeenMs = now;
  if (!playerId.empty()) it->second.playerId = playerId;
}

void LanCosmeticTransport::ExpireLocked(const std::string& peer, u64 now) {
  const u64 ttl = cfg_.reassemblyTtlMs;
  for (auto it = reassembly_.begin(); it != reassembly_.end();) {
    const bool stale = now > it->second.firstMs + ttl;
    // A peer may keep at most `maxReassemblyPerPeer` partials; the rest are
    // dropped, oldest first (a linear scan is fine for a handful).
    if (stale) {
      ++stats_.messagesDropped;
      it = reassembly_.erase(it);
    } else {
      ++it;
    }
  }
  // Enforce the per-peer cap.
  size_t count = 0;
  for (const auto& kv : reassembly_) {
    if (kv.second.peer == peer) ++count;
  }
  while (count > cfg_.maxReassemblyPerPeer) {
    // Drop the oldest partial for this peer.
    auto oldest = reassembly_.end();
    for (auto it = reassembly_.begin(); it != reassembly_.end(); ++it) {
      if (it->second.peer != peer) continue;
      if (oldest == reassembly_.end() || it->second.firstMs < oldest->second.firstMs) {
        oldest = it;
      }
    }
    if (oldest == reassembly_.end()) break;
    reassembly_.erase(oldest);
    ++stats_.messagesDropped;
    --count;
  }
}

void LanCosmeticTransport::HandleDatagramLocked(const u8* data, size_t len,
                                                const std::string& host, u16 port,
                                                std::vector<Complete>* complete) {
  ++stats_.datagramsReceived;
  if (len < kHeaderBytes || std::memcmp(data, Magic(), 4) != 0 ||
      data[4] != kVersion) {
    ++stats_.fragmentsDropped;
    return;
  }
  const u8 kind = data[5];
  const u32 tag = GetU32(&data[20]);
  // Our own datagram, looped back by multicast: skip it.
  if (tag == selfTag_) {
    ++stats_.selfSkipped;
    return;
  }
  const size_t payload = len - kHeaderBytes;

  if (kind == kKindAnnounce) {
    std::string id((const char*)data + kHeaderBytes, payload);
    TouchPeerLocked(host, port, id, clock_());
    return;
  }
  if (kind != kKindMessage) {
    ++stats_.fragmentsDropped;
    return;
  }

  const u16 msgId = GetU16(&data[6]);
  const u16 index = GetU16(&data[8]);
  const u16 count = GetU16(&data[10]);
  const u32 totalLen = GetU32(&data[12]);
  const u16 payloadPer = GetU16(&data[16]);

  if (count == 0 || index >= count || totalLen == 0 || payloadPer == 0 ||
      totalLen > cfg_.maxMessageBytes) {
    ++stats_.fragmentsDropped;
    return;
  }
  // The sender states how many message bytes each fragment carries, so this
  // receiver computes the right offset even when its own MTU differs. Each
  // fragment must be exactly one slice: every fragment full except the last. A
  // short non-final fragment, or a payload past the declared length, is a
  // malformed message rather than one we guess at.
  const size_t per = payloadPer;
  const size_t offset = (size_t)index * per;
  if (offset >= totalLen && totalLen != 0) {
    ++stats_.fragmentsDropped;
    return;
  }
  const size_t expected = (index + 1 == count) ? totalLen - offset : per;
  if (payload != expected || offset + payload > totalLen) {
    ++stats_.fragmentsDropped;
    return;
  }

  const std::string peer = PeerKey(host, port);
  const std::string key = peer + "#" + std::to_string(msgId);
  ExpireLocked(peer, clock_());

  auto it = reassembly_.find(key);
  if (it == reassembly_.end()) {
    Reassembly r;
    r.peer = peer;
    r.firstMs = clock_();
    r.totalLen = totalLen;
    r.fragCount = count;
    r.data.assign(totalLen, 0);
    r.have.assign(count, 0);
    it = reassembly_.emplace(key, std::move(r)).first;
  } else if (it->second.totalLen != totalLen || it->second.fragCount != count) {
    // A fragment disagreeing with the message's own shape poisons the whole
    // reassembly: drop it rather than splice two different messages together.
    ++stats_.fragmentsDropped;
    ++stats_.messagesDropped;
    reassembly_.erase(it);
    return;
  }

  Reassembly& r = it->second;
  if (!r.have[index]) {
    std::memcpy(&r.data[offset], data + kHeaderBytes, payload);
    r.have[index] = 1;
    ++r.received;
  }

  if (r.received == r.fragCount) {
    ++stats_.messagesReceived;
    Complete c;
    c.message.assign((const char*)r.data.data(), r.data.size());
    c.host = host;
    c.port = port;
    complete->push_back(std::move(c));
    reassembly_.erase(it);
  }
}

void LanCosmeticTransport::Poll(const MessageCb& onMessage, int budgetMs) {
  if (!open_) return;
  const u64 deadline = clock_() + (u64)std::max(0, budgetMs);
  std::vector<u8> buf(cfg_.mtu);
  std::vector<Complete> done;

  // Loop until the budget is spent. Each datagram is handled under the lock;
  // the callbacks run after, outside it, so a slow callback cannot stall a
  // sender and the callback is free to call back in.
  for (;;) {
    const u64 now = clock_();
    const int remaining = now >= deadline ? 0 : (int)(deadline - now);
    std::string host;
    u16 port = 0;
    ssize_t n = sock_.RecvFrom(&buf[0], buf.size(), &host, &port, remaining);
    if (n <= 0) break;   // timeout (0) or error (-1)

    done.clear();
    {
      std::lock_guard<std::mutex> lk(mutex_);
      HandleDatagramLocked(&buf[0], (size_t)n, host, port, &done);
    }
    if (onMessage) {
      for (const Complete& c : done) onMessage(c.message, c.host, c.port);
    }
    if (now >= deadline) break;
  }

  // Expire peers past their TTL and reassembly buffers past theirs, even when
  // no datagram arrived, so a peer that goes quiet (or sends half a message and
  // stops) does not leave state behind.
  std::lock_guard<std::mutex> lk(mutex_);
  const u64 now = clock_();
  for (auto it = peers_.begin(); it != peers_.end();) {
    if (now > it->second.lastSeenMs + cfg_.peerTtlMs) it = peers_.erase(it);
    else ++it;
  }
  for (auto it = reassembly_.begin(); it != reassembly_.end();) {
    if (now > it->second.firstMs + cfg_.reassemblyTtlMs) {
      ++stats_.messagesDropped;
      it = reassembly_.erase(it);
    } else {
      ++it;
    }
  }
}

std::vector<LanPeer> LanCosmeticTransport::peers() const {
  std::lock_guard<std::mutex> lk(mutex_);
  const u64 now = clock_();
  std::vector<LanPeer> out;
  for (const auto& kv : peers_) {
    if (now <= kv.second.lastSeenMs + cfg_.peerTtlMs) out.push_back(kv.second);
  }
  std::sort(out.begin(), out.end(),
            [](const LanPeer& a, const LanPeer& b) { return a.lastSeenMs > b.lastSeenMs; });
  return out;
}

LanCosmeticTransport::Stats LanCosmeticTransport::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

void LanCosmeticTransport::SetClock(std::function<u64()> nowMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  clock_ = nowMs ? std::move(nowMs) : std::function<u64()>(&SteadyNowMs);
}

}  // namespace Berry
