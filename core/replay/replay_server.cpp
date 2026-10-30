// Loopback replay server. See include/berry/replay_server.h for the model.
#include "berry/replay_server.h"

#include "berry/url.h"

#include <algorithm>
#include <cstring>

namespace Berry {

namespace {

void PutU32(std::string* out, u32 v) {
  out->push_back((char)((v >> 24) & 0xff));
  out->push_back((char)((v >> 16) & 0xff));
  out->push_back((char)((v >> 8) & 0xff));
  out->push_back((char)(v & 0xff));
}
bool GetU32(const std::string& s, size_t off, u32* out) {
  if (off + 4 > s.size()) return false;
  *out = ((u32)(unsigned char)s[off] << 24) |
         ((u32)(unsigned char)s[off + 1] << 16) |
         ((u32)(unsigned char)s[off + 2] << 8) |
         (u32)(unsigned char)s[off + 3];
  return true;
}

constexpr char kJoin[4] = {'B', 'R', 'J', 'O'};
constexpr char kAccept[4] = {'B', 'R', 'A', 'C'};
constexpr char kPacket[4] = {'B', 'R', 'P', 'K'};
constexpr char kEnd[4] = {'B', 'R', 'E', 'N'};
constexpr char kBye[4] = {'B', 'R', 'B', 'Y'};
constexpr char kGoto[4] = {'B', 'R', 'G', 'O'};

}  // namespace

const char* ReplayServerErrorName(ReplayServerError e) {
  switch (e) {
    case ReplayServerError::Ok: return "ok";
    case ReplayServerError::NotLoopback: return "not_loopback";
    case ReplayServerError::BindFailed: return "bind_failed";
    case ReplayServerError::LiveSessionActive: return "live_session_active";
    case ReplayServerError::BadProtocolVersion: return "bad_protocol_version";
    case ReplayServerError::AlreadyRunning: return "already_running";
  }
  return "unknown";
}

ReplayServer::ReplayServer() = default;
ReplayServer::~ReplayServer() { Stop(); }

ReplayServerError ReplayServer::Start(const ReplayServerConfig& cfg) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (running_) return ReplayServerError::AlreadyRunning;
  // The whole safety story: a replay is served to loopback or not at all, so a
  // recorded session can never leave the device.
  if (!IsLoopbackHost(cfg.host)) return ReplayServerError::NotLoopback;
  // A replay must never run alongside a live session. Starting playback while
  // connected to a real server would feed recorded packets into that session,
  // so it is refused up front rather than left to a caller's discipline.
  if (cfg.liveSessionActive && cfg.liveSessionActive()) {
    // Remember the probe even on the refusal, so `liveBlocked()` explains why.
    liveProbe_ = cfg.liveSessionActive;
    return ReplayServerError::LiveSessionActive;
  }
  cfg_ = cfg;
  liveProbe_ = cfg.liveSessionActive;
  if (!sock_.Bind(cfg.port, cfg.host.c_str())) {
    return ReplayServerError::BindFailed;
  }
  running_ = true;
  client_ = Client{};
  packetsSent_ = 0;
  return ReplayServerError::Ok;
}

void ReplayServer::Stop() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!running_) return;
  sock_.Close();
  running_ = false;
  paused_ = false;
  client_ = Client{};
}

bool ReplayServer::running() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return running_;
}

u16 ReplayServer::port() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return running_ ? sock_.localPort() : 0;
}

ReplayServerConfig ReplayServer::config() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return cfg_;
}

ReplayServerError ReplayServer::Attach(const ReplayFile* file) {
  std::lock_guard<std::mutex> lk(mutex_);
  // A replay is only meaningful to the parsers of the build it was recorded
  // against. Refuse a protocol mismatch before it can displace a good replay,
  // so a wrong-version file never reaches the client's packet handlers.
  if (file && !file->sections.empty() && cfg_.expectedProtocolVersion != 0 &&
      file->fingerprint.protocolVersion != cfg_.expectedProtocolVersion) {
    return ReplayServerError::BadProtocolVersion;
  }
  file_ = (file && !file->sections.empty()) ? file : nullptr;
  client_ = Client{};
  packetsSent_ = 0;
  return ReplayServerError::Ok;
}

bool ReplayServer::liveBlocked() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return liveProbe_ && liveProbe_();
}

bool ReplayServer::SendFrameLocked(const std::string& frame) {
  if (!client_.connected) return false;
  return sock_.SendTo(frame.data(), frame.size(), client_.host.c_str(),
                      client_.port);
}

void ReplayServer::RewindToLocked(u64 tick) {
  client_.streamTick = tick;
  client_.sentEnd = false;
  client_.recordIdx = 0;
  client_.sectionIdx = 0;
  if (!file_) return;
  // Start at the first packet at or after `tick`, so a seek lands on the
  // nearest keyframe's worth of packets rather than replaying from the front.
  for (size_t i = 0; i < file_->sections.size(); ++i) {
    const ReplaySection& s = file_->sections[i];
    for (size_t j = 0; j < s.records.size(); ++j) {
      if (s.records[j].kind != ReplayRecordKind::Packet) continue;
      if (s.records[j].tick >= tick) {
        client_.sectionIdx = i;
        client_.recordIdx = j;
        return;
      }
    }
  }
  // No packet at or after `tick`: the cursor sits at the end.
  client_.sectionIdx = file_->sections.size();
  client_.recordIdx = 0;
}

void ReplayServer::StreamLocked() {
  if (!client_.connected || paused_) return;
  // The guard is re-checked on every stream, not only at Start: a live session
  // can begin mid-playback (the player joins a server while a replay is
  // scrubbing), and from that instant recorded packets must not reach the live
  // one. Drop the client so the stream cannot resume into a live session.
  if (liveProbe_ && liveProbe_()) {
    client_ = Client{};
    return;
  }
  if (!file_) {
    if (!client_.sentEnd) {
      std::string frame(kEnd, 4);
      SendFrameLocked(frame);
      client_.sentEnd = true;
    }
    return;
  }
  if (client_.sentEnd) return;

  if (client_.streamTick > file_->lastTick()) {
    std::string frame(kEnd, 4);
    SendFrameLocked(frame);
    client_.sentEnd = true;
    return;
  }

  // Send the packets recorded at this tick, walking the sections forward from
  // the cursor. The cursor only moves forward, so a pump is O(packets sent).
  size_t sent = 0;
  while (client_.sectionIdx < file_->sections.size() &&
         sent < cfg_.packetsPerTick) {
    const ReplaySection& s = file_->sections[client_.sectionIdx];
    if (client_.recordIdx >= s.records.size()) {
      ++client_.sectionIdx;
      client_.recordIdx = 0;
      continue;
    }
    const ReplayRecord& r = s.records[client_.recordIdx];
    if (r.tick > client_.streamTick) break;
    ++client_.recordIdx;
    if (r.kind != ReplayRecordKind::Packet) continue;
    std::string frame(kPacket, 4);
    PutU32(&frame, r.tag);
    PutU32(&frame, (u32)r.payload.size());
    frame.append(r.payload);
    if (!SendFrameLocked(frame)) return;
    ++packetsSent_;
    ++sent;
  }
  ++client_.streamTick;
}

void ReplayServer::HandleDatagramLocked(const std::string& data,
                                        const std::string& fromHost,
                                        u16 fromPort) {
  if (data.size() < 4) return;
  const std::string magic = data.substr(0, 4);

  if (magic == std::string(kJoin, 4)) {
    // One client at a time. A second joiner is ignored rather than served, so
    // the stream is never split.
    if (client_.connected) return;
    client_.connected = true;
    client_.host = fromHost;
    client_.port = fromPort;
    client_.sentEnd = false;
    RewindToLocked(file_ ? file_->firstTick() : 0);
    std::string frame(kAccept, 4);
    PutU32(&frame, (u32)client_.streamTick);
    SendFrameLocked(frame);
    return;
  }

  // Every other frame is only meaningful from the connected client.
  if (!client_.connected) return;
  if (fromHost != client_.host || fromPort != client_.port) return;

  if (magic == std::string(kBye, 4)) {
    client_ = Client{};
    return;
  }
  if (magic == std::string(kGoto, 4)) {
    u32 tick = 0;
    if (!GetU32(data, 4, &tick)) return;
    RewindToLocked((u64)tick);
    return;
  }
  // Unknown frame: dropped, client left connected.
}

size_t ReplayServer::Pump(int timeoutMs) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!running_) return 0;
  // While a live session is active the server does nothing at all: it neither
  // accepts a join nor streams, so no recorded byte can reach a live session.
  if (liveProbe_ && liveProbe_()) {
    client_ = Client{};
    return 0;
  }

  size_t processed = 0;
  // Drain the client's datagrams first, so a join or seek is applied before
  // this pump streams.
  for (size_t i = 0; i < cfg_.maxDatagramsPerPump; ++i) {
    std::vector<char> buf(cfg_.maxFrameBytes + 1);
    std::string fromHost;
    u16 fromPort = 0;
    const ssize_t n = sock_.RecvFrom(buf.data(), buf.size(), &fromHost, &fromPort,
                                     i == 0 ? timeoutMs : 0);
    if (n <= 0) break;
    ++processed;
    if ((size_t)n > cfg_.maxFrameBytes) {
      // An over-cap frame drops the client rather than growing the buffer.
      client_ = Client{};
      continue;
    }
    HandleDatagramLocked(std::string(buf.data(), (size_t)n), fromHost, fromPort);
  }

  StreamLocked();
  return processed;
}

bool ReplayServer::clientConnected() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return client_.connected;
}

u64 ReplayServer::packetsSent() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return packetsSent_;
}

u64 ReplayServer::streamTick() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return client_.streamTick;
}

bool ReplayServer::Seek(u64 tick) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (!client_.connected) return false;
  // Land on the nearest keyframe at or before `tick`, so the client's state is
  // complete before the deltas after it arrive.
  u64 target = tick;
  if (file_) {
    for (const ReplaySection& s : file_->sections) {
      if (s.kind != ReplaySectionKind::Keyframe) continue;
      if (s.startTick <= tick) target = s.startTick;
      else break;
    }
  }
  RewindToLocked(target);
  return true;
}

void ReplayServer::Pause() {
  std::lock_guard<std::mutex> lk(mutex_);
  paused_ = true;
}

void ReplayServer::Resume() {
  std::lock_guard<std::mutex> lk(mutex_);
  paused_ = false;
}

bool ReplayServer::paused() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return paused_;
}

}  // namespace Berry
