#include "berry/services.h"
#include "berry/module.h"
#include "berry/engine.h"
#include "berry/logger.h"

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
    combat_->Attach(nullptr);
    return true;
  }
  void Detach(Engine& e) override {
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
    renderer_->Attach(nullptr);
    return true;
  }
  void Detach(Engine& e) override {
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
    Log::Info("Berry::Network attached");
    return true;
  }
  void Detach(Engine& e) override {
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
  void SetTime(const LevelTime& t) override { if (seam_) seam_->SetTime(t); }
  LevelTime GetTime() const override { return seam_ ? seam_->GetTime() : LevelTime{}; }
  bool SetBlock(const Vec3& p, u32 b) override { return seam_ && seam_->SetBlock(p, b); }
  u32 GetBlock(const Vec3& p) const override { return seam_ ? seam_->GetBlock(p) : 0; }
  bool SendMessage(const char* m) override { return seam_ && seam_->SendMessage(m); }
  void SetValue(const char* k, const char* v) override { kv_[k] = v; }
  const char* GetValue(const char* k, const char* fb) const override {
    auto it = kv_.find(k);
    return it != kv_.end() ? it->second.c_str() : fb;
  }
  bool HasValue(const char* k) const override { return kv_.find(k) != kv_.end(); }

  // Module lifecycle ---
  bool Attach(Engine& e) override {
    (void)e;
    Log::Info("Berry::World attached (seam: %s)", seam_ ? seam_->Name() : "null");
    return true;
  }
  void Detach(Engine& e) override {
    (void)e;
    Log::Info("Berry::World detached");
  }
  void OnTick(GameTick tick) override {
    if (seam_ && (tick % 20) == 0) RefreshCache();
  }

 private:
  std::shared_ptr<LevelSeam> seam_;
  std::vector<EntityId> cache_;
  std::unordered_map<std::string, std::string> kv_;
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