// Record & replay. See include/berry/record.h for the model.
#include "berry/record.h"

#include "json_min.h"
#include "json_write.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace Berry {

namespace jsonw = detail::jsonw;   // internal writer, see json_write.h

namespace {

bool ReadInt(const detail::json::Value* v, i64* out) {
  if (!v || !v->IsNumber()) return false;
  const f64 n = v->number;
  if (n != std::floor(n)) return false;
  if (n < (f64)std::numeric_limits<i64>::min() ||
      n > (f64)std::numeric_limits<i64>::max()) {
    return false;
  }
  *out = (i64)n;
  return true;
}

bool ReadU64Field(const detail::json::Value* v, u64* out) {
  i64 i = 0;
  if (!ReadInt(v, &i) || i < 0) return false;
  *out = (u64)i;
  return true;
}

bool ReadVec3(const detail::json::Value* v, Vec3* out) {
  if (!v || !v->IsArray() || v->array.size() != 3) return false;
  for (const auto& e : v->array) {
    if (!e.IsNumber()) return false;
  }
  *out = Vec3((f32)v->array[0].number, (f32)v->array[1].number,
              (f32)v->array[2].number);
  return true;
}

}  // namespace

const char* TapeErrorName(TapeError e) {
  switch (e) {
    case TapeError::Ok: return "ok";
    case TapeError::BadJson: return "bad json";
    case TapeError::MissingVersion: return "missing format version";
    case TapeError::BadVersion: return "unsupported format version";
    case TapeError::NoFrames: return "no frames";
    case TapeError::TooManyFrames: return "too many frames";
    case TapeError::TooManyEntities: return "too many entities in a frame";
    case TapeError::TooManyEvents: return "too many events in a frame";
    case TapeError::BadFrame: return "bad frame";
    case TapeError::BadEntity: return "bad entity";
    case TapeError::BadEvent: return "bad event";
    case TapeError::BadBlock: return "bad block";
    case TapeError::BadTick: return "ticks are not strictly increasing";
  }
  return "unknown";
}

//----------------------------------------------------------------------------
// Recording
//----------------------------------------------------------------------------

bool Recording::Push(const RecordedFrame& frame) {
  if (frames_.size() >= kMaxFrames) return false;
  if (!frames_.empty() && frame.tick <= frames_.back().tick) return false;
  frames_.push_back(frame);
  return true;
}

void WriteEntity(jsonw::Writer& w, const EntitySnapshot& e) {
  w.BeginObject();
  w.Member("id", e.id.raw);
  w.Member("runtime", (u64)e.id.runtime);
  w.Member("type", std::string(EntityTypeName(e.type)));
  w.Key("pos");
  w.BeginArray();
  w.Number(e.position.x); w.Number(e.position.y); w.Number(e.position.z);
  w.EndArray();
  w.Key("vel");
  w.BeginArray();
  w.Number(e.velocity.x); w.Number(e.velocity.y); w.Number(e.velocity.z);
  w.EndArray();
  w.Key("rot");
  w.BeginArray();
  w.Number(e.rotation.x); w.Number(e.rotation.y); w.Number(e.rotation.z);
  w.EndArray();
  w.Member("health", (f64)e.health);
  w.Member("max_health", (f64)e.maxHealth);
  w.Member("alive", e.alive);
  w.Member("on_ground", e.onGround);
  w.Member("name", e.name);
  w.Member("last_seen", e.lastSeenTick);
  w.EndObject();
}

std::string Recording::ToJson() const {
  std::string out;
  out.reserve(frames_.size() * 256 + 64);
  jsonw::Writer w(&out);
  w.BeginObject();
  w.Member("format", (u64)kFormatVersion);
  w.Key("frames");
  w.BeginArray();
  for (const RecordedFrame& f : frames_) {
    w.BeginObject();
    w.Member("tick", f.tick);
    w.Key("time");
    w.BeginObject();
    w.Member("absolute", f.time.absolute);
    w.Member("daylight", (f64)f.time.daylight);
    w.Member("day", f.time.day);
    w.Member("rain", (f64)f.time.rainLevel);
    w.Member("thunder", (f64)f.time.thunderLevel);
    w.EndObject();
    w.Key("entities");
    w.BeginArray();
    for (const EntitySnapshot& e : f.entities) WriteEntity(w, e);
    w.EndArray();
    w.Key("events");
    w.BeginArray();
    for (const RecordedEvent& e : f.events) {
      w.BeginObject();
      w.Member("channel", e.channel);
      w.Member("payload", e.payload);
      w.EndObject();
    }
    w.EndArray();
    w.Key("blocks");
    w.BeginArray();
    for (const RecordedBlock& b : f.blocks) {
      w.BeginObject();
      w.Member("x", (i64)b.x);
      w.Member("y", (i64)b.y);
      w.Member("z", (i64)b.z);
      w.Member("block", (u64)b.block);
      w.EndObject();
    }
    w.EndArray();
    w.EndObject();
  }
  w.EndArray();
  w.EndObject();
  return out;
}

bool Recording::FromJson(const std::string& json, Recording* out,
                         TapeError* outError, std::string* error) {
  auto fail = [&](TapeError e, const std::string& detail) {
    if (outError) *outError = e;
    if (error) *error = detail;
    return false;
  };

  detail::json::Value root;
  std::string jerr;
  if (!detail::json::Parse(json, &root, &jerr)) return fail(TapeError::BadJson, jerr);
  if (!root.IsObject()) return fail(TapeError::BadJson, "root is not an object");

  static const char* kKnown[] = {"format", "frames"};
  for (const auto& kv : root.object) {
    bool known = false;
    for (const char* k : kKnown) {
      if (kv.first == k) { known = true; break; }
    }
    if (!known) return fail(TapeError::BadJson, "unknown key '" + kv.first + "'");
  }

  const detail::json::Value* format = root.Get("format");
  if (!format) return fail(TapeError::MissingVersion, "missing 'format'");
  i64 ver = 0;
  if (!ReadInt(format, &ver)) return fail(TapeError::BadVersion, "bad 'format'");
  if (ver != (i64)kFormatVersion) {
    return fail(TapeError::BadVersion, "unsupported format version");
  }

  const detail::json::Value* frames = root.Get("frames");
  if (!frames || !frames->IsArray()) return fail(TapeError::NoFrames, "missing 'frames'");
  if (frames->array.empty()) return fail(TapeError::NoFrames, "no frames");
  if (frames->array.size() > kMaxFrames) return fail(TapeError::TooManyFrames, "too many frames");

  Recording tape;
  u64 lastTick = 0;
  bool first = true;
  for (const auto& fv : frames->array) {
    if (!fv.IsObject()) return fail(TapeError::BadFrame, "frame is not an object");
    static const char* kFrameKnown[] = {"tick", "time", "entities", "events", "blocks"};
    for (const auto& kv : fv.object) {
      bool known = false;
      for (const char* k : kFrameKnown) {
        if (kv.first == k) { known = true; break; }
      }
      if (!known) return fail(TapeError::BadFrame, "unknown frame key '" + kv.first + "'");
    }
    RecordedFrame f;
    const detail::json::Value* tick = fv.Get("tick");
    if (!tick || !ReadU64Field(tick, &f.tick)) {
      return fail(TapeError::BadFrame, "frame is missing a valid 'tick'");
    }
    if (!first && f.tick <= lastTick) return fail(TapeError::BadTick, "ticks must increase");
    lastTick = f.tick;
    first = false;

    if (const detail::json::Value* t = fv.Get("time")) {
      if (!t->IsObject()) return fail(TapeError::BadFrame, "'time' is not an object");
      i64 abs = 0;
      if (const detail::json::Value* a = t->Get("absolute")) {
        if (!ReadInt(a, &abs)) return fail(TapeError::BadFrame, "bad 'absolute'");
      }
      f.time.absolute = abs;
      if (const detail::json::Value* d = t->Get("daylight")) {
        if (!d->IsNumber()) return fail(TapeError::BadFrame, "bad 'daylight'");
        f.time.daylight = (f32)d->number;
      }
      if (const detail::json::Value* day = t->Get("day")) {
        if (!day->IsBool()) return fail(TapeError::BadFrame, "bad 'day'");
        f.time.day = day->boolean;
      }
      if (const detail::json::Value* r = t->Get("rain")) {
        if (!r->IsNumber()) return fail(TapeError::BadFrame, "bad 'rain'");
        f.time.rainLevel = (f32)r->number;
      }
      if (const detail::json::Value* th = t->Get("thunder")) {
        if (!th->IsNumber()) return fail(TapeError::BadFrame, "bad 'thunder'");
        f.time.thunderLevel = (f32)th->number;
      }
    }

    if (const detail::json::Value* es = fv.Get("entities")) {
      if (!es->IsArray()) return fail(TapeError::BadFrame, "'entities' is not an array");
      if (es->array.size() > kMaxEntitiesPerFrame) {
        return fail(TapeError::TooManyEntities, "too many entities in a frame");
      }
      for (const auto& ev : es->array) {
        if (!ev.IsObject()) return fail(TapeError::BadEntity, "entity is not an object");
        EntitySnapshot s;
        const detail::json::Value* id = ev.Get("id");
        if (!id || !ReadU64Field(id, &s.id.raw)) {
          return fail(TapeError::BadEntity, "entity is missing a valid 'id'");
        }
        if (const detail::json::Value* rt = ev.Get("runtime")) {
          u64 r = 0;
          if (!ReadU64Field(rt, &r)) return fail(TapeError::BadEntity, "bad 'runtime'");
          s.id.runtime = (u16)r;
        }
        if (const detail::json::Value* ty = ev.Get("type")) {
          if (!ty->IsString()) return fail(TapeError::BadEntity, "bad 'type'");
          if (!ParseEntityType(ty->str, &s.type)) {
            return fail(TapeError::BadEntity, "unknown entity type '" + ty->str + "'");
          }
        }
        if (const detail::json::Value* p = ev.Get("pos")) {
          if (!ReadVec3(p, &s.position)) return fail(TapeError::BadEntity, "bad 'pos'");
        }
        if (const detail::json::Value* v = ev.Get("vel")) {
          if (!ReadVec3(v, &s.velocity)) return fail(TapeError::BadEntity, "bad 'vel'");
        }
        if (const detail::json::Value* r = ev.Get("rot")) {
          if (!ReadVec3(r, &s.rotation)) return fail(TapeError::BadEntity, "bad 'rot'");
        }
        if (const detail::json::Value* h = ev.Get("health")) {
          if (!h->IsNumber()) return fail(TapeError::BadEntity, "bad 'health'");
          s.health = (f32)h->number;
        }
        if (const detail::json::Value* h = ev.Get("max_health")) {
          if (!h->IsNumber()) return fail(TapeError::BadEntity, "bad 'max_health'");
          s.maxHealth = (f32)h->number;
        }
        if (const detail::json::Value* a = ev.Get("alive")) {
          if (!a->IsBool()) return fail(TapeError::BadEntity, "bad 'alive'");
          s.alive = a->boolean;
        }
        if (const detail::json::Value* g = ev.Get("on_ground")) {
          if (!g->IsBool()) return fail(TapeError::BadEntity, "bad 'on_ground'");
          s.onGround = g->boolean;
        }
        if (const detail::json::Value* n = ev.Get("name")) {
          if (!n->IsString()) return fail(TapeError::BadEntity, "bad 'name'");
          s.name = n->str;
        }
        if (const detail::json::Value* l = ev.Get("last_seen")) {
          if (!ReadU64Field(l, &s.lastSeenTick)) {
            return fail(TapeError::BadEntity, "bad 'last_seen'");
          }
        }
        f.entities.push_back(std::move(s));
      }
    }

    if (const detail::json::Value* evs = fv.Get("events")) {
      if (!evs->IsArray()) return fail(TapeError::BadFrame, "'events' is not an array");
      if (evs->array.size() > kMaxEventsPerFrame) {
        return fail(TapeError::TooManyEvents, "too many events in a frame");
      }
      for (const auto& e : evs->array) {
        if (!e.IsObject()) return fail(TapeError::BadEvent, "event is not an object");
        const detail::json::Value* c = e.Get("channel");
        const detail::json::Value* p = e.Get("payload");
        if (!c || !c->IsString() || !p || !p->IsString()) {
          return fail(TapeError::BadEvent, "event needs string 'channel' and 'payload'");
        }
        f.events.push_back(RecordedEvent{c->str, p->str});
      }
    }

    if (const detail::json::Value* bs = fv.Get("blocks")) {
      if (!bs->IsArray()) return fail(TapeError::BadFrame, "'blocks' is not an array");
      if (bs->array.size() > kMaxBlocksPerFrame) {
        return fail(TapeError::TooManyEvents, "too many blocks in a frame");
      }
      for (const auto& b : bs->array) {
        if (!b.IsObject()) return fail(TapeError::BadBlock, "block is not an object");
        RecordedBlock rb;
        i64 x = 0, y = 0, z = 0;
        if (!ReadInt(b.Get("x"), &x) || !ReadInt(b.Get("y"), &y) ||
            !ReadInt(b.Get("z"), &z)) {
          return fail(TapeError::BadBlock, "block needs integer 'x','y','z'");
        }
        u64 blk = 0;
        if (!ReadU64Field(b.Get("block"), &blk)) {
          return fail(TapeError::BadBlock, "block needs a valid 'block'");
        }
        rb.x = (i32)x; rb.y = (i32)y; rb.z = (i32)z; rb.block = (u32)blk;
        f.blocks.push_back(rb);
      }
    }

    tape.frames_.push_back(std::move(f));
  }

  *out = std::move(tape);
  if (outError) *outError = TapeError::Ok;
  if (error) error->clear();
  return true;
}

//----------------------------------------------------------------------------
// Recorder
//----------------------------------------------------------------------------

void Recorder::BeginFrame(u64 tick, const LevelTime& time) {
  pending_ = RecordedFrame{};
  pending_.tick = tick;
  pending_.time = time;
  building_ = true;
}

bool Recorder::AddEntity(const EntitySnapshot& snap) {
  if (!building_) return false;
  if (pending_.entities.size() >= Recording::kMaxEntitiesPerFrame) {
    overflowed_ = true;
    return false;
  }
  pending_.entities.push_back(snap);
  return true;
}

bool Recorder::AddEvent(const std::string& channel, const std::string& payload) {
  if (!building_) return false;
  if (pending_.events.size() >= Recording::kMaxEventsPerFrame) {
    overflowed_ = true;
    return false;
  }
  pending_.events.push_back(RecordedEvent{channel, payload});
  return true;
}

bool Recorder::AddBlock(i32 x, i32 y, i32 z, u32 block) {
  if (!building_) return false;
  if (pending_.blocks.size() >= Recording::kMaxBlocksPerFrame) {
    overflowed_ = true;
    return false;
  }
  RecordedBlock b;
  b.x = x; b.y = y; b.z = z; b.block = block;
  pending_.blocks.push_back(b);
  return true;
}

bool Recorder::CommitFrame() {
  if (!building_ || !tape_) return false;
  building_ = false;
  // A tape's ticks must strictly increase. A caller that commits two frames at
  // the same tick is a bug; drop the second rather than write a tape that will
  // not load.
  return tape_->Push(pending_);
}

void Recorder::CaptureFrom(LevelSeam& seam) {
  for (const EntityId& id : seam.AllEntities()) {
    EntitySnapshot snap;
    if (!seam.GetEntity(id, &snap)) continue;
    if (!AddEntity(snap)) break;
  }
}

//----------------------------------------------------------------------------
// ReplaySeam
//----------------------------------------------------------------------------

ReplaySeam::ReplaySeam(const Recording* tape) : tape_(tape) {}

const RecordedFrame* ReplaySeam::current() const {
  if (!tape_ || index_ < 0) return nullptr;
  if ((size_t)index_ >= tape_->frameCount()) return nullptr;
  return &tape_->frames()[(size_t)index_];
}

u64 ReplaySeam::currentTick() const {
  const RecordedFrame* f = current();
  return f ? f->tick : 0;
}

const std::vector<RecordedEvent>* ReplaySeam::currentEvents() const {
  const RecordedFrame* f = current();
  return f ? &f->events : nullptr;
}

bool ReplaySeam::Step() {
  if (!tape_) return false;
  if (index_ < (i64)tape_->frameCount()) ++index_;
  return index_ < (i64)tape_->frameCount();
}

bool ReplaySeam::SeekTo(size_t index) {
  if (!tape_ || index >= tape_->frameCount()) return false;
  index_ = (i64)index;
  return true;
}

void ReplaySeam::Rewind() { index_ = -1; }

bool ReplaySeam::GetEntity(EntityId id, EntitySnapshot* out) const {
  const RecordedFrame* f = current();
  if (!f) return false;
  for (const auto& e : f->entities) {
    if (e.id.raw == id.raw && e.id.runtime == id.runtime) {
      if (out) *out = e;
      return true;
    }
  }
  return false;
}

std::vector<EntityId> ReplaySeam::AllEntities() const {
  std::vector<EntityId> out;
  const RecordedFrame* f = current();
  if (!f) return out;
  out.reserve(f->entities.size());
  for (const auto& e : f->entities) out.push_back(e.id);
  return out;
}

std::vector<EntityId> ReplaySeam::EntitiesInAabb(const Aabb& box,
                                                 EntityType filter) const {
  std::vector<EntityId> out;
  const RecordedFrame* f = current();
  if (!f) return out;
  for (const auto& e : f->entities) {
    if (filter != EntityType::Unknown && e.type != filter) continue;
    if (box.Contains(e.position)) out.push_back(e.id);
  }
  return out;
}

std::vector<EntityId> ReplaySeam::EntitiesInRadius(const Vec3& center, f32 radius) const {
  std::vector<EntityId> out;
  const RecordedFrame* f = current();
  if (!f) return out;
  const f32 r2 = radius * radius;
  for (const auto& e : f->entities) {
    if (e.position.DistanceSq(center) <= r2) out.push_back(e.id);
  }
  return out;
}

bool ReplaySeam::RaycastEntity(const Vec3& origin, const Vec3& dir, f32 maxDist,
                               EntityId* outId, f32* outDist) const {
  const RecordedFrame* f = current();
  if (!f) return false;
  // A ray-sphere test per entity, sphere radius 0.5 (a player-sized box is
  // close enough for a replay; this is a test fixture, not a physics engine).
  const f32 len = dir.Length();
  if (len <= 0) return false;
  const Vec3 d = dir / len;
  bool hit = false;
  f32 best = maxDist;
  EntityId bestId;
  for (const auto& e : f->entities) {
    const Vec3 to = e.position - origin;
    const f32 proj = to.Dot(d);
    if (proj < 0 || proj > best) continue;
    const f32 perp2 = to.Dot(to) - proj * proj;
    if (perp2 > 0.25f) continue;   // 0.5^2
    best = proj;
    bestId = e.id;
    hit = true;
  }
  if (!hit) return false;
  if (outId) *outId = bestId;
  if (outDist) *outDist = best;
  return true;
}

bool ReplaySeam::RaycastBlock(const Vec3& origin, const Vec3& dir, f32 maxDist,
                              Vec3* outPos, u32* outBlockType) const {
  const RecordedFrame* f = current();
  if (!f) return false;
  const f32 len = dir.Length();
  if (len <= 0) return false;
  const Vec3 d = dir / len;
  bool hit = false;
  f32 best = maxDist;
  RecordedBlock bestB;
  for (const RecordedBlock& b : f->blocks) {
    // Blocks are recorded sparsely: only coordinates a script read. A replay
    // raycasts only what was recorded, so it can never invent a block.
    const Vec3 c((f32)b.x + 0.5f, (f32)b.y + 0.5f, (f32)b.z + 0.5f);
    const Vec3 to = c - origin;
    const f32 proj = to.Dot(d);
    if (proj < 0 || proj > best) continue;
    const f32 perp2 = to.Dot(to) - proj * proj;
    if (perp2 > 0.5f) continue;   // inside the unit block
    best = proj;
    bestB = b;
    hit = true;
  }
  if (!hit) return false;
  if (outPos) *outPos = Vec3((f32)bestB.x, (f32)bestB.y, (f32)bestB.z);
  if (outBlockType) *outBlockType = bestB.block;
  return true;
}

u32 ReplaySeam::GetBlock(const Vec3& worldPos) const {
  const RecordedFrame* f = current();
  if (!f) return 0;
  // Integer coordinates, matching the live seam's contract.
  const i32 x = (i32)std::floor(worldPos.x);
  const i32 y = (i32)std::floor(worldPos.y);
  const i32 z = (i32)std::floor(worldPos.z);
  for (const RecordedBlock& b : f->blocks) {
    if (b.x == x && b.y == y && b.z == z) return b.block;
  }
  return 0;
}

LevelTime ReplaySeam::GetTime() const {
  const RecordedFrame* f = current();
  return f ? f->time : LevelTime{};
}

void ReplayAll(const Recording& tape,
               const std::function<void(ReplaySeam&, const RecordedFrame&)>& fn) {
  ReplaySeam seam(&tape);
  while (seam.Step()) {
    fn(seam, tape.frames()[(size_t)seam.frameIndex()]);
  }
}

}  // namespace Berry
