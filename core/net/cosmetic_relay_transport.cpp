// Cosmetic-sync transport over a UDP relay. See
// include/berry/cosmetic_relay_transport.h for the design and the room envelope.
#include "berry/cosmetic_relay_transport.h"

#include "berry/cosmetic_relay.h"  // CosmeticRelay::RoomTagHex / RegisterAuthHex
#include "berry/logger.h"
#include "cosmetic_frag.h"
#include "relay_wire.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace Berry {

// The public header declares the wire sizes; the shared headers define them.
// Assert they agree, so a change to one without the other fails to build rather
// than putting two incompatible formats on the wire.
static_assert(CosmeticRelayTransport::kVersion == relaywire::kVersion,
              "relay version");
static_assert(CosmeticRelayTransport::kKindRegister == relaywire::kKindRegister,
              "reg kind");
static_assert(CosmeticRelayTransport::kKindData == relaywire::kKindData,
              "data kind");
static_assert(CosmeticRelayTransport::kTagBytes == relaywire::kTagBytes, "tag");
static_assert(CosmeticRelayTransport::kAuthBytes == relaywire::kAuthBytes,
              "auth");
static_assert(CosmeticRelayTransport::kEnvelopeBytes ==
                  relaywire::kEnvelopeBytes,
              "envelope");
static_assert(CosmeticRelayTransport::kMinSecretBytes ==
                  relaywire::kMinSecretBytes,
              "min secret");
static_assert(CosmeticRelayTransport::kHeaderBytes ==
                  cosmeticfrag::kHeaderBytes,
              "fragment header");

namespace {

u64 SteadyNowMs() {
  using namespace std::chrono;
  return (u64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

const char* CosmeticRelayTransport::Magic() {
  static char m[5] = {relaywire::kMagic[0], relaywire::kMagic[1],
                      relaywire::kMagic[2], relaywire::kMagic[3], '\0'};
  return m;
}

CosmeticRelayTransport::CosmeticRelayTransport() { clock_ = &SteadyNowMs; }
CosmeticRelayTransport::~CosmeticRelayTransport() { Close(); }

bool CosmeticRelayTransport::Open(const RelayCosmeticConfig& cfg) {
  Close();
  if (cfg.mtu <= relaywire::kEnvelopeBytes + cosmeticfrag::kHeaderBytes) {
    Log::Warn("cosmetic relay: mtu too small for the envelope and the header");
    return false;
  }
  if (cfg.room.empty() || cfg.roomSecret.size() < kMinSecretBytes) {
    Log::Warn("cosmetic relay: room secret must be at least %zu bytes",
              kMinSecretBytes);
    return false;
  }
  if (cfg.relayHost.empty()) {
    Log::Warn("cosmetic relay: no relay host");
    return false;
  }
  if (!sock_.Bind(cfg.bindPort, cfg.bindAddr.c_str(), /*broadcast=*/false)) {
    Log::Warn("cosmetic relay: bind failed: %s", sock_.LastError().c_str());
    return false;
  }
  cfg_ = cfg;
  tagHex_ = CosmeticRelay::RoomTagHex(cfg.room, cfg.roomSecret);
  authHex_ = CosmeticRelay::RegisterAuthHex(tagHex_, cfg.roomSecret);
  reassembler_ = std::make_unique<cosmeticfrag::Reassembler>(
      cfg.maxMessageBytes, cfg.maxReassemblyPerPeer, cfg.reassemblyTtlMs);
  open_ = true;
  // Register at once, so the relay knows our address before our first data
  // datagram reaches it.
  SendRegister();
  return true;
}

void CosmeticRelayTransport::Close() {
  sock_.Close();
  std::lock_guard<std::mutex> lk(mutex_);
  open_ = false;
  lastRegisterMs_ = 0;
  if (reassembler_) reassembler_->Clear();
}

u16 CosmeticRelayTransport::localPort() const { return sock_.localPort(); }

bool CosmeticRelayTransport::SendRegister() {
  if (!open_) return false;
  std::string dg;
  if (!relaywire::BuildRegister(tagHex_, authHex_, cfg_.mtu, &dg)) return false;
  if (!sock_.SendTo(dg.data(), dg.size(), cfg_.relayHost.c_str(),
                    cfg_.relayPort)) {
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  stats_.registrationsSent++;
  stats_.datagramsSent++;
  lastRegisterMs_ = clock_();
  return true;
}

bool CosmeticRelayTransport::WrapData(const std::string& inner,
                                      std::string* out) const {
  return relaywire::BuildData(tagHex_, inner, cfg_.mtu, out);
}

bool CosmeticRelayTransport::Send(const std::string& message) {
  if (!open_) return false;
  // A relay fragment datagram must fit inside the relay envelope, so the
  // fragment payload budget is the mtu minus both headers.
  const size_t innerBudget = cfg_.mtu - relaywire::kEnvelopeBytes;
  const size_t payloadPer = innerBudget - kHeaderBytes;
  if (payloadPer == 0) return false;
  const u16 msgId = msgSeq_++;
  std::vector<u8> datagram(innerBudget);
  const bool ok = cosmeticfrag::Fragment(
      message, payloadPer, cfg_.maxMessageBytes,
      [&](u16 index, u16 count, u32 totalLen, u16 per, size_t off,
          size_t chunk) {
        cosmeticfrag::FillHeader(&datagram[0], cosmeticfrag::kKindMessage, msgId,
                                 index, count, totalLen, per, 0 /* tag unused */);
        std::memcpy(&datagram[kHeaderBytes], message.data() + off, chunk);
        const std::string inner((const char*)&datagram[0],
                                kHeaderBytes + chunk);
        std::string dg;
        if (!WrapData(inner, &dg)) return false;
        if (!sock_.SendTo(dg.data(), dg.size(), cfg_.relayHost.c_str(),
                          cfg_.relayPort)) {
          Log::Warn("cosmetic relay: sendto failed: %s",
                    sock_.LastError().c_str());
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

void CosmeticRelayTransport::Poll(const MessageCb& onMessage, int budgetMs) {
  if (!open_) return;
  if (budgetMs > 0) {
    // Re-register on the interval, so a relay that restarted still finds us.
    u64 last = 0;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      last = lastRegisterMs_;
    }
    if (last == 0 || clock_() - last >= cfg_.registerIntervalMs) {
      SendRegister();
    }
  }

  std::vector<u8> buf(cfg_.mtu + 1);
  const u64 deadline = clock_() + (u64)std::max(0, budgetMs);
  for (;;) {
    const u64 now = clock_();
    const int remaining = now >= deadline ? 0 : (int)(deadline - now);
    std::string host;
    u16 port = 0;
    const ssize_t n = sock_.RecvFrom(buf.data(), buf.size(), &host, &port,
                                     budgetMs > 0 ? remaining : 0);
    if (n <= 0) break;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsReceived++;
    }
    // Only a data envelope from the configured relay carries a message. A
    // register/envelope from anywhere else is dropped.
    if (!relaywire::HasPrefix((const char*)buf.data(), (size_t)n)) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    if (host != cfg_.relayHost) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    if ((u8)buf[5] != relaywire::kKindData) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    const size_t off = relaywire::kEnvelopeBytes;
    const size_t innerLen = (size_t)n - off;
    cosmeticfrag::Complete c;
    bool dropped = false;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      const cosmeticfrag::AddResult r =
          reassembler_->Add(host, port, buf.data() + off, innerLen, clock_(), &c,
                            &dropped);
      if (r == cosmeticfrag::AddResult::Dropped) {
        stats_.fragmentsDropped++;
        if (dropped) stats_.messagesDropped++;
        continue;
      }
      if (r == cosmeticfrag::AddResult::Incomplete) continue;
      stats_.messagesReceived++;
    }
    // Invoked outside the lock, so a callback that re-enters cannot deadlock.
    if (onMessage) onMessage(c.message, c.host, c.port);
    if (now >= deadline) break;
  }

  // Expire partials past their TTL even when no datagram arrived.
  std::lock_guard<std::mutex> lk(mutex_);
  stats_.messagesDropped += reassembler_->ExpireAll(clock_());
}

CosmeticRelayTransport::Stats CosmeticRelayTransport::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

void CosmeticRelayTransport::SetClock(std::function<u64()> nowMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  clock_ = nowMs ? std::move(nowMs) : std::function<u64()>(&SteadyNowMs);
}

}  // namespace Berry
