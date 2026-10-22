// Cosmetic sync channel: signed loadout envelopes. See
// include/berry/cosmetic_sync.h for the security rules.
#include "berry/cosmetic_sync.h"
#include "berry/logger.h"
#include "berry/manifest.h"

#include <algorithm>
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
  const CosmeticSyncResult r = Verify(message, &msg);
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
