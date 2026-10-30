// Replay capture. See include/berry/replay_capture.h for the model.
//
// Two halves:
//
//   * `ReplayCodec` — a compact binary payload per record kind, so a consumer
//     decodes a struct instead of hand-parsing bytes. Every decoder fails
//     closed on a short or malformed payload: a corrupt replay must not make a
//     reader read past a buffer.
//   * `ReplayCapture` — the service that walks the seam once per tick, diffs
//     against the previous tick to build deltas, anchors them with periodic
//     keyframes, stores raw packets verbatim, and evicts the oldest sections to
//     hold a byte budget and a time window.
#include "berry/replay_capture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace Berry {

namespace {

//--- little-endian primitives (shared shape with replay_format.cpp) ---------

void PutU32(std::string* out, u32 v) {
  out->push_back((char)(v & 0xff));
  out->push_back((char)((v >> 8) & 0xff));
  out->push_back((char)((v >> 16) & 0xff));
  out->push_back((char)((v >> 24) & 0xff));
}
void PutU64(std::string* out, u64 v) {
  PutU32(out, (u32)(v & 0xffffffffu));
  PutU32(out, (u32)(v >> 32));
}
void PutF32(std::string* out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  PutU32(out, bits);
}

/// A bounds-checked reader. Every read either succeeds and advances or fails
/// and leaves the cursor alone.
class Reader {
 public:
  explicit Reader(const std::string& s)
      : data_(s.data()), size_(s.size()), left_(s.size()) {}
  bool U32(u32* out) {
    if (left_ < 4) return false;
    const unsigned char* b = (const unsigned char*)(data_ + pos_);
    *out = (u32)b[0] | ((u32)b[1] << 8) | ((u32)b[2] << 16) | ((u32)b[3] << 24);
    pos_ += 4; left_ -= 4;
    return true;
  }
  bool U64(u64* out) {
    u32 lo = 0, hi = 0;
    if (!U32(&lo) || !U32(&hi)) return false;
    *out = (u64)lo | ((u64)hi << 32);
    return true;
  }
  bool F32(f32* out) {
    u32 bits = 0;
    if (!U32(&bits)) return false;
    std::memcpy(out, &bits, 4);
    return true;
  }
  bool U8(u8* out) {
    if (left_ < 1) return false;
    *out = (u8)data_[pos_++]; --left_;
    return true;
  }
  bool Str(std::string* out, size_t cap) {
    u32 n = 0;
    if (!U32(&n)) return false;
    if (n > cap || n > left_) return false;
    out->assign(data_ + pos_, n);
    pos_ += n; left_ -= n;
    return true;
  }
  bool AtEnd() const { return left_ == 0; }

 private:
  const char* data_;
  size_t size_;
  size_t pos_ = 0;
  size_t left_ = 0;
};

/// A string cap for a decoded name/channel, so a corrupt length cannot ask for
/// a huge allocation.
constexpr size_t kMaxStringBytes = 4096;

}  // namespace

//--- codec -----------------------------------------------------------------

namespace ReplayCodec {

std::string EncodeEntity(const EntitySnapshot& snap, bool full) {
  std::string out;
  out.reserve(64);
  PutU64(&out, snap.id.raw);
  PutU32(&out, (u32)snap.id.runtime);
  PutU32(&out, (u32)snap.type);
  PutF32(&out, snap.position.x);
  PutF32(&out, snap.position.y);
  PutF32(&out, snap.position.z);
  PutF32(&out, snap.velocity.x);
  PutF32(&out, snap.velocity.y);
  PutF32(&out, snap.velocity.z);
  PutF32(&out, snap.rotation.x);
  PutF32(&out, snap.rotation.y);
  PutF32(&out, snap.rotation.z);
  PutF32(&out, snap.health);
  PutF32(&out, snap.maxHealth);
  out.push_back((char)(snap.alive ? 1 : 0));
  out.push_back((char)(snap.onGround ? 1 : 0));
  out.push_back((char)(full ? 1 : 0));
  out.push_back(0);   // reserved
  PutU64(&out, snap.lastSeenTick);
  // A bounded name, so a corrupt length cannot blow up a decode.
  const std::string name = snap.name.size() > kMaxStringBytes
                               ? snap.name.substr(0, kMaxStringBytes)
                               : snap.name;
  PutU32(&out, (u32)name.size());
  out.append(name);
  return out;
}

bool DecodeEntity(const std::string& payload, EntitySnapshot* out,
                  bool* outFull) {
  Reader r(payload);
  EntitySnapshot s;
  u32 runtime = 0, type = 0, lastSeen = 0;
  u8 alive = 0, onGround = 0, full = 0, reserved = 0;
  if (!r.U64(&s.id.raw)) return false;
  if (!r.U32(&runtime)) return false;
  if (!r.U32(&type)) return false;
  s.id.runtime = (u16)runtime;
  s.type = (EntityType)type;
  if (!r.F32(&s.position.x) || !r.F32(&s.position.y) || !r.F32(&s.position.z)) return false;
  if (!r.F32(&s.velocity.x) || !r.F32(&s.velocity.y) || !r.F32(&s.velocity.z)) return false;
  if (!r.F32(&s.rotation.x) || !r.F32(&s.rotation.y) || !r.F32(&s.rotation.z)) return false;
  if (!r.F32(&s.health)) return false;
  if (!r.F32(&s.maxHealth)) return false;
  if (!r.U8(&alive) || !r.U8(&onGround) || !r.U8(&full) || !r.U8(&reserved)) return false;
  s.alive = alive != 0;
  s.onGround = onGround != 0;
  if (!r.U64(&s.lastSeenTick)) return false;
  if (!r.Str(&s.name, kMaxStringBytes)) return false;
  if (!r.AtEnd()) return false;
  (void)lastSeen;
  *out = std::move(s);
  if (outFull) *outFull = full != 0;
  return true;
}

std::string EncodeTime(const LevelTime& time) {
  std::string out;
  PutU64(&out, (u64)time.absolute);
  PutF32(&out, time.daylight);
  out.push_back((char)(time.day ? 1 : 0));
  PutF32(&out, time.rainLevel);
  PutF32(&out, time.thunderLevel);
  return out;
}

bool DecodeTime(const std::string& payload, LevelTime* out) {
  Reader r(payload);
  u64 absolute = 0;
  u8 day = 0;
  LevelTime t;
  if (!r.U64(&absolute)) return false;
  t.absolute = (i64)absolute;
  if (!r.F32(&t.daylight)) return false;
  if (!r.U8(&day)) return false;
  t.day = day != 0;
  if (!r.F32(&t.rainLevel)) return false;
  if (!r.F32(&t.thunderLevel)) return false;
  if (!r.AtEnd()) return false;
  *out = t;
  return true;
}

std::string EncodeEvent(const std::string& channel, const std::string& payload) {
  std::string out;
  const std::string ch = channel.size() > kMaxStringBytes
                             ? channel.substr(0, kMaxStringBytes)
                             : channel;
  const std::string pl = payload.size() > kMaxStringBytes
                             ? payload.substr(0, kMaxStringBytes)
                             : payload;
  PutU32(&out, (u32)ch.size());
  out.append(ch);
  PutU32(&out, (u32)pl.size());
  out.append(pl);
  return out;
}

bool DecodeEvent(const std::string& payload, std::string* outChannel,
                 std::string* outPayload) {
  Reader r(payload);
  std::string ch, pl;
  if (!r.Str(&ch, kMaxStringBytes)) return false;
  if (!r.Str(&pl, kMaxStringBytes)) return false;
  if (!r.AtEnd()) return false;
  *outChannel = std::move(ch);
  *outPayload = std::move(pl);
  return true;
}

}  // namespace ReplayCodec

const char* CaptureStateName(CaptureState s) {
  switch (s) {
    case CaptureState::Idle: return "idle";
    case CaptureState::Recording: return "recording";
    case CaptureState::Throttled: return "throttled";
    case CaptureState::Stopped: return "stopped";
  }
  return "unknown";
}

//----------------------------------------------------------------------------
// ReplayCapture
//----------------------------------------------------------------------------

ReplayCapture::ReplayCapture() = default;
ReplayCapture::~ReplayCapture() = default;

void ReplayCapture::SetConfig(const CaptureConfig& cfg) {
  std::lock_guard<std::mutex> lk(mutex_);
  cfg_ = cfg;
  if (state_ == CaptureState::Recording || state_ == CaptureState::Throttled) {
    EvictLocked();
  }
}

CaptureConfig ReplayCapture::config() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return cfg_;
}

void ReplayCapture::SetFingerprint(const ReplayFingerprint& fp) {
  std::lock_guard<std::mutex> lk(mutex_);
  fp_ = fp;
}

ReplayFingerprint ReplayCapture::fingerprint() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return fp_;
}

bool ReplayCapture::Start() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (state_ == CaptureState::Recording || state_ == CaptureState::Throttled) {
    return false;
  }
  sections_.clear();
  pending_ = ReplaySection{};
  building_ = false;
  bytes_ = 0;
  evicted_ = 0;
  overflowed_ = false;
  haveKeyframe_ = false;
  haveTime_ = false;
  lastEntities_.clear();
  anonCounter_ = 0;
  state_ = thermal_ >= kThermalThrottleLevel ? CaptureState::Throttled
                                             : CaptureState::Recording;
  return true;
}

bool ReplayCapture::Stop() {
  std::lock_guard<std::mutex> lk(mutex_);
  if (state_ != CaptureState::Recording && state_ != CaptureState::Throttled) {
    return false;
  }
  // Flush a half-built delta so the last tick is not lost.
  if (building_ && !pending_.records.empty()) {
    pending_.endTick = tick_;
    PushLocked(pending_);
  }
  building_ = false;
  pending_ = ReplaySection{};
  state_ = CaptureState::Idle;
  return true;
}

CaptureState ReplayCapture::state() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return state_;
}

bool ReplayCapture::recording() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return state_ == CaptureState::Recording || state_ == CaptureState::Throttled;
}

void ReplayCapture::SetThermalLevel(i32 level) {
  std::lock_guard<std::mutex> lk(mutex_);
  thermal_ = level < 0 ? 0 : level;
  if (state_ == CaptureState::Recording && thermal_ >= kThermalThrottleLevel) {
    state_ = CaptureState::Throttled;
  } else if (state_ == CaptureState::Throttled &&
             thermal_ < kThermalThrottleLevel) {
    state_ = CaptureState::Recording;
  }
}

i32 ReplayCapture::thermalLevel() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return thermal_;
}

bool ReplayCapture::PushLocked(const ReplaySection& s) {
  if (sections_.size() >= 1000000) {   // an absolute backstop
    overflowed_ = true;
    return false;
  }
  sections_.push_back(s);
  for (const ReplayRecord& r : s.records) bytes_ += r.payload.size();
  EvictLocked();
  return true;
}

void ReplayCapture::EvictLocked() {
  // The ring is a section count derived from the configured seconds: at 20 tps
  // a delta covers one tick and a keyframe one tick, so the window in sections
  // is the window in ticks.
  size_t ringSections = 0;
  if (cfg_.ringSeconds > 0.0f) {
    const f64 ticks = (f64)cfg_.ringSeconds * 20.0;
    ringSections = (size_t)std::max<f64>(1.0, ticks);
  }
  while (!sections_.empty() &&
         ((cfg_.maxBytes > 0 && bytes_ > cfg_.maxBytes) ||
          (ringSections > 0 && sections_.size() > ringSections))) {
    for (const ReplayRecord& r : sections_.front().records) {
      bytes_ -= std::min(bytes_, r.payload.size());
    }
    sections_.pop_front();
    ++evicted_;
  }
}

void ReplayCapture::CaptureTick(u64 tick, LevelSeam& seam,
                                const std::vector<WorldDiffEvent>& diff,
                                EntityId localPlayer) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (state_ != CaptureState::Recording && state_ != CaptureState::Throttled) {
    return;
  }
  tick_ = tick;

  const bool wantKeyframe =
      !haveKeyframe_ || (cfg_.keyframeTicks > 0 &&
                         tick >= lastKeyframeTick_ + cfg_.keyframeTicks);
  const bool throttled = state_ == CaptureState::Throttled;

  // Gather the current entity set once; both paths use it.
  std::vector<EntitySnapshot> now;
  const std::vector<EntityId> ids = seam.AllEntities();
  now.reserve(ids.size());
  for (const EntityId id : ids) {
    EntitySnapshot s;
    if (seam.GetEntity(id, &s)) now.push_back(std::move(s));
  }

  if (wantKeyframe) {
    ReplaySection key;
    key.kind = ReplaySectionKind::Keyframe;
    key.startTick = tick;
    key.endTick = tick;
    for (const EntitySnapshot& s : now) {
      ReplayRecord r;
      r.tick = tick;
      r.kind = ReplayRecordKind::EntitySpawn;
      r.tag = (u32)s.type;
      r.payload = ReplayCodec::EncodeEntity(s, /*full=*/true);
      key.records.push_back(std::move(r));
    }
    const LevelTime t = seam.GetTime();
    ReplayRecord tr;
    tr.tick = tick;
    tr.kind = ReplayRecordKind::TimeWeather;
    tr.payload = ReplayCodec::EncodeTime(t);
    key.records.push_back(std::move(tr));

    // The keyframe supersedes any half-built delta for this tick.
    if (building_ && !pending_.records.empty()) {
      pending_.endTick = tick > 0 ? tick - 1 : 0;
      if (pending_.endTick >= pending_.startTick) PushLocked(pending_);
    }
    pending_ = ReplaySection{};
    building_ = false;

    if (!PushLocked(key)) return;
    lastKeyframeTick_ = tick;
    haveKeyframe_ = true;
    lastEntities_ = now;
    lastTime_ = t;
    haveTime_ = true;
  } else if (!throttled) {
    // A delta: only what changed. Compare against the previous tick's set.
    if (!building_) {
      pending_ = ReplaySection{};
      pending_.kind = ReplaySectionKind::Delta;
      pending_.startTick = tick;
      building_ = true;
    }
    pending_.endTick = tick;

    std::unordered_map<u64, const EntitySnapshot*> prev;
    prev.reserve(lastEntities_.size() * 2);
    for (const EntitySnapshot& s : lastEntities_) prev[s.id.raw] = &s;

    std::unordered_set<u64> seen;
    seen.reserve(now.size() * 2);
    for (const EntitySnapshot& s : now) {
      seen.insert(s.id.raw);
      auto it = prev.find(s.id.raw);
      const bool spawned = it == prev.end();
      bool moved = spawned;
      bool changed = spawned;
      if (!spawned) {
        const EntitySnapshot& p = *it->second;
        moved = p.position.x != s.position.x || p.position.y != s.position.y ||
                p.position.z != s.position.z || p.velocity.x != s.velocity.x ||
                p.velocity.y != s.velocity.y || p.velocity.z != s.velocity.z ||
                p.rotation.x != s.rotation.x || p.rotation.y != s.rotation.y ||
                p.rotation.z != s.rotation.z;
        changed = moved || p.health != s.health || p.alive != s.alive ||
                  p.name != s.name || p.type != s.type;
      }
      if (!changed) continue;
      ReplayRecord r;
      r.tick = tick;
      r.kind = spawned ? ReplayRecordKind::EntitySpawn
                       : (moved ? ReplayRecordKind::EntityMove
                                : ReplayRecordKind::EntityMetadata);
      r.tag = (u32)s.type;
      r.payload = ReplayCodec::EncodeEntity(s, /*full=*/false);
      pending_.records.push_back(std::move(r));
    }
    // Entities that left the client's view.
    for (const EntitySnapshot& p : lastEntities_) {
      if (seen.count(p.id.raw)) continue;
      ReplayRecord r;
      r.tick = tick;
      r.kind = ReplayRecordKind::EntityLeave;
      r.tag = (u32)p.type;
      r.payload = ReplayCodec::EncodeEntity(p, /*full=*/false);
      pending_.records.push_back(std::move(r));
    }
    // A time/weather change.
    const LevelTime t = seam.GetTime();
    if (!haveTime_ || t.absolute != lastTime_.absolute ||
        t.daylight != lastTime_.daylight || t.day != lastTime_.day ||
        t.rainLevel != lastTime_.rainLevel ||
        t.thunderLevel != lastTime_.thunderLevel) {
      ReplayRecord tr;
      tr.tick = tick;
      tr.kind = ReplayRecordKind::TimeWeather;
      tr.payload = ReplayCodec::EncodeTime(t);
      pending_.records.push_back(std::move(tr));
      lastTime_ = t;
      haveTime_ = true;
    }
  }

  // The local player's own position each tick, so its motion is in the replay
  // even between keyframes.
  if (localPlayer.raw != 0 && !throttled) {
    EntitySnapshot s;
    if (seam.GetEntity(localPlayer, &s)) {
      ReplayRecord r;
      r.tick = tick;
      r.kind = ReplayRecordKind::PlayerInput;
      r.tag = (u32)s.type;
      r.payload = ReplayCodec::EncodeEntity(s, /*full=*/false);
      if (!building_) {
        pending_ = ReplaySection{};
        pending_.kind = ReplaySectionKind::Delta;
        pending_.startTick = tick;
        building_ = true;
      }
      pending_.endTick = tick;
      pending_.records.push_back(std::move(r));
    }
  }

  // Timeline markers from the grid's world transitions.
  for (const WorldDiffEvent& e : diff) {
    const char* channel = WorldDiffChannel(e.kind);
    if (!channel) continue;
    const EntitySnapshot& snap = e.kind == WorldDiff::Left ? e.before : e.after;
    ReplayRecord r;
    r.tick = tick;
    r.kind = ReplayRecordKind::Event;
    r.payload = ReplayCodec::EncodeEvent(channel, WorldDiffPayload(snap));
    if (!building_) {
      pending_ = ReplaySection{};
      pending_.kind = ReplaySectionKind::Delta;
      pending_.startTick = tick;
      building_ = true;
    }
    pending_.endTick = tick;
    pending_.records.push_back(std::move(r));
  }

  // A delta that covers many ticks is flushed so a seek never decodes more than
  // one keyframe interval of deltas. A small one waits for the next keyframe.
  if (building_ && cfg_.keyframeTicks > 0 &&
      tick >= pending_.startTick + cfg_.keyframeTicks) {
    PushLocked(pending_);
    pending_ = ReplaySection{};
    building_ = false;
  }

  lastEntities_ = std::move(now);
}

bool ReplayCapture::CapturePacket(PacketDir dir, u32 id,
                                  const std::string& payload, u64 tick,
                                  ReplayRecordKind kind) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (state_ != CaptureState::Recording && state_ != CaptureState::Throttled) {
    return false;
  }
  if (!cfg_.capturePackets) return false;
  if (payload.size() > cfg_.maxPacketBytes) {
    overflowed_ = true;
    return false;
  }
  if (!building_) {
    pending_ = ReplaySection{};
    pending_.kind = ReplaySectionKind::Delta;
    pending_.startTick = tick;
    building_ = true;
  }
  pending_.endTick = std::max(pending_.endTick, tick);
  ReplayRecord r;
  r.tick = tick;
  // A chunk/block packet is stored under its own kind so the loopback server
  // and a consumer can route it without re-parsing the id; everything else is
  // a plain packet.
  r.kind = (kind == ReplayRecordKind::Chunk) ? ReplayRecordKind::Chunk
                                             : ReplayRecordKind::Packet;
  r.tag = id;
  r.dir = dir == PacketDir::Outbound ? 1 : 0;
  r.payload = payload;
  pending_.records.push_back(std::move(r));
  return true;
}

bool ReplayCapture::CaptureEvent(const std::string& channel,
                                 const std::string& payload, u64 tick) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (state_ != CaptureState::Recording && state_ != CaptureState::Throttled) {
    return false;
  }
  if (!building_) {
    pending_ = ReplaySection{};
    pending_.kind = ReplaySectionKind::Delta;
    pending_.startTick = tick;
    building_ = true;
  }
  pending_.endTick = std::max(pending_.endTick, tick);
  ReplayRecord r;
  r.tick = tick;
  r.kind = ReplayRecordKind::Event;
  r.payload = ReplayCodec::EncodeEvent(channel, payload);
  pending_.records.push_back(std::move(r));
  return true;
}

size_t ReplayCapture::sectionCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return sections_.size() + (building_ && !pending_.records.empty() ? 1 : 0);
}

size_t ReplayCapture::bytes() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return bytes_;
}

u64 ReplayCapture::evicted() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return evicted_;
}

bool ReplayCapture::overflowed() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return overflowed_;
}

u64 ReplayCapture::firstTick() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (sections_.empty()) return 0;
  return sections_.front().startTick;
}

u64 ReplayCapture::lastTick() const {
  std::lock_guard<std::mutex> lk(mutex_);
  if (building_ && !pending_.records.empty()) return pending_.endTick;
  if (sections_.empty()) return 0;
  return sections_.back().endTick;
}

bool ReplayCapture::empty() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return sections_.empty() && pending_.records.empty();
}

void ReplayCapture::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  sections_.clear();
  pending_ = ReplaySection{};
  building_ = false;
  bytes_ = 0;
  evicted_ = 0;
  overflowed_ = false;
  haveKeyframe_ = false;
  haveTime_ = false;
  lastEntities_.clear();
}

ReplayFile ReplayCapture::Snapshot() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return SnapshotLocked();
}

ReplayFile ReplayCapture::SnapshotLocked() const {
  ReplayFile f;
  f.fingerprint = fp_;
  for (const ReplaySection& s : sections_) f.sections.push_back(s);
  // Include a half-built delta only when it is the only thing there is, so a
  // snapshot taken mid-tick is not missing the newest records.
  if (building_ && !pending_.records.empty()) f.sections.push_back(pending_);
  f.RebuildIndex();
  return f;
}

/// Rewrites every recorded display name in `f` to a stable pseudonym. The map
/// is local to the call, so the same name maps to the same pseudonym within one
/// export. This is the "anonymize on save" path: the live capture keeps the
/// real names, only the exported copy is rewritten.
void ReplayCapture::AnonymizeFile(ReplayFile* f, size_t* counter) const {
  std::unordered_map<std::string, std::string> map;
  for (ReplaySection& s : f->sections) {
    for (ReplayRecord& r : s.records) {
      if (r.kind != ReplayRecordKind::EntitySpawn &&
          r.kind != ReplayRecordKind::EntityMove &&
          r.kind != ReplayRecordKind::EntityMetadata &&
          r.kind != ReplayRecordKind::EntityLeave &&
          r.kind != ReplayRecordKind::PlayerInput) {
        continue;
      }
      EntitySnapshot snap;
      bool full = false;
      if (!ReplayCodec::DecodeEntity(r.payload, &snap, &full)) continue;
      if (snap.name.empty()) continue;
      auto it = map.find(snap.name);
      if (it == map.end()) {
        it = map.emplace(snap.name, "Player" + std::to_string(++*counter)).first;
      }
      snap.name = it->second;
      r.payload = ReplayCodec::EncodeEntity(snap, full);
    }
  }
}

ReplayError ReplayCapture::Save(std::string* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  ReplayFile f = SnapshotLocked();
  if (cfg_.anonymizeOnSave) {
    size_t counter = 0;
    AnonymizeFile(&f, &counter);
  }
  ReplayWriter w;
  return w.Write(f, out);
}

ReplayError ReplayCapture::SaveRing(f32 ringSeconds, std::string* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  ReplayFile f = SnapshotLocked();
  if (cfg_.anonymizeOnSave) {
    size_t counter = 0;
    AnonymizeFile(&f, &counter);
  }
  size_t ringSections = 0;
  if (ringSeconds > 0.0f) {
    ringSections = (size_t)std::max<f64>(1.0, (f64)ringSeconds * 20.0);
  }
  ReplayWriter w;
  return w.WriteRing(f, ringSections, out);
}

void ReplayCapture::Anonymize() {
  std::lock_guard<std::mutex> lk(mutex_);
  std::unordered_map<std::string, std::string> map;
  auto rewrite = [&](ReplaySection& s) {
    for (ReplayRecord& r : s.records) {
      if (r.kind != ReplayRecordKind::EntitySpawn &&
          r.kind != ReplayRecordKind::EntityMove &&
          r.kind != ReplayRecordKind::EntityMetadata &&
          r.kind != ReplayRecordKind::EntityLeave &&
          r.kind != ReplayRecordKind::PlayerInput) {
        continue;
      }
      EntitySnapshot snap;
      bool full = false;
      if (!ReplayCodec::DecodeEntity(r.payload, &snap, &full)) continue;
      if (snap.name.empty()) continue;
      auto it = map.find(snap.name);
      if (it == map.end()) {
        it = map.emplace(snap.name, "Player" + std::to_string(++anonCounter_)).first;
      }
      snap.name = it->second;
      r.payload = ReplayCodec::EncodeEntity(snap, full);
    }
  };
  for (ReplaySection& s : sections_) rewrite(s);
  if (building_) rewrite(pending_);
  // The diff baseline carries names too, so re-anonymize it consistently.
  for (EntitySnapshot& e : lastEntities_) {
    auto it = map.find(e.name);
    if (it == map.end() && !e.name.empty()) {
      it = map.emplace(e.name, "Player" + std::to_string(++anonCounter_)).first;
    }
    if (it != map.end()) e.name = it->second;
  }
}

}  // namespace Berry
