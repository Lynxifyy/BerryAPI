#pragma once
#include "berry/game.h"
#include "berry/render.h"
#include "berry/cosmetic.h"
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

  LevelTime GetTime() const override { return time_; }

  // Test-only mutators (the shim is fake state, not the game). These are NOT
  // part of LevelSeam; the public seam is read-only.
  void SetBlock(const Vec3& worldPos, u32 blockType) {
    std::lock_guard<std::mutex> lk(blockM_);
    blocks_[BlockKey(worldPos)] = blockType;
  }
  void SetTime(const LevelTime& t) { time_ = t; }
  void RecordMessage(const std::string& msg) {
    std::lock_guard<std::mutex> lk(m_);
    messages_.push_back(msg);
  }
  void SetHealth(const EntityId& id, f32 health) {
    std::lock_guard<std::mutex> lk(m_);
    auto key = id.raw ? id.raw : id.runtime;
    auto it = entities_.find(key);
    if (it == entities_.end()) return;
    it->second.health = health;
    it->second.alive = health > 0;
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

//----------------------------------------------------------------------------
// Cosmetic shims: a synthetic skeleton rig and a capturing geometry sink, so
// the cosmetics path runs headless like every other render seam.
//----------------------------------------------------------------------------

class ShimSkeleton final : public CosmeticSkeletonSource {
 public:
  /// When true, no bone resolves — the "asset's bone is gone" path.
  bool failAll = false;

  bool BoneTransform(EntityId entity, Bone bone,
                     CosmeticBoneTransform* out) const override {
    (void)entity;
    if (failAll || bone == Bone::None) return false;
    // A fixed rig at a known spot with a 90-degree yaw, so the attach math is
    // asserted against hand-computed values.
    out->position = Vec3(10.0f, 64.0f, 5.0f);
    out->rotation = Vec3(0.0f, 90.0f, 0.0f);
    out->scale = 1.0f;
    out->valid = true;
    return true;
  }
};

class CaptureCosmeticSeam final : public CosmeticRenderSeam {
 public:
  std::vector<CosmeticDrawItem> items;
  void SubmitGeometry(const CosmeticDrawItem& item) override {
    items.push_back(item);
  }
  void Clear() { items.clear(); }
};

}  // namespace Berry::Test