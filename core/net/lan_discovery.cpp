// mDNS discovery for the LAN cosmetic transport. See
// include/berry/lan_discovery.h for the scope.
#include "berry/lan_discovery.h"
#include "berry/logger.h"

#include <algorithm>
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace Berry {

namespace {

constexpr u16 kTypeA = 1;
constexpr u16 kTypePTR = 12;
constexpr u16 kTypeTXT = 16;
constexpr u16 kTypeSRV = 33;

u64 SteadyNowMs() {
  using namespace std::chrono;
  return (u64)duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
      .count();
}

u16 GetU16(const u8* p) { return (u16)(((u16)p[0] << 8) | p[1]); }
void PutU16(std::string* s, u16 v) {
  s->push_back((char)(v >> 8));
  s->push_back((char)v);
}
void PutU32(std::string* s, u32 v) {
  s->push_back((char)(v >> 24));
  s->push_back((char)(v >> 16));
  s->push_back((char)(v >> 8));
  s->push_back((char)v);
}

/// Appends a DNS name in label form. No compression on send: a reader must
/// still follow pointers (other responders compress), but a writer that never
/// emits one cannot be misread.
void EncodeName(std::string* out, const std::string& name) {
  size_t start = 0;
  while (start < name.size()) {
    const size_t dot = name.find('.', start);
    const std::string label =
        dot == std::string::npos ? name.substr(start) : name.substr(start, dot - start);
    // A label is at most 63 bytes; a longer one is split at the boundary rather
    // than truncated (mDNS names are short, so this is a guard, not a path).
    size_t off = 0;
    while (off < label.size()) {
      const size_t n = std::min<size_t>(63, label.size() - off);
      out->push_back((char)n);
      out->append(label, off, n);
      off += n;
    }
    if (dot == std::string::npos) break;
    start = dot + 1;
  }
  out->push_back('\0');
}

/// Decodes a DNS name at `*off`, following compression pointers (bounded, so a
/// pointer loop cannot hang the reader). Advances `*off` past the name in the
/// packet (not past a followed pointer). False on any malformed input.
bool DecodeName(const u8* data, size_t len, size_t* off, std::string* out) {
  size_t pos = *off;
  bool jumped = false;
  int jumps = 0;
  std::string name;
  for (;;) {
    if (pos >= len) return false;
    const u8 l = data[pos];
    if (l == 0) {
      if (!jumped) *off = pos + 1;
      break;
    }
    if ((l & 0xc0) == 0xc0) {
      if (pos + 1 >= len) return false;
      const size_t ptr = ((size_t)(l & 0x3f) << 8) | data[pos + 1];
      if (ptr >= len) return false;
      if (!jumped) *off = pos + 2;
      jumped = true;
      if (++jumps > 64) return false;
      pos = ptr;
      continue;
    }
    if ((l & 0xc0) != 0) return false;   // reserved label type
    if (pos + 1 + l > len) return false;
    if (!name.empty()) name += '.';
    name.append((const char*)data + pos + 1, l);
    pos += 1 + l;
  }
  *out = name;
  return true;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
  return s.size() >= suffix.size() &&
         s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

/// The local address to advertise: the configured interface, or the address the
/// system would route to the group from.
std::string LocalAddressFor(const std::string& group, u16 port,
                            const std::string& iface) {
  if (iface != "0.0.0.0") return iface;
  int s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s < 0) return "0.0.0.0";
  sockaddr_in d{};
  d.sin_family = AF_INET;
  d.sin_port = htons(port);
  ::inet_pton(AF_INET, group.c_str(), &d.sin_addr);
  std::string out = "0.0.0.0";
  if (::connect(s, (sockaddr*)&d, sizeof d) == 0) {
    sockaddr_in l{};
    socklen_t ll = sizeof l;
    if (::getsockname(s, (sockaddr*)&l, &ll) == 0) {
      char ip[INET_ADDRSTRLEN] = {0};
      ::inet_ntop(AF_INET, &l.sin_addr, ip, sizeof ip);
      out = ip;
    }
  }
  ::close(s);
  return out;
}

}  // namespace

LanDiscovery::LanDiscovery() { clock_ = &SteadyNowMs; }
LanDiscovery::~LanDiscovery() { Close(); }

std::string LanDiscovery::FullInstanceName(const std::string& instance) const {
  // A short name gets the service type appended; a name that already carries it
  // is left alone, so a caller can pass either.
  if (EndsWith(instance, cfg_.serviceType)) return instance;
  return instance + "." + cfg_.serviceType;
}

bool LanDiscovery::Open(const LanDiscoveryConfig& cfg) {
  Close();
  if (!sock_.Bind(cfg.port, "0.0.0.0", /*broadcast=*/false)) {
    Log::Warn("lan discovery: bind failed: %s", sock_.LastError().c_str());
    return false;
  }
  cfg_ = cfg;
  open_ = true;
  joined_ = sock_.JoinMulticast(cfg.group.c_str(), cfg.iface.c_str());
  if (!joined_) {
    Log::Warn("lan discovery: multicast join failed: %s",
              sock_.LastError().c_str());
  } else if (cfg.iface != "0.0.0.0") {
    sock_.SetMulticastInterface(cfg.iface.c_str());
  }
  localAddress_ = LocalAddressFor(cfg.group, cfg.port, cfg.iface);
  return true;
}

void LanDiscovery::Close() {
  sock_.Close();
  std::lock_guard<std::mutex> lk(mutex_);
  open_ = false;
  joined_ = false;
  services_.clear();
  addresses_.clear();
  published_ = false;
  pubInstance_.clear();
  pubPort_ = 0;
  pubTxt_.clear();
}

u16 LanDiscovery::localPort() const { return sock_.localPort(); }

std::string LanDiscovery::BuildResponse() const {
  const std::string instanceName = FullInstanceName(pubInstance_);
  const std::string hostName = pubInstance_ + ".local";
  std::string pkt;
  PutU16(&pkt, 0);          // id (mDNS uses 0)
  PutU16(&pkt, 0x8400);     // flags: response, authoritative
  PutU16(&pkt, 0);          // qd
  PutU16(&pkt, 4);          // an: PTR, SRV, TXT, A
  PutU16(&pkt, 0);          // ns
  PutU16(&pkt, 0);          // ar

  std::string rd;
  // PTR: service type -> instance
  EncodeName(&pkt, cfg_.serviceType);
  PutU16(&pkt, kTypePTR); PutU16(&pkt, 1); PutU32(&pkt, cfg_.ttlSeconds);
  rd.clear(); EncodeName(&rd, instanceName);
  PutU16(&pkt, (u16)rd.size()); pkt += rd;

  // SRV: instance -> host:port
  EncodeName(&pkt, instanceName);
  PutU16(&pkt, kTypeSRV); PutU16(&pkt, 1); PutU32(&pkt, cfg_.ttlSeconds);
  rd.clear(); PutU16(&rd, 0); PutU16(&rd, 0); PutU16(&rd, pubPort_);
  EncodeName(&rd, hostName);
  PutU16(&pkt, (u16)rd.size()); pkt += rd;

  // TXT: instance -> the caller's string (a single length-prefixed string).
  EncodeName(&pkt, instanceName);
  PutU16(&pkt, kTypeTXT); PutU16(&pkt, 1); PutU32(&pkt, cfg_.ttlSeconds);
  rd.clear();
  if (pubTxt_.empty()) {
    rd.push_back('\0');
  } else {
    const size_t n = std::min<size_t>(255, pubTxt_.size());
    rd.push_back((char)n);
    rd.append(pubTxt_, 0, n);
  }
  PutU16(&pkt, (u16)rd.size()); pkt += rd;

  // A: host -> address
  EncodeName(&pkt, hostName);
  PutU16(&pkt, kTypeA); PutU16(&pkt, 1); PutU32(&pkt, cfg_.ttlSeconds);
  rd.clear();
  u8 a[4] = {0, 0, 0, 0};
  ::inet_pton(AF_INET, localAddress_.c_str(), a);
  rd.assign((const char*)a, 4);
  PutU16(&pkt, (u16)rd.size()); pkt += rd;

  return pkt;
}

bool LanDiscovery::Publish(const std::string& instance, u16 port,
                           const std::string& txt) {
  if (!open_) return false;
  std::string pkt;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    pubInstance_ = instance;
    pubPort_ = port;
    pubTxt_ = txt;
    published_ = true;
    pkt = BuildResponse();
  }
  if (!sock_.SendTo(pkt.data(), pkt.size(), cfg_.group.c_str(), cfg_.port)) {
    Log::Warn("lan discovery: publish sendto failed: %s",
              sock_.LastError().c_str());
    return false;
  }
  ++stats_.packetsSent;
  return true;
}

bool LanDiscovery::Query() {
  if (!open_) return false;
  std::string pkt;
  PutU16(&pkt, 0);          // id
  PutU16(&pkt, 0x0000);     // flags: query
  PutU16(&pkt, 1);          // qd
  PutU16(&pkt, 0); PutU16(&pkt, 0); PutU16(&pkt, 0);
  EncodeName(&pkt, cfg_.serviceType);
  PutU16(&pkt, kTypePTR); PutU16(&pkt, 1);
  if (!sock_.SendTo(pkt.data(), pkt.size(), cfg_.group.c_str(), cfg_.port)) {
    Log::Warn("lan discovery: query sendto failed: %s", sock_.LastError().c_str());
    return false;
  }
  ++stats_.packetsSent;
  return true;
}

void LanDiscovery::HandlePacketLocked(const u8* data, size_t len, u64 now,
                                      bool* wantsReply) {
  ++stats_.packetsReceived;
  if (len < 12) { ++stats_.packetsDropped; return; }
  const u16 qd = GetU16(data + 4);
  const u16 an = GetU16(data + 6);
  const u16 ns = GetU16(data + 8);
  const u16 ar = GetU16(data + 10);
  size_t off = 12;

  // Questions: a PTR query for our service type is a request for our records.
  for (u16 i = 0; i < qd; ++i) {
    std::string qn;
    if (!DecodeName(data, len, &off, &qn) || off + 4 > len) {
      ++stats_.packetsDropped;
      return;
    }
    const u16 qt = GetU16(data + off);
    off += 4;
    if (qt == kTypePTR && qn == cfg_.serviceType) *wantsReply = true;
  }

  const size_t total = (size_t)an + ns + ar;
  for (size_t i = 0; i < total; ++i) {
    std::string owner;
    if (!DecodeName(data, len, &off, &owner) || off + 10 > len) {
      ++stats_.packetsDropped;
      return;
    }
    const u16 type = GetU16(data + off);
    const u16 rdlen = GetU16(data + off + 8);
    const size_t rdStart = off + 10;
    if (rdStart + rdlen > len) {
      ++stats_.packetsDropped;
      return;
    }
    ++stats_.recordsParsed;

    if (type == kTypePTR && owner == cfg_.serviceType) {
      size_t rp = rdStart;
      std::string inst;
      if (DecodeName(data, len, &rp, &inst) && EndsWith(inst, cfg_.serviceType)) {
        Entry& e = services_[inst];
        e.lastSeenMs = now;
      }
    } else if (type == kTypeSRV && EndsWith(owner, cfg_.serviceType)) {
      if (rdlen >= 6) {
        size_t rp = rdStart + 6;
        std::string target;
        if (DecodeName(data, len, &rp, &target)) {
          Entry& e = services_[owner];
          e.port = GetU16(data + rdStart + 4);
          e.host = target;
          e.haveSrv = true;
          e.lastSeenMs = now;
        }
      }
    } else if (type == kTypeTXT && EndsWith(owner, cfg_.serviceType)) {
      // A TXT rdata is a sequence of length-prefixed strings; join them.
      std::string txt;
      size_t p = rdStart;
      const size_t end = rdStart + rdlen;
      while (p < end) {
        const u8 sl = data[p];
        if (p + 1 + sl > end) break;
        if (!txt.empty()) txt += ';';
        txt.append((const char*)data + p + 1, sl);
        p += 1 + sl;
      }
      Entry& e = services_[owner];
      e.txt = txt;
      e.lastSeenMs = now;
    } else if (type == kTypeA) {
      if (rdlen == 4) {
        char ip[INET_ADDRSTRLEN] = {0};
        ::inet_ntop(AF_INET, data + rdStart, ip, sizeof ip);
        addresses_[owner] = {ip, now};
      }
    }
    off = rdStart + rdlen;
  }
}

void LanDiscovery::ExpireLocked(u64 now) {
  for (auto it = services_.begin(); it != services_.end();) {
    if (now > it->second.lastSeenMs + cfg_.cacheTtlMs) {
      ++stats_.servicesExpired;
      it = services_.erase(it);
    } else {
      ++it;
    }
  }
  for (auto it = addresses_.begin(); it != addresses_.end();) {
    if (now > it->second.second + cfg_.cacheTtlMs) it = addresses_.erase(it);
    else ++it;
  }
}

void LanDiscovery::Poll(int budgetMs) {
  if (!open_) return;
  const u64 deadline = clock_() + (u64)std::max(0, budgetMs);
  std::vector<u8> buf(1500);

  for (;;) {
    const u64 now = clock_();
    const int remaining = now >= deadline ? 0 : (int)(deadline - now);
    std::string host;
    u16 port = 0;
    const ssize_t n = sock_.RecvFrom(&buf[0], buf.size(), &host, &port, remaining);
    if (n <= 0) break;

    bool wantsReply = false;
    std::string reply;
    {
      std::lock_guard<std::mutex> lk(mutex_);
      HandlePacketLocked(&buf[0], (size_t)n, now, &wantsReply);
      // Build the answer under the same lock, so it sees a consistent
      // published state; send it after, so a slow socket does not hold the
      // lock while another thread waits.
      if (wantsReply && published_) reply = BuildResponse();
    }
    if (!reply.empty()) {
      if (sock_.SendTo(reply.data(), reply.size(), cfg_.group.c_str(), cfg_.port)) {
        ++stats_.packetsSent;
      }
    }
    if (now >= deadline) break;
  }

  std::lock_guard<std::mutex> lk(mutex_);
  ExpireLocked(clock_());
}

std::vector<LanService> LanDiscovery::services() const {
  std::lock_guard<std::mutex> lk(mutex_);
  const u64 now = clock_();
  std::vector<LanService> out;
  for (const auto& kv : services_) {
    if (now > kv.second.lastSeenMs + cfg_.cacheTtlMs) continue;
    // A PTR alone names a service but gives no way to reach it; report only a
    // service whose SRV record arrived, so every listed peer has a port.
    if (!kv.second.haveSrv) continue;
    LanService s;
    s.instance = kv.first;
    s.host = kv.second.host;
    s.port = kv.second.port;
    s.txt = kv.second.txt;
    s.lastSeenMs = kv.second.lastSeenMs;
    auto a = addresses_.find(kv.second.host);
    if (a != addresses_.end()) s.address = a->second.first;
    out.push_back(std::move(s));
  }
  std::sort(out.begin(), out.end(), [](const LanService& a, const LanService& b) {
    return a.lastSeenMs > b.lastSeenMs;
  });
  return out;
}

LanDiscovery::Stats LanDiscovery::stats() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stats_;
}

void LanDiscovery::SetClock(std::function<u64()> nowMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  clock_ = nowMs ? std::move(nowMs) : std::function<u64()>(&SteadyNowMs);
}

}  // namespace Berry
