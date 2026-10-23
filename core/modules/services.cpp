#include "berry/services.h"
#include "berry/module.h"
#include "berry/engine.h"
#include "berry/logger.h"
#include "berry/session_recorder.h"
#include "berry/replay_service.h"
#include "berry/world_query.h"

#include <memory>
#include <unordered_map>
#include <vector>

namespace Berry {

//----------------------------------------------------------------------------
// Berry::Combat wrapper module
//----------------------------------------------------------------------------

class CombatModule : public BerryModule {
 public:
  explicit CombatModule(std::shared_ptr<Combat> c) : combat_(std::move(c)) {
    if (!combat_) combat_ = std::make_shared<Combat>();
    SetServicePtr(combat_.get());
  }

  const char* Name() const override { return BERRY_MOD_COMBAT; }
  u32 Capabilities() const override { return kCapCombat; }

  bool Attach(Engine& e) override {
    (void)e;
    combat_->Attach(nullptr);
    return true;
  }
  void Detach(Engine& e) override {
    (void)e;
    combat_->Detach();
  }

 private:
  std::shared_ptr<Combat> combat_;
};

//----------------------------------------------------------------------------
// Berry::Render wrapper module
//----------------------------------------------------------------------------

class RenderModule : public BerryModule {
 public:
  explicit RenderModule(std::shared_ptr<Renderer> r) : renderer_(std::move(r)) {
    if (!renderer_) renderer_ = std::make_shared<Renderer>();
    SetServicePtr(renderer_.get());
  }

  const char* Name() const override { return BERRY_MOD_RENDER; }
  u32 Capabilities() const override { return kCapRender | kCapWorld; }

  bool Attach(Engine& e) override {
    (void)e;
    renderer_->Attach(nullptr);
    return true;
  }
  void Detach(Engine& e) override {
    (void)e;
    renderer_->Detach();
  }

 private:
  std::shared_ptr<Renderer> renderer_;
};

//----------------------------------------------------------------------------
// Berry::Network wrapper module
//----------------------------------------------------------------------------

class NetworkModule : public BerryModule {
 public:
  NetworkModule() = default;

  const char* Name() const override { return BERRY_MOD_NETWORK; }
  u32 Capabilities() const override { return kCapNetwork; }

  bool Attach(Engine& e) override {
    (void)e;
    Log::Info("Berry::Network attached");
    return true;
  }
  void Detach(Engine& e) override {
    (void)e;
    Log::Info("Berry::Network detached");
  }
};

//----------------------------------------------------------------------------
// Berry::World wrapper module.
//----------------------------------------------------------------------------

class WorldModule : public BerryModule, public WorldHandle {
 public:
  WorldModule() { SetServicePtr(static_cast<WorldHandle*>(this)); }

  const char* Name() const override { return BERRY_MOD_WORLD; }
  u32 Capabilities() const override { return kCapWorld; }

  // WorldHandle ---
  void SetSeam(std::shared_ptr<LevelSeam> seam) override { seam_ = std::move(seam); }
  std::shared_ptr<LevelSeam> Seam() const override { return seam_; }
  void RefreshCache() override {
    if (seam_) cache_ = seam_->AllEntities();
  }
  std::vector<EntityId> CachedEntities() const override { return cache_; }
  EntitySnapshot GetEntity(EntityId id) const override {
    EntitySnapshot s;
    if (seam_ && seam_->GetEntity(id, &s)) return s;
    return s;
  }
  LevelTime GetTime() const override { return seam_ ? seam_->GetTime() : LevelTime{}; }
  u32 GetBlock(const Vec3& p) const override { return seam_ ? seam_->GetBlock(p) : 0; }
  std::string GetBiome(const BlockPos& p) const override {
    return seam_ ? seam_->GetBiome(p) : std::string();
  }
  i32 GetLight(const BlockPos& p) const override {
    return seam_ ? seam_->GetLight(p) : -1;
  }
  ChunkState GetChunkState(const BlockPos& p) const override {
    return seam_ ? seam_->GetChunkState(p) : ChunkState::Unloaded;
  }
  LocalPlayerState GetLocalPlayerState() const override {
    return seam_ ? seam_->GetLocalPlayerState() : LocalPlayerState{};
  }
  void SetValue(const char* k, const char* v) override { kv_[k] = v; }
  const char* GetValue(const char* k, const char* fb) const override {
    auto it = kv_.find(k);
    return it != kv_.end() ? it->second.c_str() : fb;
  }
  bool HasValue(const char* k) const override { return kv_.find(k) != kv_.end(); }

  // Module lifecycle ---
  bool Attach(Engine& e) override {
    engine_ = &e;
    Log::Info("Berry::World attached (seam: %s)", seam_ ? seam_->Name() : "null");
    return true;
  }
  void Detach(Engine& e) override {
    (void)e;
    engine_ = nullptr;
    Log::Info("Berry::World detached");
  }
  void OnTick(GameTick tick) override {
    if (seam_ && (tick % 20) == 0) RefreshCache();
    // Drive the reactive grid from the same seam the read-only queries use.
    // The grid diffs each tick against the previous one and publishes
    // entityEntered / entityLeft / entityUpdated onto the EventBus, so a
    // script's `Events.once` sees a transition without polling. It reads the
    // seam only; it never writes to game state.
    if (seam_) {
      const std::vector<WorldDiffEvent> diff =
          UpdateWorldGrid(grid_, *seam_, (u64)tick);
      if (!diff.empty()) PublishWorldDiffToEvents(diff);
      // Feed the same tick to the session recorder and the replay capture.
      // Recording is off by default, in which case each is a no-op. Both read
      // the seam and the diff already computed above, never game memory.
      if (engine_) {
        engine_->Recorder().CaptureTick((u64)tick, *seam_, diff);
        engine_->Replay().Tick((u64)tick, *seam_, diff);
      }
    }
  }

 private:
  std::shared_ptr<LevelSeam> seam_;
  std::vector<EntityId> cache_;
  std::unordered_map<std::string, std::string> kv_;
  SpatialGrid grid_;
  Engine* engine_ = nullptr;
};

//----------------------------------------------------------------------------
// Factories
//----------------------------------------------------------------------------

std::shared_ptr<BerryModule> CreateWorldModule() {
  return std::make_shared<WorldModule>();
}

std::shared_ptr<BerryModule> CreateCombatModule(std::shared_ptr<Combat> combat) {
  return std::make_shared<CombatModule>(std::move(combat));
}

std::shared_ptr<BerryModule> CreateRenderModule(std::shared_ptr<Renderer> renderer) {
  return std::make_shared<RenderModule>(std::move(renderer));
}

std::shared_ptr<BerryModule> CreateNetworkModule() {
  return std::make_shared<NetworkModule>();
}

std::vector<std::shared_ptr<BerryModule>> CreateDefaultModules() {
  std::vector<std::shared_ptr<BerryModule>> out;
  out.push_back(CreateWorldModule());
  out.push_back(CreateCombatModule());
  out.push_back(CreateRenderModule());
  out.push_back(CreateNetworkModule());
  return out;
}

}  // namespace Berry