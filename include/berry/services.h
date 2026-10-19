#pragma once
#include "berry/export.h"
#include "berry/types.h"
#include "berry/module.h"
#include "berry/combat.h"
#include "berry/render.h"
#include "berry/game.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <mutex>

namespace Berry {

//----------------------------------------------------------------------------
// Built-in strict module factories + shared services.
//
// The engine factory `CreateDefaultModules()` returns the four strict modules
// (World, Combat, Render, Network) in dependency order so the registry can
// attach them safely. Each returns a BerryModule whose Name() matches the
// Berry:: namespace it owns.
//----------------------------------------------------------------------------

BERRY_API std::shared_ptr<BerryModule> CreateWorldModule();
BERRY_API std::shared_ptr<BerryModule> CreateCombatModule(std::shared_ptr<Combat> combat = nullptr);
BERRY_API std::shared_ptr<BerryModule> CreateRenderModule(std::shared_ptr<Renderer> renderer = nullptr);
BERRY_API std::shared_ptr<BerryModule> CreateNetworkModule();

/// Convenience: registers the four strict modules in order.
BERRY_API std::vector<std::shared_ptr<BerryModule>> CreateDefaultModules();

//----------------------------------------------------------------------------
// WorldHandle — the typed service owned by the Berry::World module.
// Sibling modules (Render for ESP, Combat for entity targeting) pull this
// from the registry without reaching into the module internals.
//----------------------------------------------------------------------------

class BERRY_API WorldHandle {
 public:
  virtual ~WorldHandle() = default;
  virtual void SetSeam(std::shared_ptr<LevelSeam> seam) = 0;
  virtual std::shared_ptr<LevelSeam> Seam() const = 0;
  virtual void RefreshCache() = 0;
  virtual std::vector<EntityId> CachedEntities() const = 0;
  virtual EntitySnapshot GetEntity(EntityId id) const = 0;
  virtual void SetTime(const LevelTime& t) = 0;
  virtual LevelTime GetTime() const = 0;
  virtual bool SetBlock(const Vec3& p, u32 blockType) = 0;
  virtual u32 GetBlock(const Vec3& p) const = 0;
  virtual bool SendMessage(const char* msg) = 0;
  virtual void SetValue(const char* key, const char* value) = 0;
  virtual const char* GetValue(const char* key, const char* fallback) const = 0;
  virtual bool HasValue(const char* key) const = 0;
};

//----------------------------------------------------------------------------
// Component fans — typed accessors pulled from the registry. Modules use
// these to reach sibling services without breaking namespace isolation.
//----------------------------------------------------------------------------

inline Combat* CombatFrom(ModuleRegistry& reg) {
  auto mod = reg.FindRaw(BERRY_MOD_COMBAT);
  return mod ? mod->Service<Combat>() : nullptr;
}

inline Renderer* RendererFrom(ModuleRegistry& reg) {
  auto mod = reg.FindRaw(BERRY_MOD_RENDER);
  return mod ? mod->Service<Renderer>() : nullptr;
}

inline WorldHandle* WorldFrom(ModuleRegistry& reg) {
  auto mod = reg.FindRaw(BERRY_MOD_WORLD);
  return mod ? mod->Service<WorldHandle>() : nullptr;
}

}  // namespace Berry