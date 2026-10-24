// Authenticated, encrypted LAN transport for the proximity voice channel.
// See include/berry/voice_secure_transport.h for the wire format and the
// threat model.
#include "berry/voice_secure_transport.h"

#include "berry/event.h"
#include "berry/logger.h"

#include <algorithm>
#include <cstring>

namespace Berry {

namespace {

constexpr char kMagic[4] = {'B', 'V', 'X', 'S'};

void PutU16(std::string* s, u16 v) {
  s->push_back((char)(v >> 8));
  s->push_back((char)v);
}

u16 GetU16(const char* p) {
  return (u16)(((u16)(u8)p[0] << 8) | (u8)p[1]);
}

void PutU64(std::string* s, u64 v) {
  for (int i = 0; i < 8; ++i) s->push_back((char)(u8)(v >> (8 * (7 - i))));
}

u64 GetU64(const char* p) {
  u64 v = 0;
  for (int i = 0; i < 8; ++i) v = (v << 8) | (u8)p[i];
  return v;
}

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
  if (ann.empty()) return false;
  std::string dg;
  dg.append(kMagic, 4);
  dg.push_back((char)kVersion);
  dg.push_back((char)kKindAnnounce);
  dg.append(ann);
  if (dg.size() > cfg_.mtu) return false;
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
  if (out == nullptr) return false;
  if (routeTag.size() != kRouteTagBytes) return false;
  if (speaker.empty() || speaker.size() > kMaxIdBytes) return false;
  if (adpcm.empty()) return false;

  std::string header;
  header.append(kMagic, 4);
  header.push_back((char)kVersion);
  header.push_back((char)kKindSealedFrame);
  header.append(routeTag);
  PutU64(&header, seq);
  PutU16(&header, (u16)speaker.size());
  header.push_back(0);   // reserved
  header.append(speaker);

  std::string sealed;
  if (!const_cast<VoiceSession&>(session).Seal(
          seq, header, std::string((const char*)adpcm.data(), adpcm.size()),
          &sealed)) {
    return false;
  }
  if (header.size() + sealed.size() > cfg_.mtu) return false;
  header.append(sealed);
  *out = std::move(header);
  return true;
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
    if (n < (ssize_t)kPrefixBytes) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    if (std::memcmp(buf.data(), kMagic, 4) != 0 || (u8)buf[4] != kVersion) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    const u8 kind = (u8)buf[5];
    if (kind == kKindAnnounce) {
      const std::string ann(buf.data() + kPrefixBytes, (size_t)n - kPrefixBytes);
      const std::string fp = channel_.AcceptAnnounce(ann);
      std::lock_guard<std::mutex> lk(mutex_);
      if (fp.empty()) {
        stats_.announcesRejected++;
      } else {
        stats_.announcesAccepted++;
      }
    } else if (kind == kKindSealedFrame) {
      if ((size_t)n < kSealedHeaderBytes + 1) {
        std::lock_guard<std::mutex> lk(mutex_);
        stats_.datagramsDropped++;
        continue;
      }
      const std::string routeTag(buf.data() + kPrefixBytes, kRouteTagBytes);
      const u64 seq = GetU64(buf.data() + kPrefixBytes + kRouteTagBytes);
      const size_t idLen = GetU16(buf.data() + kPrefixBytes + kRouteTagBytes + 8);
      if (idLen == 0 || idLen > kMaxIdBytes ||
          (size_t)n < kSealedHeaderBytes + idLen + 16) {
        std::lock_guard<std::mutex> lk(mutex_);
        stats_.datagramsDropped++;
        continue;
      }
      const size_t idOff = kSealedHeaderBytes;
      const std::string speaker(buf.data() + idOff, idLen);
      // The AAD is the header exactly as it arrived, up to and including the id.
      const std::string aad(buf.data(), idOff + idLen);
      const std::string sealed(buf.data() + idOff + idLen,
                               (size_t)n - idOff - idLen);

      VoiceSession* session = channel_.SessionByRoute(routeTag);
      if (session == nullptr) {
        std::lock_guard<std::mutex> lk(mutex_);
        stats_.datagramsDropped++;
        continue;
      }
      std::string plain;
      if (!session->Open(seq, aad, sealed, &plain) || plain.empty()) {
        std::lock_guard<std::mutex> lk(mutex_);
        stats_.datagramsDropped++;
        continue;
      }
      std::vector<u8> adpcm(plain.begin(), plain.end());
      {
        std::lock_guard<std::mutex> lk(mutex_);
        stats_.framesReceived++;
      }
      // Invoked outside the lock, so a callback that re-enters cannot deadlock.
      if (onFrame) onFrame(speaker, (u32)(seq & 0xffffffffu), adpcm, host, port);
    } else {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
    }
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
