#include "berry/voice_transport.h"

#include "berry/logger.h"
#include "berry/event.h"

#include <algorithm>
#include <cstring>

namespace Berry {

namespace {

const char kMagic[4] = {'B', 'V', 'O', 'X'};
constexpr u8 kVersion = 1;
constexpr u8 kKindVoice = 1;

void PutU16(char* p, u16 v) {
  p[0] = (char)(v >> 8);
  p[1] = (char)v;
}

u16 GetU16(const char* p) {
  return (u16)(((u16)(u8)p[0] << 8) | (u8)p[1]);
}

}  // namespace

const char* LanVoiceTransport::Magic() { return kMagic; }

LanVoiceTransport::LanVoiceTransport() = default;
LanVoiceTransport::~LanVoiceTransport() { Close(); }

bool LanVoiceTransport::Open(const VoiceTransportConfig& cfg) {
  Close();
  if (cfg.mtu <= kHeaderBytes + kMaxIdBytes) {
    Log::Warn("voice: mtu too small for the frame header");
    return false;
  }
  if (!sock_.Bind(cfg.port, "0.0.0.0", /*broadcast=*/true)) {
    Log::Warn("voice: bind failed: %s", sock_.LastError().c_str());
    return false;
  }
  cfg_ = cfg;
  open_ = true;
  // A failed group join is not fatal: a unicast-only socket still works between
  // two clients that know each other's address, and the caller can see why.
  joined_ = sock_.JoinMulticast(cfg.group.c_str(), cfg.iface.c_str());
  if (!joined_) {
    Log::Warn("voice: multicast join failed: %s", sock_.LastError().c_str());
  } else if (cfg.iface != "0.0.0.0") {
    sock_.SetMulticastInterface(cfg.iface.c_str());
  }
  return true;
}

void LanVoiceTransport::Close() {
  sock_.Close();
  std::lock_guard<std::mutex> lk(mutex_);
  open_ = false;
  joined_ = false;
}

u16 LanVoiceTransport::localPort() const { return sock_.localPort(); }

bool LanVoiceTransport::BuildDatagram(const std::string& speaker, u32 seq,
                                      const std::vector<u8>& adpcm,
                                      std::string* out) const {
  if (!out) return false;
  if (speaker.empty() || speaker.size() > kMaxIdBytes) return false;
  if (adpcm.empty()) return false;
  if (kHeaderBytes + speaker.size() + adpcm.size() > cfg_.mtu) return false;
  out->assign(kHeaderBytes, '\0');
  std::memcpy(&(*out)[0], kMagic, 4);
  (*out)[4] = (char)kVersion;
  (*out)[5] = (char)kKindVoice;
  PutU16(&(*out)[6], (u16)(seq & 0xffff));
  (*out)[8] = (char)(u8)speaker.size();
  (*out)[9] = 0;   // reserved
  out->append(speaker);
  out->append((const char*)adpcm.data(), adpcm.size());
  return true;
}

bool LanVoiceTransport::SendFrame(const std::string& speaker, u32 seq,
                                  const std::vector<u8>& adpcm) {
  if (!open_) return false;
  std::string datagram;
  if (!BuildDatagram(speaker, seq, adpcm, &datagram)) return false;
  if (!sock_.SendTo(datagram.data(), datagram.size(), cfg_.group.c_str(),
                    cfg_.port)) {
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  stats_.datagramsSent++;
  stats_.framesSent++;
  return true;
}

void LanVoiceTransport::Poll(const FrameCb& onFrame, int budgetMs) {
  if (!open_ || !onFrame) return;
  // A frame is one datagram, so the buffer is one MTU. A datagram larger than
  // the buffer is truncated by the socket and then rejected by the size check.
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
    if (n < (ssize_t)(kHeaderBytes + 1)) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    if (std::memcmp(buf.data(), kMagic, 4) != 0 || (u8)buf[4] != kVersion ||
        (u8)buf[5] != kKindVoice) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    const u32 seq = GetU16(&buf[6]);
    const size_t idLen = (u8)buf[8];
    if (idLen == 0 || idLen > kMaxIdBytes ||
        (size_t)n < kHeaderBytes + idLen + 1) {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.datagramsDropped++;
      continue;
    }
    const std::string speaker(&buf[kHeaderBytes], idLen);
    const size_t payloadOff = kHeaderBytes + idLen;
    const size_t payloadLen = (size_t)n - payloadOff;
    std::vector<u8> adpcm((const u8*)&buf[payloadOff],
                          (const u8*)&buf[payloadOff] + payloadLen);
    {
      std::lock_guard<std::mutex> lk(mutex_);
      stats_.framesReceived++;
    }
    // Invoked outside the lock, so a callback that re-enters cannot deadlock.
    onFrame(speaker, seq, adpcm, host, port);
    if (budgetMs <= 0) break;
  }
}

LanVoiceTransport::Stats LanVoiceTransport::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

}  // namespace Berry
