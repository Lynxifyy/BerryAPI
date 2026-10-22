// Cosmetic sync channel: signed loadout envelopes. See
// include/berry/cosmetic_sync.h for the security rules.
#include "berry/cosmetic_sync.h"
#include "berry/logger.h"
#include "berry/manifest.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>

namespace Berry {

namespace {

/// Wire magic + version. A peer that does not present this exact line is
/// refused, so a future format cannot be silently misread as this one.
constexpr const char* kMagicLine = "BERRY-COSMETIC-SYNC 1";

/// The payload always begins with this banner, so a signed blob cannot be
/// replayed as some other kind of BerryAPI message.
constexpr const char* kPayloadBanner = "berry-cosmetic-sync/1";

constexpr size_t kMaxMessageBytes = 64 * 1024;

std::string AlgoFor(CosmeticSyncKey::Kind kind) {
  return kind == CosmeticSyncKey::Kind::Ed25519 ? std::string(signature_algo::Ed25519())
                                                : std::string(signature_algo::HmacSha256());
}

/// The default clock: milliseconds from a steady source. Monotonic, so a
/// wall-clock adjustment cannot move the replay TTL or the rate limiter.
u64 SteadyNowMs() {
  using namespace std::chrono;
  return (u64)duration_cast<milliseconds>(
             steady_clock::now().time_since_epoch())
      .count();
}

/// Reads the value of a "key: value" header line. Returns false when the line
/// is not exactly `key` followed by ": " and a non-empty value.
bool HeaderValue(const std::string& line, const char* key, std::string* out) {
  const std::string prefix = std::string(key) + ": ";
  if (line.size() <= prefix.size()) return false;
  if (line.compare(0, prefix.size(), prefix) != 0) return false;
  *out = line.substr(prefix.size());
  return true;
}

/// Splits a payload into the player id and the loadout body. The payload is
/// "<banner>\nplayer=<id>\n<loadout canonical>".
bool ParsePayload(const std::string& payload, std::string* playerId,
                  std::string* loadoutText) {
  const std::string banner = std::string(kPayloadBanner) + "\n";
  if (payload.compare(0, banner.size(), banner) != 0) return false;
  const size_t p = banner.size();
  const size_t nl = payload.find('\n', p);
  if (nl == std::string::npos) return false;
  const std::string playerLine = payload.substr(p, nl - p);
  const std::string prefix = "player=";
  if (playerLine.compare(0, prefix.size(), prefix) != 0) return false;
  *playerId = playerLine.substr(prefix.size());
  *loadoutText = payload.substr(nl + 1);
  return true;
}

}  // namespace

const char* CosmeticSyncResultName(CosmeticSyncResult r) {
  switch (r) {
    case CosmeticSyncResult::Ok: return "ok";
    case CosmeticSyncResult::EmptyMessage: return "empty message";
    case CosmeticSyncResult::TooLarge: return "message too large";
    case CosmeticSyncResult::BadEnvelope: return "bad envelope";
    case CosmeticSyncResult::UnknownAlgorithm: return "unknown algorithm";
    case CosmeticSyncResult::BadSignature: return "bad signature";
    case CosmeticSyncResult::BadLoadout: return "bad loadout";
    case CosmeticSyncResult::NoTransport: return "no transport";
    case CosmeticSyncResult::NotConfigured: return "no verification key";
    case CosmeticSyncResult::Replayed: return "replayed message";
    case CosmeticSyncResult::RateLimited: return "rate limited";
    case CosmeticSyncResult::Revoked: return "revoked peer";
  }
  return "unknown";
}

const char* CosmeticSyncMagic() { return kMagicLine; }
size_t CosmeticSyncMaxMessageBytes() { return kMaxMessageBytes; }

//----------------------------------------------------------------------------
// Key
//----------------------------------------------------------------------------

CosmeticSyncKey CosmeticSyncKey::Ed25519PublicKey(std::string hex) {
  CosmeticSyncKey k;
  k.kind = Kind::Ed25519;
  k.keyHex = std::move(hex);
  return k;
}

CosmeticSyncKey CosmeticSyncKey::HmacSecret(std::string secret) {
  CosmeticSyncKey k;
  k.kind = Kind::Hmac;
  k.keyHex = std::move(secret);
  return k;
}

bool CosmeticSyncKey::Valid() const {
  if (keyHex.empty()) return false;
  if (kind == Kind::Ed25519) {
    std::string raw;
    return crypto::Ed25519PublicKeyFromHex(keyHex, &raw);
  }
  // HMAC accepts any non-empty secret (local development / self-tests).
  return true;
}

//----------------------------------------------------------------------------
// Payload + framing
//----------------------------------------------------------------------------

std::string CosmeticSyncPayload(const std::string& playerId,
                                const CosmeticLoadout& loadout) {
  std::string s = std::string(kPayloadBanner) + "\n";
  s += "player=" + playerId + "\n";
  s += loadout.Canonical();
  return s;
}

std::string CosmeticSyncFrame(const std::string& playerId,
                              const CosmeticLoadout& loadout,
                              const std::string& algo,
                              const std::string& signatureHex) {
  const std::string payload = CosmeticSyncPayload(playerId, loadout);
  std::ostringstream os;
  os << kMagicLine << '\n';
  os << "algo: " << algo << '\n';
  os << "sig: " << signatureHex << '\n';
  os << "payload: " << payload.size() << '\n';
  os << payload;
  return os.str();
}

bool CosmeticSyncSignEd25519(const std::string& playerId,
                             const CosmeticLoadout& loadout,
                             const std::string& secretKey64,
                             std::string* outMessage) {
  const std::string payload = CosmeticSyncPayload(playerId, loadout);
  std::string sig;
  if (!crypto::Ed25519Sign(payload, secretKey64, &sig)) return false;
  if (outMessage) {
    *outMessage = CosmeticSyncFrame(playerId, loadout, signature_algo::Ed25519(),
                                    crypto::ToHexLower(sig));
  }
  return true;
}

bool CosmeticSyncSignHmac(const std::string& playerId,
                          const CosmeticLoadout& loadout,
                          const std::string& secret,
                          std::string* outMessage) {
  if (secret.empty()) return false;
  const std::string payload = CosmeticSyncPayload(playerId, loadout);
  const std::string sig = crypto::HmacSha256Hex(secret, payload);
  if (outMessage) {
    *outMessage = CosmeticSyncFrame(playerId, loadout,
                                    signature_algo::HmacSha256(), sig);
  }
  return true;
}

//----------------------------------------------------------------------------
// Channel
//----------------------------------------------------------------------------

void CosmeticSyncChannel::SetConfig(const CosmeticSyncConfig& config) {
  std::lock_guard<std::mutex> lk(mutex_);
  config_ = config;
  // Drop state whose meaning depended on the old limits, so a shrunk window
  // takes effect at once rather than after the old entries age out.
  seen_.clear();
  peers_.clear();
}

void CosmeticSyncChannel::SetClock(std::function<u64()> nowMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  clock_ = std::move(nowMs);
}

void CosmeticSyncChannel::SetVerifyKey(const CosmeticSyncKey& key) {
  std::lock_guard<std::mutex> lk(mutex_);
  key_ = key;
  hasKey_ = key.Valid();
}

bool CosmeticSyncChannel::configured() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return hasKey_;
}

void CosmeticSyncChannel::ClearVerifyKey() {
  std::lock_guard<std::mutex> lk(mutex_);
  key_ = CosmeticSyncKey();
  hasKey_ = false;
}

void CosmeticSyncChannel::RevokePeer(const std::string& playerId) {
  if (playerId.empty()) return;
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& p : revokedPeers_) {
    if (p == playerId) return;
  }
  revokedPeers_.push_back(playerId);
}

bool CosmeticSyncChannel::IsPeerRevoked(const std::string& playerId) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (const auto& p : revokedPeers_) {
    if (p == playerId) return true;
  }
  return false;
}

void CosmeticSyncChannel::ClearRevocations() {
  std::lock_guard<std::mutex> lk(mutex_);
  revokedPeers_.clear();
}

size_t CosmeticSyncChannel::trackedPeers() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return peers_.size();
}

size_t CosmeticSyncChannel::replayWindowSize() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return seen_.size();
}

void CosmeticSyncChannel::ExpireReplayLocked(u64 now) {
  // Drop entries older than the TTL. `seen_` is oldest-first, so the expired
  // ones are a prefix.
  const u64 ttl = config_.replayTtlMs;
  size_t drop = 0;
  while (drop < seen_.size() && now - seen_[drop].atMs > ttl) ++drop;
  if (drop) seen_.erase(seen_.begin(), seen_.begin() + (long)drop);
}

CosmeticSyncChannel::PeerState& CosmeticSyncChannel::PeerLocked(
    const std::string& playerId, u64 now) {
  auto it = peers_.find(playerId);
  if (it != peers_.end()) {
    it->second.lastSeen = ++peerClock_;
    return it->second;
  }
  // A new peer needs a slot. When the table is full, evict the
  // least-recently-seen one, so an unbounded flood of distinct ids cannot grow
  // the table without limit.
  if (config_.maxPeers > 0 && peers_.size() >= config_.maxPeers) {
    auto victim = peers_.begin();
    for (auto it2 = peers_.begin(); it2 != peers_.end(); ++it2) {
      if (it2->second.lastSeen < victim->second.lastSeen) victim = it2;
    }
    peers_.erase(victim);
  }
  PeerState st;
  st.tokens = config_.rateBurst;
  st.lastMs = now;
  st.lastSeen = ++peerClock_;
  auto res = peers_.emplace(playerId, st);
  return res.first->second;
}

CosmeticSyncResult CosmeticSyncChannel::AdmitLocked(
    const std::string& playerId, const std::string& message, u64 now) {
  // Revocation first: a revoked peer is refused regardless of anything else.
  for (const auto& p : revokedPeers_) {
    if (p == playerId) return CosmeticSyncResult::Revoked;
  }

  // Rate limit: a per-peer token bucket. Refill by elapsed time, cap at the
  // burst, then spend one token. No token means "too fast".
  PeerState& peer = PeerLocked(playerId, now);
  const u64 elapsed = now >= peer.lastMs ? now - peer.lastMs : 0;
  peer.lastMs = now;
  if (elapsed) {
    peer.tokens = std::min(config_.rateBurst,
                           peer.tokens + (double)elapsed * config_.ratePerSecond / 1000.0);
  }
  if (peer.tokens < 1.0) return CosmeticSyncResult::RateLimited;
  peer.tokens -= 1.0;

  // Replay: refuse a message we have already accepted within the TTL. Check
  // after the rate limit so a replay flood costs the peer its budget rather
  // than filling the window.
  ExpireReplayLocked(now);
  const std::string digest = crypto::Sha256Hex(message);
  for (const auto& s : seen_) {
    if (s.digest == digest) return CosmeticSyncResult::Replayed;
  }
  seen_.push_back(SeenDigest{digest, now});
  // Trim to the window size (oldest first).
  if (config_.replayWindow > 0 && seen_.size() > config_.replayWindow) {
    seen_.erase(seen_.begin(),
                seen_.begin() + (long)(seen_.size() - config_.replayWindow));
  }
  return CosmeticSyncResult::Ok;
}

bool CosmeticSyncChannel::Publish(const std::string& playerId,
                                  const CosmeticLoadout& loadout,
                                  const std::string& secretKey64,
                                  bool useHmac) {
  if (playerId.empty()) return false;
  CosmeticSyncTransport* transport = transport_;
  if (!transport) {
    std::lock_guard<std::mutex> lk(mutex_);
    last_ = CosmeticSyncResult::NoTransport;
    ++rejected_;
    return false;
  }
  std::string message;
  const bool ok = useHmac
                      ? CosmeticSyncSignHmac(playerId, loadout, secretKey64, &message)
                      : CosmeticSyncSignEd25519(playerId, loadout, secretKey64, &message);
  if (!ok) {
    std::lock_guard<std::mutex> lk(mutex_);
    last_ = CosmeticSyncResult::BadSignature;
    ++rejected_;
    return false;
  }
  return transport->Send(message);
}

CosmeticSyncResult CosmeticSyncChannel::Verify(const std::string& message,
                                               CosmeticSyncMessage* out) const {
  if (message.empty()) return CosmeticSyncResult::EmptyMessage;
  if (message.size() > kMaxMessageBytes) return CosmeticSyncResult::TooLarge;

  CosmeticSyncKey key;
  bool hasKey = false;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    key = key_;
    hasKey = hasKey_;
  }
  if (!hasKey) return CosmeticSyncResult::NotConfigured;

  // Header lines, then the payload.
  const size_t firstNl = message.find('\n');
  if (firstNl == std::string::npos) return CosmeticSyncResult::BadEnvelope;
  if (message.compare(0, firstNl, kMagicLine) != 0) {
    return CosmeticSyncResult::BadEnvelope;
  }

  std::string algo, sigHex, payloadLenText;
  size_t pos = firstNl + 1;
  bool sawPayload = false;
  size_t payloadStart = 0;
  while (pos < message.size()) {
    const size_t nl = message.find('\n', pos);
    if (nl == std::string::npos) return CosmeticSyncResult::BadEnvelope;
    const std::string line = message.substr(pos, nl - pos);
    if (HeaderValue(line, "algo", &algo)) {
      // ok
    } else if (HeaderValue(line, "sig", &sigHex)) {
      // ok
    } else if (HeaderValue(line, "payload", &payloadLenText)) {
      payloadStart = nl + 1;
      sawPayload = true;
      break;
    } else {
      return CosmeticSyncResult::BadEnvelope;
    }
    pos = nl + 1;
  }
  if (!sawPayload || algo.empty() || sigHex.empty() || payloadLenText.empty()) {
    return CosmeticSyncResult::BadEnvelope;
  }

  size_t payloadLen = 0;
  try {
    payloadLen = (size_t)std::stoull(payloadLenText);
  } catch (...) {
    return CosmeticSyncResult::BadEnvelope;
  }
  if (payloadStart + payloadLen != message.size()) {
    return CosmeticSyncResult::BadEnvelope;
  }
  const std::string payload = message.substr(payloadStart, payloadLen);

  // The algorithm must be one we know and must match the key's mode, so an
  // HMAC frame cannot be presented to an Ed25519 verifier.
  const std::string expectedAlgo = AlgoFor(key.kind);
  if (algo != expectedAlgo) {
    return algo == signature_algo::Ed25519() ||
                   algo == signature_algo::HmacSha256()
               ? CosmeticSyncResult::BadSignature
               : CosmeticSyncResult::UnknownAlgorithm;
  }

  // Verify the signature over the exact payload bytes.
  if (key.kind == CosmeticSyncKey::Kind::Ed25519) {
    std::string pub32, sig64;
    if (!crypto::Ed25519PublicKeyFromHex(key.keyHex, &pub32)) {
      return CosmeticSyncResult::BadSignature;
    }
    if (!crypto::FromHex(sigHex, &sig64, 64)) {
      return CosmeticSyncResult::BadSignature;
    }
    if (!crypto::Ed25519Verify(payload, sig64, pub32)) {
      return CosmeticSyncResult::BadSignature;
    }
  } else {
    const std::string want = crypto::HmacSha256Hex(key.keyHex, payload);
    if (!crypto::ConstantTimeEquals(want, sigHex)) {
      return CosmeticSyncResult::BadSignature;
    }
  }

  // The payload must be a canonical (player, loadout) pair: re-deriving it and
  // comparing catches a signature over some other byte string.
  std::string playerId, loadoutText;
  if (!ParsePayload(payload, &playerId, &loadoutText)) {
    return CosmeticSyncResult::BadEnvelope;
  }
  CosmeticLoadout loadout;
  if (!loadout.FromCanonical(loadoutText)) return CosmeticSyncResult::BadLoadout;
  if (CosmeticSyncPayload(playerId, loadout) != payload) {
    return CosmeticSyncResult::BadEnvelope;
  }

  if (out) {
    out->playerId = playerId;
    out->loadout = loadout;
    out->algo = algo;
  }
  return CosmeticSyncResult::Ok;
}

CosmeticSyncResult CosmeticSyncChannel::Receive(const std::string& message,
                                                CosmeticService* service,
                                                size_t* applied) {
  CosmeticSyncMessage msg;
  const CosmeticSyncResult vr = Verify(message, &msg);

  // Post-signature defences, applied under the lock so the replay window and
  // the rate limiter see a consistent order. Only a message whose signature
  // already verified can consume a token or enter the window, so an
  // unauthenticated flood cannot starve a real peer.
  CosmeticSyncResult r = vr;
  if (vr == CosmeticSyncResult::Ok) {
    std::function<u64()> clock;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      clock = clock_;
    }
    const u64 now = clock ? clock() : SteadyNowMs();
    std::lock_guard<std::mutex> lk(mutex_);
    r = AdmitLocked(msg.playerId, message, now);
  }

  {
    std::lock_guard<std::mutex> lk(mutex_);
    last_ = r;
    if (r == CosmeticSyncResult::Ok) {
      ++accepted_;
    } else {
      ++rejected_;
    }
  }
  if (r != CosmeticSyncResult::Ok) return r;
  size_t n = 0;
  if (service) n = service->SetRemoteLoadout(msg.playerId, msg.loadout);
  if (applied) *applied = n;
  return r;
}

u64 CosmeticSyncChannel::accepted() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return accepted_;
}

u64 CosmeticSyncChannel::rejected() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return rejected_;
}

CosmeticSyncResult CosmeticSyncChannel::lastResult() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return last_;
}

}  // namespace Berry
