#pragma once
#include "berry/game.h"
#include "berry/render.h"
#include <array>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry::Test {

//----------------------------------------------------------------------------
// Host test shims.
//
// A fake LevelSeam so World/Combat/Render can operate headlessly. Also a
// CaptureRenderSeam that records everything the Renderer issues, so tests can
// assert on draw calls without a GPU.
//----------------------------------------------------------------------------

class ShimLevel final : public LevelSeam {
 public:
  const char* Name() const override { return "test-shim"; }

  void AddEntity(const EntityId& id, const EntitySnapshot& snap) {
    std::lock_guard<std::mutex> lk(m_);
    entities_[id.raw ? id.raw : id.runtime] = snap;
  }
  void RemoveEntity(const EntityId& id) {
    std::lock_guard<std::mutex> lk(m_);
    entities_.erase(id.raw ? id.raw : id.runtime);
  }

  bool GetEntity(EntityId id, EntitySnapshot* out) const override {
    std::lock_guard<std::mutex> lk(m_);
    auto it = entities_.find(id.raw ? id.raw : id.runtime);
    if (it == entities_.end()) return false;
    if (out) *out = it->second;
    return true;
  }

  std::vector<EntityId> AllEntities() const override {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<EntityId> out;
    for (auto& [k, v] : entities_) {
      out.push_back(v.id);
    }
    return out;
  }

  std::vector<EntityId> EntitiesInAabb(const Aabb& box,
                                       EntityType filter) const override {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<EntityId> out;
    for (auto& [k, v] : entities_) {
      if (filter != EntityType::Unknown && v.type != filter) continue;
      if (box.Contains(v.position)) out.push_back(v.id);
    }
    return out;
  }

  std::vector<EntityId> EntitiesInRadius(const Vec3& c, f32 radius) const override {
    std::lock_guard<std::mutex> lk(m_);
    std::vector<EntityId> out;
    for (auto& [k, v] : entities_) {
      if (v.alive && (v.position - c).Length() <= radius) out.push_back(v.id);
    }
    return out;
  }

  bool RaycastEntity(const Vec3& origin, const Vec3& dir, f32 maxDist,
                     EntityId* outId, f32* outDist) const override {
    std::lock_guard<std::mutex> lk(m_);
    EntityId best;
    f32 bestD = maxDist;
    bool hit = false;
    for (auto& [k, v] : entities_) {
      if (!v.alive) continue;
      Vec3 to = v.position - origin;
      f32 t = to.Dot(dir);
      if (t < 0 || t > bestD) continue;
      Vec3 closest = origin + dir * t;
      f32 d = (closest - v.position).Length();
      if (d < 1.0f) {   // treat as hit within 1 block of center
        best = v.id;
        bestD = t;
        hit = true;
      }
    }
    if (hit) {
      if (outId) *outId = best;
      if (outDist) *outDist = bestD;
      return true;
    }
    return false;
  }

  bool RaycastBlock(const Vec3& origin, const Vec3& dir, f32 maxDist,
                    Vec3* outPos, u32* outBlockType) const override {
    // No block simulation in the shim.
    (void)origin; (void)dir; (void)maxDist; (void)outPos; (void)outBlockType;
    return false;
  }

  u32 GetBlock(const Vec3& worldPos) const override {
    std::lock_guard<std::mutex> lk(blockM_);
    auto it = blocks_.find(BlockKey(worldPos));
    return it == blocks_.end() ? 0 : it->second;
  }
  bool SetBlock(const Vec3& worldPos, u32 blockType) override {
    std::lock_guard<std::mutex> lk(blockM_);
    blocks_[BlockKey(worldPos)] = blockType;
    return true;
  }

  LevelTime GetTime() const override { return time_; }
  void SetTime(const LevelTime& t) override { time_ = t; }

  bool SendMessage(const char* message) override {
    std::lock_guard<std::mutex> lk(m_);
    messages_.push_back(message ? message : "");
    return true;
  }

  bool ApplyDamage(EntityId target, f32 amount, DamageCause cause,
                   EntityId source) override {
    std::lock_guard<std::mutex> lk(m_);
    auto it = entities_.find(target.raw ? target.raw : target.runtime);
    if (it == entities_.end()) return false;
    auto& e = it->second;
    e.health -= amount;
    if (e.health <= 0) e.alive = false;
    (void)cause; (void)source;
    return true;
  }

  const std::vector<std::string>& Messages() const { return messages_; }

 private:
  static std::array<i32, 3> BlockKey(const Vec3& p) {
    return { (i32)std::floor(p.x), (i32)std::floor(p.y), (i32)std::floor(p.z) };
  }

  mutable std::mutex m_;
  std::map<u64, EntitySnapshot> entities_;
  mutable std::mutex blockM_;
  std::map<std::array<i32, 3>, u32> blocks_;
  LevelTime time_;
  std::vector<std::string> messages_;
};

class CaptureRenderSeam final : public RenderSeam {
 public:
  size_t lines = 0, texts = 0, rects = 0, circles = 0;
  Vec2 lastScreen;
  std::vector<std::string> textContents;

  void Present(const std::vector<Line2D>& l,
               const std::vector<Text2D>& t,
               const std::vector<Rect2D>& r,
               const std::vector<Circle2D>& c,
               const Vec2& screenSize) override {
    lines = l.size();
    texts = t.size();
    rects = r.size();
    circles = c.size();
    lastScreen = screenSize;
    textContents.clear();
    for (auto& tx : t) textContents.push_back(tx.text);
  }
};

}  // namespace Berry::Test