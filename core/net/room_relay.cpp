// Shared UDP relay engine. See core/net/room_relay.h for the design and
// core/net/relay_wire.h for the envelope.
#include "room_relay.h"

#include "berry/event.h"
#include "berry/logger.h"
#include "berry/manifest.h"  // crypto::HmacSha256Hex / ConstantTimeEquals
#include "relay_wire.h"

#include <algorithm>
#include <cstring>

namespace Berry {

// The public envelope sizes are defined once in relay_wire.h. Assert the relay's
// own constants agree, so a change to one without the other fails to build.
static_assert(RoomRelay::kTagBytes == relaywire::kTagBytes, "tag size");
static_assert(RoomRelay::kAuthBytes == relaywire::kAuthBytes, "auth size");
static_assert(RoomRelay::kMinSecretBytes == relaywire::kMinSecretBytes,
              "min secret");

namespace {

std::string HostPortKey(const std::string& host, u16 port) {
  return host + ":" + std::to_string(port);
}

}  // namespace

std::string RoomRelay::RoomTagHex(const std::string& name,
                                  const std::string& secret) {
  return crypto::HmacSha256Hex(secret,
                               std::string(relaywire::kRoomDomain) + name);
}

std::string RoomRelay::RegisterAuthHex(const std::string& tagHex,
                                       const std::string& secret) {
  return crypto::HmacSha256Hex(secret,
                               std::string(relaywire::kRegisterDomain) + tagHex);
}

RoomRelay::RoomRelay() = default;
RoomRelay::~RoomRelay() {
  Stop();
  Close();
}

bool RoomRelay::AddRoom(const std::string& name, const std::string& secret) {
  if (name.empty() || secret.size() < kMinSecretBytes) {
    Log::Warn("relay: room '%s' refused (short secret)", name.c_str());
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  for (const Room& r : rooms_) {
    if (r.name == name) return false;  // duplicate
  }
  if (rooms_.size() >= cfg_.maxRooms) return false;
  Room r;
  r.name = name;
  r.secret = secret;
  r.tagHex = RoomTagHex(name, secret);
  rooms_.push_back(std::move(r));
  return true;
}

size_t RoomRelay::roomCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return rooms_.size();
}

bool RoomRelay::Open(const RoomRelayConfig& cfg) {
  Close();
  if (cfg.mtu < relaywire::kEnvelopeBytes) {
    Log::Warn("relay: mtu too small for the envelope");
    return false;
  }
  if (rooms_.empty()) {
    Log::Warn("relay: no rooms configured");
    return false;
  }
  if (!sock_.Bind(cfg.port, cfg.bindAddr.c_str(), /*broadcast=*/false)) {
    Log::Warn("relay: bind failed: %s", sock_.LastError().c_str());
    return false;
  }
  cfg_ = cfg;
  open_ = true;
  return true;
}

void RoomRelay::Close() {
  Stop();
  sock_.Close();
  std::lock_guard<std::mutex> lk(mutex_);
  open_ = false;
  for (Room& r : rooms_) r.peers.clear();
}

u16 RoomRelay::localPort() const { return sock_.localPort(); }

RoomRelay::Room* RoomRelay::FindRoomLocked(const std::string& tagHex) {
  for (Room& r : rooms_) {
    if (crypto::ConstantTimeEquals(r.tagHex, tagHex)) return &r;
  }
  return nullptr;
}

void RoomRelay::EvictLocked(u64 now) {
  for (Room& r : rooms_) {
    for (auto it = r.peers.begin(); it != r.peers.end();) {
      if (now > it->second.lastMs && now - it->second.lastMs > cfg_.peerTtlMs) {
        it = r.peers.erase(it);
        stats_.peersEvicted++;
      } else {
        ++it;
      }
    }
  }
}

void RoomRelay::HandleLocked(const char* data, size_t n,
                             const std::string& host, u16 port, u64 now,
                             size_t* forwarded) {
  stats_.datagramsReceived++;
  if (!relaywire::HasPrefix(data, n)) {
    stats_.datagramsDropped++;
    return;
  }
  const u8 kind = (u8)data[5];
  const std::string tag(data + 6, kTagBytes);
  Room* room = FindRoomLocked(tag);
  if (room == nullptr) {
    stats_.datagramsDropped++;
    return;
  }
  const std::string key = HostPortKey(host, port);

  if (kind == relaywire::kKindRegister) {
    if (n < relaywire::kEnvelopeBytes + kAuthBytes) {
      stats_.datagramsDropped++;
      return;
    }
    const std::string auth(data + relaywire::kEnvelopeBytes, kAuthBytes);
    const std::string expect = RegisterAuthHex(tag, room->secret);
    // Constant-time compare, so a peer cannot learn the auth byte by byte.
    if (!crypto::ConstantTimeEquals(auth, expect)) {
      stats_.datagramsDropped++;
      return;
    }
    if (room->peers.find(key) == room->peers.end() &&
        room->peers.size() >= cfg_.maxPeersPerRoom) {
      stats_.datagramsDropped++;  // room full
      return;
    }
    room->peers[key].lastMs = now;
    stats_.registrations++;
    return;
  }

  if (kind == relaywire::kKindData) {
    if (n > cfg_.mtu) {
      stats_.datagramsDropped++;
      return;
    }
    // Only a registered source is forwarded: the relay is not an open relay.
    auto self = room->peers.find(key);
    if (self == room->peers.end()) {
      stats_.datagramsDropped++;
      return;
    }
    self->second.lastMs = now;  // activity keeps the registration alive
    for (const auto& kv : room->peers) {
      if (kv.first == key) continue;  // never echo to the sender
      std::string h;
      u16 p = 0;
      const size_t colon = kv.first.rfind(':');
      if (colon == std::string::npos) continue;
      h = kv.first.substr(0, colon);
      p = (u16)std::strtoul(kv.first.c_str() + colon + 1, nullptr, 10);
      if (sock_.SendTo(data, n, h.c_str(), p)) {
        stats_.datagramsForwarded++;
        if (forwarded) (*forwarded)++;
      }
    }
    return;
  }

  stats_.datagramsDropped++;  // unknown kind
}

size_t RoomRelay::Pump(int budgetMs) {
  if (!open_) return 0;
  std::vector<char> buf(cfg_.mtu + 1);
  const u64 deadline = NowMs() + (u64)std::max(0, budgetMs);
  size_t forwarded = 0;
  bool first = true;
  // Evict once up front: a peer must age out even on a tick that receives
  // nothing, otherwise an idle relay keeps a stale registration forever.
  {
    std::lock_guard<std::mutex> lk(mutex_);
    EvictLocked(NowMs());
  }
  for (;;) {
    std::string host;
    u16 port = 0;
    int wait = 0;
    if (budgetMs > 0 && first) {
      const u64 now = NowMs();
      wait = (int)(deadline > now ? deadline - now : 0);
    }
    const ssize_t n = sock_.RecvFrom(buf.data(), buf.size(), &host, &port, wait);
    first = false;
    if (n <= 0) break;
    std::lock_guard<std::mutex> lk(mutex_);
    const u64 now = NowMs();
    EvictLocked(now);
    HandleLocked(buf.data(), (size_t)n, host, port, now, &forwarded);
  }
  return forwarded;
}

bool RoomRelay::Start() {
  if (!open_ || running_) return false;
  running_ = true;
  thread_ = std::thread([this] {
    while (running_) Pump(20);
  });
  return true;
}

void RoomRelay::Stop() {
  if (!running_) return;
  running_ = false;
  if (thread_.joinable()) thread_.join();
}

RoomRelay::Stats RoomRelay::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

size_t RoomRelay::peerCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  size_t n = 0;
  for (const Room& r : rooms_) n += r.peers.size();
  return n;
}

}  // namespace Berry
