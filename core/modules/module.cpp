#include "berry/module.h"
#include "berry/event.h"
#include "berry/logger.h"
#include <algorithm>

namespace Berry {

const char* CapabilityName(u32 cap) {
  switch (cap) {
    case kCapCombat:  return "Combat";
    case kCapRender:  return "Render";
    case kCapWorld:   return "World";
    case kCapNetwork: return "Network";
    case kCapFileIO:  return "FileIO";
    default:          return "Unknown";
  }
}

//----------------------------------------------------------------------------
// ModuleRegistry
//----------------------------------------------------------------------------

ModuleRegistry::ModuleRegistry() = default;
ModuleRegistry::~ModuleRegistry() = default;

bool ModuleRegistry::ValidNamespace(const char* name) {
  if (!name || !*name) return false;
  // Minimum: "Berry::X"
  const char* p = name;
  if (strncmp(p, "Berry::", 7) != 0) return false;
  p += 7;
  return *p != 0;
}

bool ModuleRegistry::Register(std::shared_ptr<BerryModule> module) {
  if (!module) return false;
  const char* name = module->Name();
  if (!ValidNamespace(name)) {
    Log::Error("ModuleRegistry: rejecting invalid module name '%s'", name);
    return false;
  }
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& m : modules_) {
    if (strcmp(m->Name(), name) == 0) {
      Log::Error("ModuleRegistry: duplicate module '%s'", name);
      return false;
    }
  }
  modules_.push_back(std::move(module));
  Log::Info("ModuleRegistry: registered %s", name);
  return true;
}

bool ModuleRegistry::Unregister(const char* name) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = std::find_if(modules_.begin(), modules_.end(),
                         [&](const auto& m) { return strcmp(m->Name(), name) == 0; });
  if (it == modules_.end()) return false;
  modules_.erase(it);
  Log::Info("ModuleRegistry: unregistered %s", name);
  return true;
}

std::shared_ptr<BerryModule> ModuleRegistry::FindRaw(const char* name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& m : modules_) {
    if (strcmp(m->Name(), name) == 0) return m;
  }
  return nullptr;
}

std::vector<std::shared_ptr<BerryModule>> ModuleRegistry::All() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return modules_;
}

size_t ModuleRegistry::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return modules_.size();
}

void ModuleRegistry::AttachAll(Engine& e) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& m : modules_) {
    if (!m->Preload()) {
      Log::Warn("ModuleRegistry: %s Preload failed", m->Name());
      continue;
    }
    if (!m->Attach(e)) {
      Log::Warn("ModuleRegistry: %s Attach failed", m->Name());
      continue;
    }
  }
}

void ModuleRegistry::DetachAll(Engine& e) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto it = modules_.rbegin(); it != modules_.rend(); ++it) {
    (*it)->Detach(e);
  }
}

void ModuleRegistry::TickAll(GameTick tick) {
  std::lock_guard<std::mutex> lk(mutex_);
  for (auto& m : modules_) {
    // One span per module per tick, so the flame graph shows which module is
    // eating the tick rather than just that the tick was slow.
    ProfilerScope span(m->Name());
    m->OnTick(tick);
  }
}

}  // namespace Berry