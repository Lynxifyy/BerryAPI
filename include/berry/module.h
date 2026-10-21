#pragma once
#include "berry/version.h"
#include "berry/export.h"
#include "berry/types.h"
#include "berry/hooks.h"
#include "berry/sigscan.h"
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <mutex>

namespace Berry {

class Engine;

//----------------------------------------------------------------------------
// Strict module system.
//
// A BerryModule is a sandboxed unit with a unique name in the Berry:: tree,
// a capability mask (which subsystems it can touch), an explicit
// preload/attach/detach lifecycle, and module-local services. The Engine only
// exposes the hooks/signatures/events that the module's capability mask
// allows, so Berry::Combat cannot touch Berry::Network's state.
//----------------------------------------------------------------------------

enum class ModulePhase { Constructed, Preloaded, Attached, Detached };

enum ModuleCapability : u32 {
  kCapNone       = 0,
  kCapCombat     = 1 << 0,
  kCapRender     = 1 << 1,
  kCapWorld      = 1 << 2,
  kCapNetwork    = 1 << 3,
  kCapFileIO     = 1 << 4,
  kCapAll        = 0xFFFFFFFFu,
};

const char* CapabilityName(u32 cap);

class BERRY_API BerryModule {
 public:
  // Strict module API — every module implements these.
  virtual const char* Name() const = 0;                     // e.g. "Berry::Combat"
  virtual u32 Capabilities() const = 0;                     // capability mask
  virtual bool Preload() { return true; }                   // before hooks
  virtual bool Attach(Engine& e) { (void)e; return true; }  // after hooks
  virtual void Detach(Engine& e) { (void)e; }
  virtual void OnTick(GameTick tick) { (void)tick; }        // engine ticks

  /// The primary service object this module owns, if any (e.g. Combat for
  /// Berry::Combat). Used by sibling modules to reach typed functionality.
  void* ServicePtr() const { return service_; }
  template <typename T>
  T* Service() const { return static_cast<T*>(service_); }
  void SetServicePtr(void* p) { service_ = p; }

  virtual ~BerryModule() = default;

 protected:
  void* service_ = nullptr;
};

//----------------------------------------------------------------------------
// ModuleRegistry — strict namespace isolation.
//----------------------------------------------------------------------------

class BERRY_API ModuleRegistry {
 public:
  ModuleRegistry();
  ~ModuleRegistry();

  /// Registers a module instance. Fails if the name already exists or is not
  /// in a valid Berry:: tree namespace.
  bool Register(std::shared_ptr<BerryModule> module);

  bool Unregister(const char* name);

  /// Looks up by glob-ish name (e.g. "Berry::Combat", "Berry::*" not yet).
  template <typename T = BerryModule>
  std::shared_ptr<T> Find(const char* name) const {
    auto m = FindRaw(name);
    return std::dynamic_pointer_cast<T>(m);
  }

  std::vector<std::shared_ptr<BerryModule>> All() const;
  size_t Count() const;

  /// Attaches all modules (in registration order), collecting failures.
  void AttachAll(Engine& e);
  /// Detaches all modules in reverse order.
  void DetachAll(Engine& e);
  /// Runs OnTick on all modules that request ticks.
  void TickAll(GameTick tick);

  std::shared_ptr<BerryModule> FindRaw(const char* name) const;

 private:
  static bool ValidNamespace(const char* name);
  std::vector<std::shared_ptr<BerryModule>> modules_;
  mutable std::mutex mutex_;
};

}  // namespace Berry