// LAN transport for the cosmetic sync channel. See include/berry/lan_transport.h
// for the wire format and the fail-closed rules.
#include "berry/lan_transport.h"
#include "berry/logger.h"
#include "cosmetic_frag.h"
#include "multicast_lock.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <random>

namespace Berry {

using cosmeticfrag::AddResult;
using cosmeticfrag::Reassembler;

namespace {

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

std::string PeerKey(const std::string& host, u16 port) {
  return host + ":" + std::to_string(port);
}

}  // namespace

const char* LanCosmeticTransport::Magic() { return cosmeticfrag::kMagic; }

void LanCosmeticTransport::FillHeader(u8* out, u8 kind, u16 msgId, u16 index,
                                      u16 count, u32 totalLen, u16 payloadPer,
                                      u32 tag) {
  cosmeticfrag::FillHeader(out, kind, msgId, index, count, totalLen, payloadPer,
                           tag);
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
  // The reassembly table's caps come from the config; a fresh one per open.
  reassembler_ = std::make_unique<Reassembler>(cfg.maxMessageBytes,
                                               cfg.maxReassemblyPerPeer,
                                               cfg.reassemblyTtlMs);
  // Android drops multicast to a joined group without a Wi-Fi multicast lock,
  // so hold one before joining. Best-effort: a failed acquire still leaves a
  // unicast socket, and `multicastJoined` reports the join outcome.
  mcastLock_ = std::make_unique<MulticastLock>();
  mcastLock_->Acquire("BerryAPI cosmetic");
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
  if (mcastLock_) {
    mcastLock_->Release();
    mcastLock_.reset();
  }
  std::lock_guard<std::mutex> lk(mutex_);
  open_ = false;
  joined_ = false;
  if (reassembler_) reassembler_->Clear();
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
  const size_t payloadPer = cfg_.mtu - kHeaderBytes;
  const u16 msgId = msgSeq_++;
  const u32 tag = selfTag_;   // our own tag: a receiver ignores its loopback copy
  std::vector<u8> datagram(cfg_.mtu);
  const bool ok = cosmeticfrag::Fragment(
      message, payloadPer, cfg_.maxMessageBytes,
      [&](u16 index, u16 count, u32 totalLen, u16 per, size_t off,
          size_t chunk) {
        FillHeader(&datagram[0], cosmeticfrag::kKindMessage, msgId, index, count,
                   totalLen, per, tag);
        std::memcpy(&datagram[kHeaderBytes], message.data() + off, chunk);
        if (!sock_.SendTo(&datagram[0], kHeaderBytes + chunk, host.c_str(),
                          port)) {
          Log::Warn("lan: sendto failed: %s", sock_.LastError().c_str());
          return false;
        }
        ++stats_.datagramsSent;
        return true;
      });
  if (!ok) {
    ++stats_.messagesDropped;
    return false;
  }
  ++stats_.messagesSent;
  return true;
}

bool LanCosmeticTransport::Announce(const std::string& playerId) {
  if (!open_) return false;

  std::vector<u8> datagram(kHeaderBytes + playerId.size());
  FillHeader(&datagram[0], cosmeticfrag::kKindAnnounce, 0, 0, 1,
             (u32)playerId.size(), 0, selfTag_);
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

void LanCosmeticTransport::HandleDatagramLocked(const u8* data, size_t len,
                                                const std::string& host, u16 port,
                                                std::vector<Complete>* complete) {
  ++stats_.datagramsReceived;
  cosmeticfrag::Header h;
  if (!cosmeticfrag::Parse(data, len, &h)) {
    ++stats_.fragmentsDropped;
    return;
  }
  // Our own datagram, looped back by multicast: skip it.
  if (h.tag == selfTag_) {
    ++stats_.selfSkipped;
    return;
  }

  if (h.kind == cosmeticfrag::kKindAnnounce) {
    const size_t payload = len - kHeaderBytes;
    std::string id((const char*)data + kHeaderBytes, payload);
    TouchPeerLocked(host, port, id, clock_());
    return;
  }
  if (h.kind != cosmeticfrag::kKindMessage) {
    ++stats_.fragmentsDropped;
    return;
  }

  cosmeticfrag::Complete c;
  bool dropped = false;
  const AddResult r =
      reassembler_->Add(host, port, data, len, clock_(), &c, &dropped);
  if (r == AddResult::Dropped) {
    ++stats_.fragmentsDropped;
    if (dropped) ++stats_.messagesDropped;
    return;
  }
  if (r == AddResult::Complete) {
    ++stats_.messagesReceived;
    Complete out;
    out.message = std::move(c.message);
    out.host = std::move(c.host);
    out.port = c.port;
    complete->push_back(std::move(out));
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
  stats_.messagesDropped += reassembler_->ExpireAll(now);
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
