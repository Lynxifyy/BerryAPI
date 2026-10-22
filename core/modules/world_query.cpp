// Reactive world queries: a per-tick spatial grid with diff events.
// See include/berry/world_query.h for the model.
#include "berry/world_query.h"

#include <algorithm>
#include <cmath>

namespace Berry {

const char* WorldDiffName(WorldDiff d) {
  switch (d) {
    case WorldDiff::Entered: return "entered";
    case WorldDiff::Left: return "left";
    case WorldDiff::Updated: return "updated";
  }
  return "?";
}

//----------------------------------------------------------------------------
// Grid
//----------------------------------------------------------------------------

SpatialGrid::SpatialGrid(f32 cellSize)
    : cell_(cellSize > 0.0f && std::isfinite(cellSize) ? cellSize : kDefaultCell) {}

size_t SpatialGrid::KeyHash::operator()(const Key& k) const {
  // Mix the three axes; a plain xor of coordinates aliases symmetric cells.
  u64 h = 1469598103934665603ull;
  auto mix = [&h](i64 v) {
    h ^= (u64)v;
    h *= 1099511628211ull;
  };
  mix(k.x);
  mix(k.y);
  mix(k.z);
  return (size_t)h;
}

SpatialGrid::Key SpatialGrid::CellOf(const Vec3& p) const {
  // std::floor, so a negative coordinate maps to the cell that contains it
  // (int truncation would fold -0.5 and 0.5 into the same cell).
  return Key{(i64)std::floor(p.x / cell_), (i64)std::floor(p.y / cell_),
             (i64)std::floor(p.z / cell_)};
}

void SpatialGrid::BeginTick(u64 tick) {
  std::lock_guard<std::mutex> lk(mutex_);
  tick_ = tick;
  overflowed_ = false;
  current_.clear();
}

bool SpatialGrid::Add(const EntitySnapshot& snap) {
  std::lock_guard<std::mutex> lk(mutex_);
  if (current_.size() >= kMaxEntities && current_.find(EntityKey(snap.id)) == current_.end()) {
    overflowed_ = true;
    return false;
  }
  current_[EntityKey(snap.id)] = snap;
  return true;
}

std::vector<WorldDiffEvent> SpatialGrid::EndTick() {
  std::lock_guard<std::mutex> lk(mutex_);

  std::vector<WorldDiffEvent> events;

  // Ids of the new tick, sorted, so the diff is deterministic.
  std::vector<u64> nextIds;
  nextIds.reserve(current_.size());
  for (const auto& kv : current_) nextIds.push_back(kv.first);
  std::sort(nextIds.begin(), nextIds.end());

  // Left: in the stable set but not the new one.
  for (u64 key : stableIds_) {
    if (current_.find(key) == current_.end()) {
      if (events.size() < kMaxDiffEvents) {
        WorldDiffEvent e;
        e.kind = WorldDiff::Left;
        e.id = stable_[key].id;
        e.before = stable_[key];
        events.push_back(std::move(e));
      }
    }
  }

  // Entered / Updated, in id order.
  for (u64 key : nextIds) {
    const EntitySnapshot& now = current_[key];
    auto it = stable_.find(key);
    if (it == stable_.end()) {
      if (events.size() < kMaxDiffEvents) {
        WorldDiffEvent e;
        e.kind = WorldDiff::Entered;
        e.id = now.id;
        e.after = now;
        events.push_back(std::move(e));
      }
      continue;
    }
    // Updated when the observable fields changed. Comparing the fields a script
    // can see keeps a moved-but-otherwise-identical entity from being "updated"
    // on every tick.
    const EntitySnapshot& before = it->second;
    const bool changed =
        before.position.x != now.position.x || before.position.y != now.position.y ||
        before.position.z != now.position.z || before.health != now.health ||
        before.maxHealth != now.maxHealth || before.alive != now.alive ||
        before.onGround != now.onGround || before.type != now.type ||
        before.name != now.name;
    if (changed && events.size() < kMaxDiffEvents) {
      WorldDiffEvent e;
      e.kind = WorldDiff::Updated;
      e.id = now.id;
      e.before = before;
      e.after = now;
      events.push_back(std::move(e));
    }
  }

  // Swap the new set into place and rebuild the cell index.
  stable_ = current_;
  stableIds_ = std::move(nextIds);
  cells_.clear();
  for (u64 key : stableIds_) {
    const EntitySnapshot& s = stable_[key];
    cells_[CellOf(s.position)].push_back(key);
  }
  current_.clear();
  return events;
}

bool SpatialGrid::Matches(const EntitySnapshot& s, const WorldQuery& q) {
  if (q.filter != EntityType::Unknown && s.type != q.filter) return false;
  if (q.aliveOnly && !s.alive) return false;
  if (q.within > 0.0f) {
    const f32 dx = s.position.x - q.center.x;
    const f32 dy = s.position.y - q.center.y;
    const f32 dz = s.position.z - q.center.z;
    // Compare squared distances: no sqrt per candidate.
    if (dx * dx + dy * dy + dz * dz > q.within * q.within) return false;
  }
  return true;
}

std::vector<EntitySnapshot> SpatialGrid::Query(const WorldQuery& q) const {
  std::lock_guard<std::mutex> lk(mutex_);

  // Collect candidate keys. With a radius, only the cells the sphere's AABB
  // overlaps are visited; without one, every entity is a candidate.
  std::vector<u64> candidates;
  if (q.within > 0.0f) {
    const Key lo = CellOf(Vec3(q.center.x - q.within, q.center.y - q.within,
                               q.center.z - q.within));
    const Key hi = CellOf(Vec3(q.center.x + q.within, q.center.y + q.within,
                               q.center.z + q.within));
    for (i64 x = lo.x; x <= hi.x; ++x) {
      for (i64 y = lo.y; y <= hi.y; ++y) {
        for (i64 z = lo.z; z <= hi.z; ++z) {
          auto it = cells_.find(Key{x, y, z});
          if (it == cells_.end()) continue;
          candidates.insert(candidates.end(), it->second.begin(), it->second.end());
        }
      }
    }
  } else {
    candidates = stableIds_;
  }

  std::vector<EntitySnapshot> out;
  for (u64 key : candidates) {
    auto it = stable_.find(key);
    if (it == stable_.end()) continue;
    if (!Matches(it->second, q)) continue;
    out.push_back(it->second);
  }

  // A cell visited twice (a large radius spanning many cells cannot repeat one,
  // but a duplicated key could if the caller added the same id twice) would
  // double-count; sort by key and dedupe, then present in a stable order.
  std::sort(out.begin(), out.end(), [](const EntitySnapshot& a, const EntitySnapshot& b) {
    if (a.id.raw != b.id.raw) return a.id.raw < b.id.raw;
    return a.id.runtime < b.id.runtime;
  });
  out.erase(std::unique(out.begin(), out.end(),
                        [](const EntitySnapshot& a, const EntitySnapshot& b) {
                          return a.id.raw == b.id.raw && a.id.runtime == b.id.runtime;
                        }),
            out.end());

  if (q.maxResults > 0 && out.size() > q.maxResults) out.resize(q.maxResults);
  return out;
}

std::vector<EntityId> SpatialGrid::QueryIds(const WorldQuery& q) const {
  const std::vector<EntitySnapshot> snaps = Query(q);
  std::vector<EntityId> ids;
  ids.reserve(snaps.size());
  for (const auto& s : snaps) ids.push_back(s.id);
  return ids;
}

size_t SpatialGrid::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return stable_.size();
}

bool SpatialGrid::Get(EntityId id, EntitySnapshot* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = stable_.find(EntityKey(id));
  if (it == stable_.end()) return false;
  if (out) *out = it->second;
  return true;
}

bool SpatialGrid::overflowed() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return overflowed_;
}

void SpatialGrid::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  current_.clear();
  stable_.clear();
  stableIds_.clear();
  cells_.clear();
  overflowed_ = false;
}

//----------------------------------------------------------------------------
// Diff bus
//----------------------------------------------------------------------------

WorldDiffBus& WorldDiffBus::Instance() {
  static WorldDiffBus bus;
  return bus;
}

WorldDiffBus::ListenerId WorldDiffBus::On(WorldDiffListener fn) {
  std::lock_guard<std::mutex> lk(mutex_);
  const ListenerId id = nextId_++;
  listeners_.push_back(Entry{id, std::move(fn)});
  return id;
}

bool WorldDiffBus::Off(ListenerId id) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = listeners_.begin(); it != listeners_.end(); ++it) {
    if (it->id == id) {
      listeners_.erase(it);
      return true;
    }
  }
  return false;
}

void WorldDiffBus::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  listeners_.clear();
}

size_t WorldDiffBus::Publish(const std::vector<WorldDiffEvent>& events) {
  // Copy the listener list, then call outside the lock: a listener may
  // subscribe or unsubscribe from within its own callback.
  std::vector<WorldDiffListener> local;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    local.reserve(listeners_.size());
    for (const auto& e : listeners_) local.push_back(e.fn);
  }
  size_t delivered = 0;
  for (const auto& e : events) {
    for (const auto& fn : local) {
      if (fn) fn(e);
    }
    ++delivered;
  }
  return delivered;
}

size_t WorldDiffBus::ListenerCount() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return listeners_.size();
}

//----------------------------------------------------------------------------
// Seam driver
//----------------------------------------------------------------------------

std::vector<WorldDiffEvent> UpdateWorldGrid(SpatialGrid& grid, LevelSeam& seam,
                                            u64 tick, WorldDiffBus* bus) {
  grid.BeginTick(tick);
  for (const EntityId& id : seam.AllEntities()) {
    EntitySnapshot snap;
    if (!seam.GetEntity(id, &snap)) continue;
    if (!grid.Add(snap)) break;   // grid full; keep what we have
  }
  std::vector<WorldDiffEvent> diff = grid.EndTick();
  if (bus) bus->Publish(diff);
  return diff;
}

const char* WorldDiffChannel(WorldDiff d) {
  switch (d) {
    case WorldDiff::Entered: return world_event::EntityEntered();
    case WorldDiff::Left: return world_event::EntityLeft();
    case WorldDiff::Updated: return world_event::EntityUpdated();
  }
  return nullptr;
}

std::string WorldDiffPayload(const EntitySnapshot& snap) {
  return std::to_string(snap.id.raw) + ":" + EntityTypeName(snap.type);
}

size_t PublishBlockChangesToEvents(const std::vector<BlockChange>& changes) {
  size_t published = 0;
  for (const BlockChange& c : changes) {
    std::string payload = std::to_string((i64)c.pos.x) + "," +
                          std::to_string((i64)c.pos.y) + "," +
                          std::to_string((i64)c.pos.z) + "," +
                          std::to_string(c.after);
    const char* p = payload.c_str();
    EventBus::Instance().Emit(BlockChangedChannel(), &p);
    ++published;
  }
  return published;
}

size_t PublishWorldDiffToEvents(const std::vector<WorldDiffEvent>& events) {
  size_t published = 0;
  for (const WorldDiffEvent& e : events) {
    const char* channel = WorldDiffChannel(e.kind);
    if (!channel) continue;
    // The payload the listener receives is a copy of a short string, never a
    // pointer into game memory; the EventBus borrows `data` for the duration of
    // the call only, which is how its contract is written.
    std::string payload = WorldDiffPayload(e.kind == WorldDiff::Left ? e.before : e.after);
    const char* p = payload.c_str();
    EventBus::Instance().Emit(channel, &p);
    ++published;
  }
  return published;
}

}  // namespace Berry
