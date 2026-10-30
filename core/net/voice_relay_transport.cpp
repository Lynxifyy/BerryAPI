// Voice transport over a UDP relay. See include/berry/voice_relay_transport.h
// for the design and the room envelope.
#include "berry/voice_relay_transport.h"

#include "berry/event.h"
#include "berry/logger.h"
#include "berry/voice_relay.h"  // VoiceRelay::RoomTagHex / RegisterAuthHex
#include "voice_wire.h"         // voicewire:: / relaywire:: shared framing

#include <algorithm>
#include <cstring>

namespace Berry {

// The public header declares the wire sizes; the shared framing headers define
// them. Assert they agree, so a change to one without the other fails to build.
static_assert(VoiceRelayTransport::kVersion == relaywire::kVersion, "version");
static_assert(VoiceRelayTransport::kKindRegister == relaywire::kKindRegister,
              "reg kind");
static_assert(VoiceRelayTransport::kKindData == relaywire::kKindData,
              "data kind");
static_assert(VoiceRelayTransport::kTagBytes == relaywire::kTagBytes, "tag");
static_assert(VoiceRelayTransport::kAuthBytes == relaywire::kAuthBytes, "auth");
static_assert(VoiceRelayTransport::kEnvelopeBytes == relaywire::kEnvelopeBytes,
              "envelope");
static_assert(VoiceRelayTransport::kMinSecretBytes ==
                  relaywire::kMinSecretBytes,
              "min secret");

const char* VoiceRelayTransport::Magic() {
  static char m[5] = {relaywire::kMagic[0], relaywire::kMagic[1],
                      relaywire::kMagic[2], relaywire::kMagic[3], '\0'};
  return m;
}

VoiceRelayTransport::VoiceRelayTransport() = default;
VoiceRelayTransport::~VoiceRelayTransport() { Close(); }

bool VoiceRelayTransport::Open(const RelayVoiceConfig& cfg,
                               const VoiceIdentity& identity,
                               const VoiceTrustAnchor& anchor) {
  Close();
  if (cfg.mtu <= relaywire::kEnvelopeBytes + voicewire::kSealedHeaderBytes +
                     voicewire::kMaxIdBytes + voicewire::kAeadTagBytes) {
    Log::Warn("voice relay: mtu too small for the envelope and the frame header");
    return false;
  }
  if (cfg.room.empty() || cfg.roomSecret.size() < kMinSecretBytes) {
    Log::Warn("voice relay: room secret must be at least %zu bytes",
              kMinSecretBytes);
    return false;
  }
  if (cfg.relayHost.empty()) {
    Log::Warn("voice relay: no relay host");
    return false;
  }
  // Configure the channel before binding, so a bad identity/anchor fails without
  // a half-open socket. Fail closed: no plaintext fallback.
  if (!channel_.Configure(identity, anchor)) {
    Log::Warn("voice relay: secure channel refused the identity or trust anchor");
    return false;
  }
  if (!sock_.Bind(cfg.bindPort, cfg.bindAddr.c_str(), /*broadcast=*/false)) {
    Log::Warn("voice relay: bind failed: %s", sock_.LastError().c_str());
    return false;
  }
  cfg_ = cfg;
  tagHex_ = VoiceRelay::RoomTagHex(cfg.room, cfg.roomSecret);
  authHex_ = VoiceRelay::RegisterAuthHex(tagHex_, cfg.roomSecret);
  open_ = true;
  // Register, then announce, so the relay knows our address before our announce
  // reaches it and a peer that is already up learns our ephemeral key at once.
  SendRegister();
  AnnounceNow();
  return true;
}

void VoiceRelayTransport::Close() {
  sock_.Close();
  std::lock_guard<std::mutex> lk(mutex_);
  open_ = false;
  lastAnnounceMs_ = 0;
  sendSeq_.clear();
  channel_.ClearSessions();
}

u16 VoiceRelayTransport::localPort() const { return sock_.localPort(); }

bool VoiceRelayTransport::SendRegister() {
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
  return true;
}

bool VoiceRelayTransport::WrapData(const std::string& inner,
                                   std::string* out) const {
  return relaywire::BuildData(tagHex_, inner, cfg_.mtu, out);
}

bool VoiceRelayTransport::SendToRelay(const std::string& inner) {
  std::string dg;
  if (!WrapData(inner, &dg)) return false;
  if (!sock_.SendTo(dg.data(), dg.size(), cfg_.relayHost.c_str(),
                    cfg_.relayPort)) {
    return false;
  }
  return true;
}

bool VoiceRelayTransport::AnnounceNow() {
  if (!open_) return false;
  const std::string& ann = channel_.announce();
  std::string inner;
  if (!voicewire::BuildAnnounce(ann, cfg_.mtu, &inner)) return false;
  if (!SendToRelay(inner)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  stats_.announcesSent++;
  stats_.datagramsSent++;
  lastAnnounceMs_ = NowMs();
  return true;
}

bool VoiceRelayTransport::SendFrame(const std::string& speaker, u32 /*seq*/,
                                    const std::vector<u8>& adpcm) {
  if (!open_) return false;
  std::vector<VoiceSession*> sessions = channel_.allSessions();
  if (sessions.empty()) return false;  // no authenticated peer yet
  const std::string routeTag = channel_.ourRouteTag();
  if (routeTag.empty()) return false;

  bool sentAny = false;
  for (VoiceSession* s : sessions) {
    if (s == nullptr) continue;
    // The AEAD counter is per-session and strictly monotonic; the service's u32
    // sequence wraps and is shared, so it is not used for the counter.
    u64 counter = 0;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      counter = ++sendSeq_[routeTag];
    }
    std::string inner;
    if (!voicewire::BuildSealed(*s, routeTag, speaker, counter, adpcm, cfg_.mtu,
                                &inner)) {
      continue;
    }
    if (!SendToRelay(inner)) continue;
    sentAny = true;
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.datagramsSent++;
    stats_.framesSent++;
  }
  return sentAny;
}

void VoiceRelayTransport::Poll(const FrameCb& onFrame, int budgetMs) {
  if (!open_) return;
  if (budgetMs > 0) {
    // Re-register and re-announce on the interval, so a relay that restarted
    // (and a late peer) still finds us.
    u64 last = 0;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      last = lastAnnounceMs_;
    }
    if (last == 0 || NowMs() - last >= cfg_.announceIntervalMs) {
      SendRegister();
      AnnounceNow();
    }
  }

  std::vector<char> buf(cfg_.mtu + 1);
  const u64 deadline = NowMs() + (u64)std::max(0, budgetMs);
  for (;;) {
    std::string host;
    u16 port = 0;
    const int remaining = (int)(deadline > NowMs() ? deadline - NowMs() : 0);
    const ssize_t n = sock_.RecvFrom(buf.data(), buf.size(), &host, &port,
                                     budgetMs > 0 ? remaining : 0);
    if (n <= 0) break;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsReceived++;
    }
    // Only a datagram from the relay is trusted, and only a data envelope
    // carries voice. A register/envelope from anywhere else is dropped.
    if (!relaywire::HasPrefix(buf.data(), (size_t)n)) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    if (host != cfg_.relayHost) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    if ((u8)buf[5] != kKindData) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    const size_t off = relaywire::kEnvelopeBytes;
    const voicewire::Incoming in =
        voicewire::Decode(buf.data() + off, (size_t)n - off, channel_);
    if (in.kind == voicewire::Incoming::Kind::Announce) {
      std::lock_guard<std::mutex> lk(mutex_);
      if (in.accepted) stats_.announcesAccepted++;
      else stats_.announcesRejected++;
      continue;
    }
    if (in.kind == voicewire::Incoming::Kind::Frame && in.accepted) {
      {
        std::lock_guard<std::mutex> lk(mutex_);
        stats_.framesReceived++;
      }
      std::vector<u8> adpcm(in.plaintext.begin(), in.plaintext.end());
      if (onFrame) {
        onFrame(in.speaker, (u32)(in.seq & 0xffffffffu), adpcm, host, port);
      }
      continue;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.datagramsDropped++;
  }
}

VoiceRelayTransport::Stats VoiceRelayTransport::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

}  // namespace Berry
