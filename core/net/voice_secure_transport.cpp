// Authenticated, encrypted LAN transport for the proximity voice channel.
// See include/berry/voice_secure_transport.h for the wire format and the
// threat model.
#include "berry/voice_secure_transport.h"

#include "berry/event.h"
#include "berry/logger.h"
#include "multicast_lock.h"
#include "voice_wire.h"

#include <algorithm>
#include <cstring>

namespace Berry {

// The public header declares the wire sizes; the shared framing header defines
// them. Assert they agree, so a change to one without the other fails to build
// rather than putting two incompatible formats on the same group.
static_assert(SecureVoiceTransport::kVersion == voicewire::kVersion, "version");
static_assert(SecureVoiceTransport::kKindAnnounce == voicewire::kKindAnnounce,
              "announce kind");
static_assert(SecureVoiceTransport::kKindSealedFrame ==
                  voicewire::kKindSealedFrame,
              "sealed kind");
static_assert(SecureVoiceTransport::kPrefixBytes == voicewire::kPrefixBytes,
              "prefix size");
static_assert(SecureVoiceTransport::kRouteTagBytes == voicewire::kRouteTagBytes,
              "route tag size");
static_assert(SecureVoiceTransport::kSealedHeaderBytes ==
                  voicewire::kSealedHeaderBytes,
              "sealed header size");
static_assert(SecureVoiceTransport::kMaxIdBytes == voicewire::kMaxIdBytes,
              "id cap");

namespace {

constexpr char kMagic[4] = {'B', 'V', 'X', 'S'};

}  // namespace

const char* SecureVoiceTransport::Magic() {
  static char magic[5] = {kMagic[0], kMagic[1], kMagic[2], kMagic[3], '\0'};
  return magic;
}

SecureVoiceTransport::SecureVoiceTransport() = default;
SecureVoiceTransport::~SecureVoiceTransport() { Close(); }

bool SecureVoiceTransport::Open(const SecureVoiceConfig& cfg,
                                const VoiceIdentity& identity,
                                const VoiceTrustAnchor& anchor) {
  Close();
  if (cfg.mtu <= kSealedHeaderBytes + kMaxIdBytes + 16) {
    Log::Warn("voice: secure mtu too small for the frame header");
    return false;
  }
  // Configure the channel before binding, so a bad identity/anchor fails without
  // a half-open socket. Fail closed: no plaintext fallback.
  if (!channel_.Configure(identity, anchor)) {
    Log::Warn("voice: secure channel refused the identity or trust anchor");
    return false;
  }
  if (!sock_.Bind(cfg.port, "0.0.0.0", /*broadcast=*/true)) {
    Log::Warn("voice: secure bind failed: %s", sock_.LastError().c_str());
    return false;
  }
  cfg_ = cfg;
  open_ = true;
  // On a device the Wi-Fi power management drops multicast to a joined group
  // unless we hold a multicast lock, so acquire one *before* joining. It is
  // best-effort: a failed acquire leaves the socket usable for unicast and is
  // reported by `multicastJoined`, never a reason to refuse to open.
  mcastLock_ = std::make_unique<MulticastLock>();
  mcastLock_->Acquire("BerryAPI voice");
  joined_ = sock_.JoinMulticast(cfg.group.c_str(), cfg.iface.c_str());
  if (!joined_) {
    Log::Warn("voice: secure multicast join failed: %s",
              sock_.LastError().c_str());
  } else if (cfg.iface != "0.0.0.0") {
    sock_.SetMulticastInterface(cfg.iface.c_str());
  }
  // Put our announce out immediately so a peer that is already up learns our
  // ephemeral key without waiting a full interval.
  AnnounceNow();
  return true;
}

void SecureVoiceTransport::Close() {
  sock_.Close();
  // Drop the multicast lock with the socket: leaving Wi-Fi multicast on after
  // the channel is gone would keep the radio awake for nothing.
  if (mcastLock_) {
    mcastLock_->Release();
    mcastLock_.reset();
  }
  std::lock_guard<std::mutex> lk(mutex_);
  open_ = false;
  joined_ = false;
  lastAnnounceMs_ = 0;
  sendSeq_.clear();
  channel_.ClearSessions();
}

u16 SecureVoiceTransport::localPort() const { return sock_.localPort(); }

bool SecureVoiceTransport::AnnounceNow() {
  if (!open_) return false;
  const std::string& ann = channel_.announce();
  std::string dg;
  if (!voicewire::BuildAnnounce(ann, cfg_.mtu, &dg)) return false;
  if (!sock_.SendTo(dg.data(), dg.size(), cfg_.group.c_str(), cfg_.port)) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.announcesSent++;
    stats_.datagramsSent++;
    lastAnnounceMs_ = NowMs();
  }
  return true;
}

bool SecureVoiceTransport::BuildSealed(const VoiceSession& session,
                                       const std::string& routeTag,
                                       const std::string& speaker, u64 seq,
                                       const std::vector<u8>& adpcm,
                                       std::string* out) const {
  // The framing is shared with the relay transport (voice_wire.h), so the two
  // can never drift onto incompatible formats. Seal() is not const, so the
  // const session reference is cast away — the caller owns the session and has
  // already serialised access.
  return voicewire::BuildSealed(const_cast<VoiceSession&>(session), routeTag,
                                speaker, seq, adpcm, cfg_.mtu, out);
}

bool SecureVoiceTransport::SendFrame(const std::string& speaker, u32 /*seq*/,
                                     const std::vector<u8>& adpcm) {
  if (!open_) return false;
  std::vector<VoiceSession*> sessions = channel_.allSessions();
  if (sessions.empty()) return false;   // no authenticated peer yet: send nothing
  const std::string routeTag = channel_.ourRouteTag();
  if (routeTag.empty()) return false;

  bool sentAny = false;
  for (VoiceSession* s : sessions) {
    if (s == nullptr) continue;
    // The AEAD counter must be strictly monotonic per session and is stamped
    // into the header (which is also the AAD), so a receiver's replay window and
    // the sender's nonce agree. The service's u32 sequence is not used for the
    // counter: it wraps and is shared across peers, while the counter is
    // per-session and must never repeat under a key.
    u64 counter = 0;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      counter = ++sendSeq_[routeTag];
    }
    std::string dg;
    if (!BuildSealed(*s, routeTag, speaker, counter, adpcm, &dg)) continue;
    if (!sock_.SendTo(dg.data(), dg.size(), cfg_.group.c_str(), cfg_.port)) {
      continue;
    }
    sentAny = true;
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.datagramsSent++;
    stats_.framesSent++;
  }
  return sentAny;
}

void SecureVoiceTransport::Poll(const FrameCb& onFrame, int budgetMs) {
  if (!open_) return;
  if (budgetMs > 0) {
    // Re-announce on the interval so a late joiner still finds us.
    u64 last = 0;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      last = lastAnnounceMs_;
    }
    if (last == 0 || NowMs() - last >= cfg_.announceIntervalMs) AnnounceNow();
  }

  std::vector<char> buf(cfg_.mtu);
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
    // The framing is shared with the relay transport (voice_wire.h), so both
    // parse a datagram identically.
    const voicewire::Incoming in =
        voicewire::Decode(buf.data(), (size_t)n, channel_);
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
      // Invoked outside the lock, so a callback that re-enters cannot deadlock.
      std::vector<u8> adpcm(in.plaintext.begin(), in.plaintext.end());
      if (onFrame) {
        onFrame(in.speaker, (u32)(in.seq & 0xffffffffu), adpcm, host, port);
      }
      continue;
    }
    // Anything else (not ours, malformed, unknown route, bad tag, replay) is
    // dropped; the client stays open.
    std::lock_guard<std::mutex> lk(mutex_);
    stats_.datagramsDropped++;
    // Loop until the socket is empty (a 0-ms read returns 0) or the budget is
    // spent, so a frame drains every queued datagram: an announce from a peer
    // that just joined is not starved behind a burst of audio.
  }
}

SecureVoiceTransport::Stats SecureVoiceTransport::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

}  // namespace Berry
