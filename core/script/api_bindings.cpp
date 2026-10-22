// Bindings from the declared IDL surface to the C++ services. See
// include/berry/api_bindings.h for the threading contract.
#include "berry/api_bindings.h"

#include "berry/async.h"
#include "berry/engine.h"
#include "berry/event.h"
#include "berry/generated_bindings.h"
#include "berry/jsonrpc_client.h"
#include "berry/logger.h"
#include "berry/ws_client.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace Berry {

//----------------------------------------------------------------------------
// RenderCommandQueue
//----------------------------------------------------------------------------

RenderCommandQueue::RenderCommandQueue(size_t capacity)
    : capacity_(capacity ? capacity : kDefaultCapacity) {}

bool RenderCommandQueue::Post(Command cmd) {
  if (!cmd) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  if (queue_.size() >= capacity_) {
    ++dropped_;
    return false;
  }
  queue_.push_back(std::move(cmd));
  return true;
}

size_t RenderCommandQueue::Drain() {
  // Swap the queue out under the lock, then run the commands unlocked: a
  // command may itself post (or read service state), and holding the lock
  // across arbitrary service calls would serialise unrelated work and invite
  // a deadlock.
  std::deque<Command> batch;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    batch.swap(queue_);
  }
  for (Command& cmd : batch) {
    if (cmd) cmd();
  }
  {
    std::lock_guard<std::mutex> lk(mutex_);
    drained_ += batch.size();
  }
  return batch.size();
}

size_t RenderCommandQueue::Pending() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return queue_.size();
}

u64 RenderCommandQueue::dropped() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return dropped_;
}

u64 RenderCommandQueue::drained() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return drained_;
}

void RenderCommandQueue::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  queue_.clear();
}

size_t RenderCommandQueue::capacity() const { return capacity_; }

//----------------------------------------------------------------------------
// Helpers
//----------------------------------------------------------------------------

namespace {

/// The capability a generated binding requires, as the runtime gate wants it.
///
/// Network bindings are registered kind-level (`network:*`) exactly like the
/// async HTTP binding: the grant names a host, and the per-request host check
/// happens inside `HttpGetBlocking` where the URL (and its redirects) are
/// known. Registering the concrete host here would deny every script that
/// named any host but the first.
Capability CapabilityFor(const api::BindingSpec* spec) {
  Capability cap;
  cap.kind = spec->capability;
  if (spec->capability == CapKind::Network) cap.scope = "*";
  return cap;
}

/// Reads an Rgba from a script value: a table with r/g/b/a fields (each 0..255),
/// or a number packed as 0xRRGGBBAA. Anything else is white. Clamped, because a
/// hostile script must not be able to index out of the colour table.
Rgba ParseRgba(const ScriptValue& v) {
  if (v.IsTable()) {
    auto field = [&](const char* key, u8 fallback) -> u8 {
      const ScriptValue* f = v.Field(key);
      if (!f || !f->IsNumber() || !std::isfinite(f->AsNumber())) return fallback;
      const f64 n = std::floor(f->AsNumber());
      if (n < 0) return 0;
      if (n > 255) return 255;
      return (u8)n;
    };
    return Rgba(field("r", 255), field("g", 255), field("b", 255), field("a", 255));
  }
  if (v.IsNumber() && std::isfinite(v.AsNumber())) {
    const u64 packed = (u64)(int64_t)v.AsNumber();
    return Rgba((u8)((packed >> 24) & 0xFF), (u8)((packed >> 16) & 0xFF),
                (u8)((packed >> 8) & 0xFF), (u8)(packed & 0xFF));
  }
  return Rgba::White();
}

std::string Lower(const std::string& s) {
  std::string out = s;
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  }
  return out;
}

/// A Vec3 as a script table {x,y,z}. The IDL's Vec3 maps to this shape.
ScriptValue Vec3Value(const Vec3& v) {
  ScriptTable t;
  t["x"] = ScriptValue::Num((f64)v.x);
  t["y"] = ScriptValue::Num((f64)v.y);
  t["z"] = ScriptValue::Num((f64)v.z);
  return ScriptValue::Tbl(std::move(t));
}

/// A finite number clamped into [lo, hi]; non-finite input yields `fallback`.
/// Used for coordinates a script feeds to the renderer, so a NaN cannot reach
/// the draw list.
f32 SafeCoord(const ScriptValue& v, f32 fallback = 0.0f) {
  if (!v.IsNumber()) return fallback;
  const f64 n = v.AsNumber();
  if (!std::isfinite(n)) return fallback;
  return (f32)n;
}

/// A profiler flame node as a script table, recursing into children. Children
/// are keyed "0","1",… in the order the profiler sorted them (total time
/// descending), so a script that walks the table sees the expensive branch
/// first without sorting.
ScriptValue FlameToTable(const Profiler::FlameNode& node) {
  ScriptTable t;
  t["name"] = ScriptValue::Str(node.name);
  t["self_ms"] = ScriptValue::Num(node.selfMs);
  t["total_ms"] = ScriptValue::Num(node.totalMs);
  t["count"] = ScriptValue::Num((f64)node.count);
  ScriptTable kids;
  for (size_t i = 0; i < node.children.size(); ++i) {
    kids[std::to_string(i)] = FlameToTable(node.children[i]);
  }
  t["children"] = ScriptValue::Tbl(std::move(kids));
  return ScriptValue::Tbl(std::move(t));
}

/// Named input keys the observer tracks. Returns null for anything else so the
/// caller fails closed rather than guessing a mapping.
const char* KeyField(const std::string& key, bool* out) {
  const std::string k = Lower(key);
  if (k == "w") { *out = true; return "forward"; }
  if (k == "s") { *out = true; return "back"; }
  if (k == "a") { *out = true; return "left"; }
  if (k == "d") { *out = true; return "right"; }
  if (k == "space") { *out = true; return "jump"; }
  if (k == "shift") { *out = true; return "sneak"; }
  if (k == "ctrl") { *out = true; return "sprint"; }
  return nullptr;
}

/// One entity as a script table. `from`, when non-zero, adds a `distance`
/// field measured from that point, so a caller that already knows the origin
/// (the local player) does not recompute it per entity.
ScriptValue EntityTableValue(const EntitySnapshot& s, const Vec3* from = nullptr) {
  ScriptTable t;
  t["id"] = ScriptValue::Num((f64)s.id.raw);
  t["runtime"] = ScriptValue::Num((f64)s.id.runtime);
  t["type"] = ScriptValue::Str(EntityTypeName(s.type));
  t["x"] = ScriptValue::Num((f64)s.position.x);
  t["y"] = ScriptValue::Num((f64)s.position.y);
  t["z"] = ScriptValue::Num((f64)s.position.z);
  t["vx"] = ScriptValue::Num((f64)s.velocity.x);
  t["vy"] = ScriptValue::Num((f64)s.velocity.y);
  t["vz"] = ScriptValue::Num((f64)s.velocity.z);
  t["yaw"] = ScriptValue::Num((f64)s.rotation.y);
  t["pitch"] = ScriptValue::Num((f64)s.rotation.x);
  t["health"] = ScriptValue::Num((f64)s.health);
  t["max_health"] = ScriptValue::Num((f64)s.maxHealth);
  t["alive"] = ScriptValue::Bool(s.alive);
  t["on_ground"] = ScriptValue::Bool(s.onGround);
  t["name"] = ScriptValue::Str(s.name);
  if (from) t["distance"] = ScriptValue::Num((f64)s.position.Distance(*from));
  return ScriptValue::Tbl(std::move(t));
}

/// Parses the type filter an `entities_within` / `nearest` call carries. An
/// empty string or "any" means "no filter" (Unknown). Returns false for an
/// unknown name, so a typo is an error rather than a silent match-everything.
bool ParseTypeFilter(const std::string& raw, EntityType* out) {
  const std::string name = Lower(raw);
  if (name.empty() || name == "any" || name == "*") {
    *out = EntityType::Unknown;
    return true;
  }
  return ParseEntityType(name, out);
}

/// The local player's look direction from its (pitch, yaw) rotation, matching
/// the MCPE convention (yaw about +Y, pitch up positive): the direction a
/// raycast from the eye would follow.
Vec3 LookDirection(const Vec3& rotationDeg) {
  const f32 pitch = rotationDeg.x * 3.14159265358979f / 180.0f;
  const f32 yaw = rotationDeg.y * 3.14159265358979f / 180.0f;
  const f32 cp = std::cos(pitch);
  return Vec3(-std::sin(yaw) * cp, -std::sin(pitch), std::cos(yaw) * cp).Normalized();
}

}  // namespace

//----------------------------------------------------------------------------
// ScriptApi
//----------------------------------------------------------------------------

ScriptApi::ScriptApi() { InitHooks(); }
ScriptApi::ScriptApi(const Services& services) : services_(services) { InitHooks(); }
ScriptApi::~ScriptApi() { RemoveHooks(); }

void ScriptApi::InitHooks() {
  InitCombatCache();
  InitCombatStateHook();
  InitFrameHook();
}

void ScriptApi::InitFrameHook() {
  frameListener_ = EventBus::Instance().On(
      "berry:frame", [this](const EventContext&) {
        // On the render thread (see docs/THREADING.md): only render work runs
        // here. Apply the draw/HUD commands a script posted from the worker
        // BEFORE presenting, so a command issued this frame is drawn this frame.
        DrainOnRenderThread();
        if (services_.hud && services_.renderer) {
          const Vec2 screen = screenSize();
          if (screen.x > 0 && screen.y > 0) {
            services_.hud->Present(*services_.renderer, screen);
          }
        }
        // Cosmetics draw through the same Renderer, after the HUD, so a worn
        // asset presents through the one RenderSeam like every other overlay.
        // DrawLocal needs no skeleton bound: it resolves nothing and returns 0.
        if (services_.cosmetics && services_.renderer) {
          services_.cosmetics->DrawLocal(*services_.renderer, services_.localPlayer);
        }
        // The PvP overlay presents its widget tree through the HUD, then draws
        // its popups/flash through the Renderer. Its animation is advanced from
        // the frame delta so keystroke fades and popups track wall time.
        if (services_.pvp && services_.hud && services_.renderer) {
          const Vec2 screen = screenSize();
          if (screen.x > 0 && screen.y > 0) {
            const u64 now = NowMs();
            f32 dt = 0.0f;
            if (lastFrameMs_ != 0 && now > lastFrameMs_) {
              dt = (f32)(now - lastFrameMs_);
              // A long stall (a breakpoint, a pause) must not fast-forward every
              // animation; clamp the step to a couple of frames' worth.
              if (dt > 100.0f) dt = 100.0f;
            }
            lastFrameMs_ = now;
            services_.pvp->Update(dt, now);
            services_.pvp->Present(*services_.hud, *services_.renderer, screen);
          }
        }
        // The script-module overlay draws last, on top, when a user turned it
        // on. It reads a snapshot, so a module declared this frame is next
        // frame's line rather than a torn draw.
        if (services_.modules && services_.renderer) {
          const Vec2 screen = screenSize();
          if (screen.x > 0 && screen.y > 0) {
            services_.modules->DrawOverlay(*services_.renderer, screen);
          }
        }
        return EventResult::Continue;
      });
}

void ScriptApi::RemoveHooks() {
  if (frameListener_) {
    EventBus::Instance().Off("berry:frame", frameListener_);
    frameListener_ = 0;
  }
  if (combatListener_) {
    EventBus::Instance().Off("berry:combat", combatListener_);
    combatListener_ = 0;
  }
  if (tickListener_) {
    EventBus::Instance().Off("berry:tick", tickListener_);
    tickListener_ = 0;
  }
  if (combatStateListener_) {
    EventBus::Instance().Off("berry:combat", combatStateListener_);
    combatStateListener_ = 0;
  }
}

void ScriptApi::SetServices(const Services& services) {
  std::lock_guard<std::mutex> lk(mutex_);
  services_ = services;
}

bool ScriptApi::Install(const std::string& name,
                        std::function<ScriptValue(
                            ScriptContext&, const std::vector<ScriptValue>&)> body) {
  const api::BindingSpec* spec = api::FindBindingSpec(name);
  if (!spec) {
    // A name that is not in the IDL table must never be installed: the table is
    // the contract, and a binding that bypasses it would be undocumented and
    // ungated.
    Log::Error("ScriptApi: refusing to install unknown binding '%s'", name.c_str());
    return false;
  }
  const int arity = spec->arity;
  bindingsSink_->Register(
      name, CapabilityFor(spec),
      [this, name, arity, body](ScriptContext& ctx,
                                            const std::vector<ScriptValue>& args) {
        // Arity is enforced from the generated spec, not from the body, so a
        // call with the wrong argument count fails closed before any service is
        // touched.
        if ((int)args.size() != arity) {
          ctx.RaiseError(name + ": expected " + std::to_string(arity) +
                         " argument(s), got " + std::to_string(args.size()));
          return ScriptValue::Nil();
        }
        return body(ctx, args);
      });
  return true;
}

size_t ScriptApi::InstallBindings(ScriptBindings* out) {
  if (!out) return 0;
  bindingsSink_ = out;
  installed_.clear();

  auto install = [&](const char* name,
                     std::function<ScriptValue(
                         ScriptContext&, const std::vector<ScriptValue>&)> body) {
    if (Install(name, std::move(body))) installed_.push_back(name);
  };

  //--- Combat (read-only telemetry) ----------------------------------------
  install("combat.recent_kills", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.combat) return ScriptValue::Num(0);
    return ScriptValue::Num((f64)services_.combat->RecentKills().size());
  });

  install("combat.last_hit_damage", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    return ScriptValue::Num((f64)lastHitDamage_.load());
  });

  install("combat.observe", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    const f64 ticks = args[0].AsNumber();
    if (!std::isfinite(ticks) || ticks <= 0) {
      // A non-positive window means "everything observed", which is the useful
      // reading of a missing or zero argument.
      std::lock_guard<std::mutex> lk(combatMutex_);
      return ScriptValue::Num((f64)observed_.size());
    }
    std::lock_guard<std::mutex> lk(combatMutex_);
    if (observed_.empty()) return ScriptValue::Num(0);
    const u64 latest = observed_.back().tick;
    const u64 lo = latest > (u64)ticks ? latest - (u64)ticks : 0;
    size_t n = 0;
    for (const auto& e : observed_) {
      if (e.tick >= lo) ++n;
    }
    return ScriptValue::Num((f64)n);
  });

  //--- CombatState (local combat-state telemetry / CSHR) --------------------
  //
  // Everything here derives from data the client already has. When the service
  // is not wired the reads fail closed with neutral values rather than
  // fabricating combat state.

  auto combatStateOrFail = [this](ScriptContext& ctx, const char* who)
      -> CombatStateTracker* {
    if (!services_.combatState) {
      ctx.RaiseError(std::string(who) + ": combat-state tracker unavailable");
      return nullptr;
    }
    return services_.combatState;
  };

  auto parseCombatButton = [](ScriptContext& ctx, const char* who,
                              const std::string& raw, MouseButton* out) -> bool {
    const std::string b = Lower(raw);
    if (b == "left") { *out = MouseButton::Left; return true; }
    if (b == "right") { *out = MouseButton::Right; return true; }
    ctx.RaiseError(std::string(who) + ": button must be \"left\" or \"right\"");
    return false;
  };

  install("combat_state.max_reach", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.max_reach");
    if (!cs) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)cs->maxReach());
  });

  install("combat_state.set_max_reach", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.set_max_reach");
    if (!cs) return ScriptValue::Bool(false);
    const f64 blocks = args[0].AsNumber();
    if (!std::isfinite(blocks) || blocks <= 0) {
      ctx.RaiseError("combat_state.set_max_reach: blocks must be a finite positive number");
      return ScriptValue::Bool(false);
    }
    return ScriptValue::Bool(cs->SetMaxReach((f32)blocks));
  });

  install("combat_state.last_reach", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.last_reach");
    if (!cs) return ScriptValue::Nil();
    const ReachCheck rc = cs->lastReach();
    ScriptTable t;
    t["valid"] = ScriptValue::Bool(rc.valid);
    t["distance"] = ScriptValue::Num((f64)rc.distance);
    t["max_reach"] = ScriptValue::Num((f64)rc.maxReach);
    t["within_reach"] = ScriptValue::Bool(rc.withinReach);
    t["tick"] = ScriptValue::Num((f64)rc.tick);
    t["attacker"] = ScriptValue::Num((f64)rc.attacker.raw);
    t["victim"] = ScriptValue::Num((f64)rc.victim.raw);
    return ScriptValue::Tbl(std::move(t));
  });

  install("combat_state.reach_violations", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.reach_violations");
    if (!cs) return ScriptValue::Num(0);
    return ScriptValue::Num((f64)cs->reachViolations());
  });

  install("combat_state.hitbox_count", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.hitbox_count");
    if (!cs) return ScriptValue::Num(0);
    const f64 raw = args[0].AsNumber();
    if (!std::isfinite(raw) || raw <= 0) {
      ctx.RaiseError("combat_state.hitbox_count: entity id must be a positive number");
      return ScriptValue::Num(0);
    }
    EntityId id; id.raw = (u64)raw;
    return ScriptValue::Num((f64)cs->HitboxCount(id));
  });

  install("combat_state.hitbox_ticks", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.hitbox_ticks");
    if (!cs) return ScriptValue::Nil();
    const f64 raw = args[0].AsNumber();
    if (!std::isfinite(raw) || raw <= 0) {
      ctx.RaiseError("combat_state.hitbox_ticks: entity id must be a positive number");
      return ScriptValue::Nil();
    }
    EntityId id; id.raw = (u64)raw;
    const std::vector<u64> ticks = cs->HitboxTicks(id);
    ScriptTable t;
    for (size_t i = 0; i < ticks.size(); ++i) {
      t[std::to_string(i)] = ScriptValue::Num((f64)ticks[i]);
    }
    return ScriptValue::Tbl(std::move(t));
  });

  install("combat_state.hitbox_at", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.hitbox_at");
    if (!cs) return ScriptValue::Nil();
    const f64 raw = args[0].AsNumber();
    const f64 tick = args[1].AsNumber();
    if (!std::isfinite(raw) || raw <= 0 || !std::isfinite(tick) || tick < 0) {
      ctx.RaiseError("combat_state.hitbox_at: entity id must be positive and tick non-negative");
      return ScriptValue::Nil();
    }
    EntityId id; id.raw = (u64)raw;
    Aabb box;
    ScriptTable t;
    const bool ok = cs->HitboxAt(id, (u64)tick, &box);
    t["valid"] = ScriptValue::Bool(ok);
    t["min_x"] = ScriptValue::Num((f64)box.min.x);
    t["min_y"] = ScriptValue::Num((f64)box.min.y);
    t["min_z"] = ScriptValue::Num((f64)box.min.z);
    t["max_x"] = ScriptValue::Num((f64)box.max.x);
    t["max_y"] = ScriptValue::Num((f64)box.max.y);
    t["max_z"] = ScriptValue::Num((f64)box.max.z);
    return ScriptValue::Tbl(std::move(t));
  });

  auto swingToTable = [](const SwingSample& s) {
    ScriptTable t;
    t["valid"] = ScriptValue::Bool(s.valid);
    t["delta_ticks"] = ScriptValue::Num((f64)s.deltaTicks);
    t["cps"] = ScriptValue::Num(s.cps);
    t["tick"] = ScriptValue::Num((f64)s.tick);
    t["attacker"] = ScriptValue::Num((f64)s.attacker.raw);
    return ScriptValue::Tbl(std::move(t));
  };

  install("combat_state.swing", [this, combatStateOrFail, swingToTable](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.swing");
    if (!cs) return ScriptValue::Nil();
    const f64 raw = args[0].AsNumber();
    const f64 tick = args[1].AsNumber();
    if (!std::isfinite(raw) || raw <= 0 || !std::isfinite(tick) || tick < 0) {
      ctx.RaiseError("combat_state.swing: entity id must be positive and tick non-negative");
      return ScriptValue::Nil();
    }
    EntityId id; id.raw = (u64)raw;
    return swingToTable(cs->ObserveSwing(id, (u64)tick));
  });

  install("combat_state.last_swing", [this, combatStateOrFail, swingToTable](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.last_swing");
    if (!cs) return ScriptValue::Nil();
    return swingToTable(cs->lastSwing());
  });

  install("combat_state.average_swing_cps", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.average_swing_cps");
    if (!cs) return ScriptValue::Num(0);
    const f64 raw = args[0].AsNumber();
    if (!std::isfinite(raw) || raw <= 0) {
      ctx.RaiseError("combat_state.average_swing_cps: entity id must be a positive number");
      return ScriptValue::Num(0);
    }
    EntityId id; id.raw = (u64)raw;
    return ScriptValue::Num(cs->AverageSwingCps(id));
  });

  install("combat_state.predict_projectile", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    const f64 x = args[0].AsNumber(), y = args[1].AsNumber(), z = args[2].AsNumber();
    const f64 vx = args[3].AsNumber(), vy = args[4].AsNumber(), vz = args[5].AsNumber();
    const f64 gravity = args[6].AsNumber(), ticks = args[7].AsNumber();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        !std::isfinite(vx) || !std::isfinite(vy) || !std::isfinite(vz) ||
        !std::isfinite(gravity) || !std::isfinite(ticks) || ticks < 0) {
      ctx.RaiseError("combat_state.predict_projectile: arguments must be finite and ticks non-negative");
      return Vec3Value(Vec3());
    }
    const Vec3 p = CombatStateTracker::Extrapolate(
        Vec3((f32)x, (f32)y, (f32)z), Vec3((f32)vx, (f32)vy, (f32)vz),
        (f32)gravity, (u32)ticks);
    return Vec3Value(p);
  });

  install("combat_state.note_projectile", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.note_projectile");
    if (!cs) return ScriptValue::Nil();
    const f64 raw = args[0].AsNumber();
    const f64 x = args[1].AsNumber(), y = args[2].AsNumber(), z = args[3].AsNumber();
    const f64 tick = args[4].AsNumber();
    if (!std::isfinite(raw) || raw <= 0 || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || !std::isfinite(tick) || tick < 0) {
      ctx.RaiseError("combat_state.note_projectile: id must be positive and coordinates/tick finite");
      return ScriptValue::Nil();
    }
    EntityId id; id.raw = (u64)raw;
    cs->NoteProjectile(id, Vec3((f32)x, (f32)y, (f32)z), (u64)tick);
    return ScriptValue::Nil();
  });

  install("combat_state.resolve_projectile", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.resolve_projectile");
    if (!cs) return ScriptValue::Num(-1);
    const f64 raw = args[0].AsNumber();
    const f64 x = args[1].AsNumber(), y = args[2].AsNumber(), z = args[3].AsNumber();
    const f64 tick = args[4].AsNumber();
    if (!std::isfinite(raw) || raw <= 0 || !std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || !std::isfinite(tick) || tick < 0) {
      ctx.RaiseError("combat_state.resolve_projectile: id must be positive and coordinates/tick finite");
      return ScriptValue::Num(-1);
    }
    EntityId id; id.raw = (u64)raw;
    return ScriptValue::Num((f64)cs->ResolveProjectile(id, Vec3((f32)x, (f32)y, (f32)z), (u64)tick));
  });

  install("combat_state.prediction_error", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.prediction_error");
    if (!cs) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)cs->MeanPredictionError());
  });

  install("combat_state.prediction_count", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.prediction_count");
    if (!cs) return ScriptValue::Num(0);
    return ScriptValue::Num((f64)cs->PredictionCount());
  });

  install("combat_state.predict_hit", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.predict_hit");
    if (!cs) return ScriptValue::Nil();
    const f64 attacker = args[0].AsNumber();
    const f64 victim = args[1].AsNumber();
    const f64 tick = args[2].AsNumber();
    if (!std::isfinite(attacker) || attacker <= 0 || !std::isfinite(victim) || victim <= 0 ||
        !std::isfinite(tick) || tick < 0) {
      ctx.RaiseError("combat_state.predict_hit: attacker/victim must be positive and tick non-negative");
      return ScriptValue::Nil();
    }
    EntityId a; a.raw = (u64)attacker;
    EntityId v; v.raw = (u64)victim;
    cs->PredictHit(a, v, (u64)tick);
    return ScriptValue::Nil();
  });

  install("combat_state.hit_accuracy", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.hit_accuracy");
    if (!cs) return ScriptValue::Num(0);
    return ScriptValue::Num(cs->HitAccuracy());
  });

  install("combat_state.predicted_hits", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.predicted_hits");
    if (!cs) return ScriptValue::Num(0);
    return ScriptValue::Num((f64)cs->PredictedHits());
  });

  install("combat_state.confirmed_hits", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.confirmed_hits");
    if (!cs) return ScriptValue::Num(0);
    return ScriptValue::Num((f64)cs->ConfirmedHits());
  });

  install("combat_state.missed_hits", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.missed_hits");
    if (!cs) return ScriptValue::Num(0);
    return ScriptValue::Num((f64)cs->MissedHits());
  });

  install("combat_state.note_click", [this, combatStateOrFail, parseCombatButton](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.note_click");
    if (!cs) return ScriptValue::Bool(false);
    MouseButton button;
    if (!parseCombatButton(ctx, "combat_state.note_click", args[0].ToDisplay(), &button)) {
      return ScriptValue::Bool(false);
    }
    const f64 nowMs = args[1].AsNumber();
    if (!std::isfinite(nowMs) || nowMs < 0) {
      ctx.RaiseError("combat_state.note_click: now_ms must be non-negative");
      return ScriptValue::Bool(false);
    }
    return ScriptValue::Bool(cs->NoteClick(button, (u64)nowMs));
  });

  install("combat_state.click_rate", [this, combatStateOrFail, parseCombatButton](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.click_rate");
    if (!cs) return ScriptValue::Num(0);
    MouseButton button;
    if (!parseCombatButton(ctx, "combat_state.click_rate", args[0].ToDisplay(), &button)) {
      return ScriptValue::Num(0);
    }
    return ScriptValue::Num(cs->ClickRate(button));
  });

  install("combat_state.click_limited", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.click_limited");
    if (!cs) return ScriptValue::Bool(false);
    return ScriptValue::Bool(cs->ClickLimited());
  });

  install("combat_state.click_tokens", [this, combatStateOrFail, parseCombatButton](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.click_tokens");
    if (!cs) return ScriptValue::Num(0);
    MouseButton button;
    if (!parseCombatButton(ctx, "combat_state.click_tokens", args[0].ToDisplay(), &button)) {
      return ScriptValue::Num(0);
    }
    return ScriptValue::Num((f64)cs->ClickTokens(button));
  });

  install("combat_state.set_click_budget", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.set_click_budget");
    if (!cs) return ScriptValue::Bool(false);
    const f64 perSecond = args[0].AsNumber();
    const f64 burst = args[1].AsNumber();
    if (!std::isfinite(perSecond) || perSecond <= 0 ||
        !std::isfinite(burst) || burst < 1) {
      ctx.RaiseError("combat_state.set_click_budget: per_second must be positive and burst at least 1");
      return ScriptValue::Bool(false);
    }
    return ScriptValue::Bool(cs->SetClickBudget((f32)perSecond, (f32)burst));
  });

  install("combat_state.clear", [this, combatStateOrFail](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    CombatStateTracker* cs = combatStateOrFail(ctx, "combat_state.clear");
    if (cs) cs->Clear();
    return ScriptValue::Nil();
  });

  //--- Player (read-only view of the LOCAL player) -------------------------
  //
  // Every read resolves the local player through the World seam and fails
  // closed when it is not bound: a script sees "no player" rather than a
  // fabricated or another entity's state. This is the client's own state.
  auto localSnapshot = [this](ScriptContext& ctx, const char* who,
                              EntitySnapshot* out) -> bool {
    if (!services_.world) {
      ctx.RaiseError(std::string(who) + ": world unavailable");
      return false;
    }
    auto seam = services_.world->Seam();
    if (!seam) {
      ctx.RaiseError(std::string(who) + ": no level seam bound");
      return false;
    }
    const EntityId id = services_.localPlayer;
    if (!id.raw && !id.runtime) {
      ctx.RaiseError(std::string(who) + ": no local player bound");
      return false;
    }
    if (!seam->GetEntity(id, out)) {
      ctx.RaiseError(std::string(who) + ": local player not present");
      return false;
    }
    return true;
  };

  install("player.position", [this, localSnapshot](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    EntitySnapshot s;
    if (!localSnapshot(ctx, "player.position", &s)) return Vec3Value(Vec3());
    return Vec3Value(s.position);
  });

  install("player.rotation", [this, localSnapshot](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    EntitySnapshot s;
    if (!localSnapshot(ctx, "player.rotation", &s)) return Vec3Value(Vec3());
    return Vec3Value(s.rotation);
  });

  install("player.velocity", [this, localSnapshot](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    EntitySnapshot s;
    if (!localSnapshot(ctx, "player.velocity", &s)) return Vec3Value(Vec3());
    return Vec3Value(s.velocity);
  });

  install("player.health", [this, localSnapshot](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    EntitySnapshot s;
    if (!localSnapshot(ctx, "player.health", &s)) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)s.health);
  });

  install("player.max_health", [this, localSnapshot](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    EntitySnapshot s;
    if (!localSnapshot(ctx, "player.max_health", &s)) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)s.maxHealth);
  });

  install("player.alive", [this, localSnapshot](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    EntitySnapshot s;
    if (!localSnapshot(ctx, "player.alive", &s)) return ScriptValue::Bool(false);
    return ScriptValue::Bool(s.alive);
  });

  install("player.name", [this, localSnapshot](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    EntitySnapshot s;
    if (!localSnapshot(ctx, "player.name", &s)) return ScriptValue::Str("");
    return ScriptValue::Str(s.name);
  });

  // The local player's own extended state (hunger, armour, gamemode, effects).
  // Resolved through the same seam as the reads above and fail-closed: an
  // unbound seam or a seam that does not report the state raises and returns a
  // neutral value, so a script sees "no data", never a fabricated stat.
  auto localState = [this](ScriptContext& ctx, const char* who,
                           LocalPlayerState* out) -> bool {
    if (!services_.world) {
      ctx.RaiseError(std::string(who) + ": world unavailable");
      return false;
    }
    auto seam = services_.world->Seam();
    if (!seam) {
      ctx.RaiseError(std::string(who) + ": no level seam bound");
      return false;
    }
    const EntityId id = services_.localPlayer;
    if (!id.raw && !id.runtime) {
      ctx.RaiseError(std::string(who) + ": no local player bound");
      return false;
    }
    *out = seam->GetLocalPlayerState();
    if (!out->valid) {
      ctx.RaiseError(std::string(who) + ": local player state unavailable");
      return false;
    }
    return true;
  };

  install("player.hunger", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.hunger", &st)) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)st.hunger);
  });

  install("player.saturation", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.saturation", &st)) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)st.saturation);
  });

  install("player.armor", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.armor", &st)) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)st.armorPoints);
  });

  install("player.gamemode", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.gamemode", &st)) return ScriptValue::Str("unknown");
    return ScriptValue::Str(GameModeName(st.gamemode));
  });

  install("player.dimension", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.dimension", &st)) return ScriptValue::Str("unknown");
    return ScriptValue::Str(DimensionName(st.dimension));
  });

  install("player.xp_level", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.xp_level", &st)) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)st.xpLevel);
  });

  install("player.xp_progress", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.xp_progress", &st)) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)st.xpProgress);
  });

  install("player.air", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.air", &st)) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)st.air);
  });

  install("player.on_fire", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.on_fire", &st)) return ScriptValue::Bool(false);
    return ScriptValue::Bool(st.onFire);
  });

  install("player.sneaking", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.sneaking", &st)) return ScriptValue::Bool(false);
    return ScriptValue::Bool(st.sneaking);
  });

  install("player.sprinting", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.sprinting", &st)) return ScriptValue::Bool(false);
    return ScriptValue::Bool(st.sprinting);
  });

  install("player.effects", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.effects", &st)) return ScriptValue::Nil();
    ScriptTable out;
    for (size_t i = 0; i < st.effects.size(); ++i) {
      const PlayerEffect& e = st.effects[i];
      ScriptTable fx;
      fx["name"] = ScriptValue::Str(e.name);
      fx["amplifier"] = ScriptValue::Num((f64)e.amplifier);
      fx["seconds"] = ScriptValue::Num((f64)e.secondsLeft);
      fx["ambient"] = ScriptValue::Bool(e.ambient);
      out[std::to_string(i)] = ScriptValue::Tbl(std::move(fx));
    }
    out["count"] = ScriptValue::Num((f64)st.effects.size());
    return ScriptValue::Tbl(std::move(out));
  });

  install("player.state", [this, localState](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    LocalPlayerState st;
    if (!localState(ctx, "player.state", &st)) return ScriptValue::Nil();
    ScriptTable t;
    t["hunger"] = ScriptValue::Num((f64)st.hunger);
    t["saturation"] = ScriptValue::Num((f64)st.saturation);
    t["armor"] = ScriptValue::Num((f64)st.armorPoints);
    t["gamemode"] = ScriptValue::Str(GameModeName(st.gamemode));
    t["dimension"] = ScriptValue::Str(DimensionName(st.dimension));
    t["xp_level"] = ScriptValue::Num((f64)st.xpLevel);
    t["xp_progress"] = ScriptValue::Num((f64)st.xpProgress);
    t["air"] = ScriptValue::Num((f64)st.air);
    t["on_fire"] = ScriptValue::Bool(st.onFire);
    t["sneaking"] = ScriptValue::Bool(st.sneaking);
    t["sprinting"] = ScriptValue::Bool(st.sprinting);
    ScriptTable fx;
    for (size_t i = 0; i < st.effects.size(); ++i) {
      const PlayerEffect& e = st.effects[i];
      ScriptTable one;
      one["name"] = ScriptValue::Str(e.name);
      one["amplifier"] = ScriptValue::Num((f64)e.amplifier);
      one["seconds"] = ScriptValue::Num((f64)e.secondsLeft);
      one["ambient"] = ScriptValue::Bool(e.ambient);
      fx[std::to_string(i)] = ScriptValue::Tbl(std::move(one));
    }
    fx["count"] = ScriptValue::Num((f64)st.effects.size());
    t["effects"] = ScriptValue::Tbl(std::move(fx));
    return ScriptValue::Tbl(std::move(t));
  });

  //--- Render (client-side draw) -------------------------------------------
  install("render.draw_text", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.renderer) { ctx.RaiseError("render.draw_text: renderer unavailable"); return ScriptValue::Nil(); }
    const f32 x = SafeCoord(args[0]);
    const f32 y = SafeCoord(args[1]);
    const std::string text = args[2].ToDisplay();
    const Rgba color = ParseRgba(args[3]);
    PostRender([this, x, y, text, color] {
      if (services_.renderer) services_.renderer->DrawText(Vec2(x, y), text.c_str(), color);
    });
    return ScriptValue::Nil();
  });

  install("render.draw_text_centered", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.renderer) { ctx.RaiseError("render.draw_text_centered: renderer unavailable"); return ScriptValue::Nil(); }
    const f32 x = SafeCoord(args[0]);
    const f32 y = SafeCoord(args[1]);
    const std::string text = args[2].ToDisplay();
    const Rgba color = ParseRgba(args[3]);
    PostRender([this, x, y, text, color] {
      if (services_.renderer) {
        services_.renderer->DrawTextCentered(Vec2(x, y), text.c_str(), color, 1.0f);
      }
    });
    return ScriptValue::Nil();
  });

  install("render.draw_rect", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.renderer) { ctx.RaiseError("render.draw_rect: renderer unavailable"); return ScriptValue::Nil(); }
    const f32 x = SafeCoord(args[0]);
    const f32 y = SafeCoord(args[1]);
    const f32 w = SafeCoord(args[2]);
    const f32 h = SafeCoord(args[3]);
    const Rgba color = ParseRgba(args[4]);
    PostRender([this, x, y, w, h, color] {
      if (!services_.renderer) return;
      Rect2D rect;
      rect.min = Vec2(x, y);
      rect.max = Vec2(x + w, y + h);
      rect.fill = color;
      rect.filled = true;
      services_.renderer->DrawRect(rect);
    });
    return ScriptValue::Nil();
  });

  install("render.draw_line", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.renderer) { ctx.RaiseError("render.draw_line: renderer unavailable"); return ScriptValue::Nil(); }
    const f32 x1 = SafeCoord(args[0]);
    const f32 y1 = SafeCoord(args[1]);
    const f32 x2 = SafeCoord(args[2]);
    const f32 y2 = SafeCoord(args[3]);
    const Rgba color = ParseRgba(args[4]);
    PostRender([this, x1, y1, x2, y2, color] {
      if (services_.renderer) {
        services_.renderer->DrawLine(Vec2(x1, y1), Vec2(x2, y2), color);
      }
    });
    return ScriptValue::Nil();
  });

  install("render.draw_circle", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.renderer) { ctx.RaiseError("render.draw_circle: renderer unavailable"); return ScriptValue::Nil(); }
    const f32 x = SafeCoord(args[0]);
    const f32 y = SafeCoord(args[1]);
    // A negative radius would draw nothing meaningful and can confuse the
    // seam's segment math, so clamp it up to zero.
    const f32 radius = std::max(0.0f, SafeCoord(args[2]));
    const Rgba color = ParseRgba(args[3]);
    PostRender([this, x, y, radius, color] {
      if (services_.renderer) services_.renderer->DrawCircle(Vec2(x, y), radius, color);
    });
    return ScriptValue::Nil();
  });

  install("render.screen_size", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    const Vec2 s = screenSize();
    return Vec3Value(Vec3(s.x, s.y, 0.0f));
  });

  install("render.set_zoom", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    const f64 factor = args[0].AsNumber();
    if (!std::isfinite(factor) || factor <= 0) {
      ctx.RaiseError("render.set_zoom: factor must be positive");
      return ScriptValue::Nil();
    }
    // Zoom is a client-side camera intent. A server profile that disables the
    // camera group must genuinely disable it, so the intent layer is consulted
    // before the value is stored. An unbound validator is permissive here
    // (the policy layer is opt-in), unlike the read-only intent.* bindings.
    if (services_.intents &&
        !services_.intents->Allowed(IntentKind::Zoom)) {
      ctx.RaiseError("render.set_zoom: zoom disabled by server policy");
      return ScriptValue::Nil();
    }
    // Zoom is client-side camera state; the camera consumer reads it back via
    // zoomFactor(). Clamped to a sane multiplier so a script cannot divide the
    // projection by ~zero.
    zoom_.store((f32)std::min<f64>(std::max<f64>(factor, 0.01), 100.0));
    return ScriptValue::Nil();
  });

  //--- Render: world-space projection, 3D geometry and clipping ------------
  //
  // The 3D helpers project through the camera the render thread published and
  // queue the same 2D primitives, so they go through the one RenderSeam. The
  // camera is read from the Renderer (not the script thread's own copy), so a
  // script projects against exactly the view the frame is drawn with.
  install("render.world_to_screen", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.renderer) { ctx.RaiseError("render.world_to_screen: renderer unavailable"); return ScriptValue::Nil(); }
    const Vec3 world(SafeCoord(args[0]), SafeCoord(args[1]), SafeCoord(args[2]));
    const CameraState cam = services_.renderer->camera();
    ScriptTable t;
    Vec2 p;
    const bool visible = cam.valid && cam.WorldToScreen(world, &p);
    t["visible"] = ScriptValue::Bool(visible);
    t["x"] = ScriptValue::Num(visible ? (f64)p.x : 0.0);
    t["y"] = ScriptValue::Num(visible ? (f64)p.y : 0.0);
    // The eye is the inverse of the view matrix's translation column; distance
    // is measured from there so a caller can sort overlays by depth.
    const Vec3 eye(-(cam.view.m[0] * cam.view.m[12] + cam.view.m[1] * cam.view.m[13] +
                     cam.view.m[2] * cam.view.m[14]),
                   -(cam.view.m[4] * cam.view.m[12] + cam.view.m[5] * cam.view.m[13] +
                     cam.view.m[6] * cam.view.m[14]),
                   -(cam.view.m[8] * cam.view.m[12] + cam.view.m[9] * cam.view.m[13] +
                     cam.view.m[10] * cam.view.m[14]));
    t["distance"] = ScriptValue::Num((f64)(cam.valid ? world.Distance(eye) : 0.0f));
    return ScriptValue::Tbl(std::move(t));
  });

  install("render.draw_3d_line", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.renderer) { ctx.RaiseError("render.draw_3d_line: renderer unavailable"); return ScriptValue::Nil(); }
    const Vec3 a(SafeCoord(args[0]), SafeCoord(args[1]), SafeCoord(args[2]));
    const Vec3 b(SafeCoord(args[3]), SafeCoord(args[4]), SafeCoord(args[5]));
    const Rgba color = ParseRgba(args[6]);
    PostRender([this, a, b, color] {
      if (services_.renderer) services_.renderer->Draw3DLine(a, b, color);
    });
    return ScriptValue::Nil();
  });

  install("render.draw_3d_box", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.renderer) { ctx.RaiseError("render.draw_3d_box: renderer unavailable"); return ScriptValue::Nil(); }
    const Vec3 mn(SafeCoord(args[0]), SafeCoord(args[1]), SafeCoord(args[2]));
    const Vec3 mx(SafeCoord(args[3]), SafeCoord(args[4]), SafeCoord(args[5]));
    const Rgba color = ParseRgba(args[6]);
    PostRender([this, mn, mx, color] {
      if (services_.renderer) services_.renderer->Draw3DBox(mn, mx, color);
    });
    return ScriptValue::Nil();
  });

  install("render.draw_3d_text", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.renderer) { ctx.RaiseError("render.draw_3d_text: renderer unavailable"); return ScriptValue::Nil(); }
    const Vec3 world(SafeCoord(args[0]), SafeCoord(args[1]), SafeCoord(args[2]));
    const std::string text = args[3].ToDisplay();
    const Rgba color = ParseRgba(args[4]);
    PostRender([this, world, text, color] {
      if (services_.renderer) services_.renderer->Draw3DText(world, text.c_str(), color);
    });
    return ScriptValue::Nil();
  });

  install("render.push_clip_rect", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.renderer) { ctx.RaiseError("render.push_clip_rect: renderer unavailable"); return ScriptValue::Nil(); }
    const f32 x = SafeCoord(args[0]);
    const f32 y = SafeCoord(args[1]);
    const f32 w = SafeCoord(args[2]);
    const f32 h = SafeCoord(args[3]);
    PostRender([this, x, y, w, h] {
      if (!services_.renderer) return;
      Rect2D r;
      r.min = Vec2(x, y);
      r.max = Vec2(x + w, y + h);
      services_.renderer->PushClipRect(r);
    });
    return ScriptValue::Nil();
  });

  install("render.pop_clip_rect", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.renderer) { ctx.RaiseError("render.pop_clip_rect: renderer unavailable"); return ScriptValue::Nil(); }
    PostRender([this] {
      if (services_.renderer) services_.renderer->PopClipRect();
    });
    return ScriptValue::Nil();
  });

  install("render.clip_rect", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.renderer) { ctx.RaiseError("render.clip_rect: renderer unavailable"); return ScriptValue::Nil(); }
    // The clip stack lives on the render thread, but a script's pushes are
    // queued. Reporting the last-known clip from the render thread keeps the
    // read honest (it never fabricates a value the renderer has not applied).
    Rect2D r;
    const bool active = services_.renderer->clipRect(&r);
    ScriptTable t;
    t["active"] = ScriptValue::Bool(active);
    t["x"] = ScriptValue::Num(active ? (f64)r.min.x : 0.0);
    t["y"] = ScriptValue::Num(active ? (f64)r.min.y : 0.0);
    t["w"] = ScriptValue::Num(active ? (f64)(r.max.x - r.min.x) : 0.0);
    t["h"] = ScriptValue::Num(active ? (f64)(r.max.y - r.min.y) : 0.0);
    return ScriptValue::Tbl(std::move(t));
  });

  install("render.camera", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    const CameraState cam = services_.renderer ? services_.renderer->camera() : CameraState();
    // The camera's world position is the inverse of the view matrix's
    // translation; yaw/pitch come from the view's forward axis so a script can
    // reason about the view direction without the matrix.
    const Vec3 eye(-(cam.view.m[0] * cam.view.m[12] + cam.view.m[1] * cam.view.m[13] +
                     cam.view.m[2] * cam.view.m[14]),
                   -(cam.view.m[4] * cam.view.m[12] + cam.view.m[5] * cam.view.m[13] +
                     cam.view.m[6] * cam.view.m[14]),
                   -(cam.view.m[8] * cam.view.m[12] + cam.view.m[9] * cam.view.m[13] +
                     cam.view.m[10] * cam.view.m[14]));
    const Vec3 fwd(-cam.view.m[2], -cam.view.m[6], -cam.view.m[10]);
    const Vec3 angles = fwd.LengthSq() > 0.0f ? fwd.Angle() : Vec3();
    ScriptTable t;
    t["valid"] = ScriptValue::Bool(cam.valid);
    t["x"] = ScriptValue::Num((f64)eye.x);
    t["y"] = ScriptValue::Num((f64)eye.y);
    t["z"] = ScriptValue::Num((f64)eye.z);
    t["pitch"] = ScriptValue::Num((f64)angles.x);
    t["yaw"] = ScriptValue::Num((f64)angles.y);
    return ScriptValue::Tbl(std::move(t));
  });

  //--- World (read-only queries) -------------------------------------------
  install("world.query", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("world.query: world unavailable"); return ScriptValue::Num(0); }
    const f32 within = (f32)args[0].AsNumber();
    if (!std::isfinite(within) || within < 0) {
      ctx.RaiseError("world.query: radius must be a non-negative number");
      return ScriptValue::Num(0);
    }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError("world.query: no level seam bound"); return ScriptValue::Num(0); }
    const EntitySnapshot self = services_.world->GetEntity(services_.localPlayer);
    if (!self.id.raw && !self.id.runtime) {
      // Without a known local player there is no origin to measure "within"
      // against; fail closed rather than pick an arbitrary point.
      ctx.RaiseError("world.query: no local player bound");
      return ScriptValue::Num(0);
    }
    return ScriptValue::Num((f64)seam->EntitiesInRadius(self.position, within).size());
  });

  install("world.block_at", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("world.block_at: world unavailable"); return ScriptValue::Num(0); }
    const Vec3 p((f32)args[0].AsNumber(), (f32)args[1].AsNumber(), (f32)args[2].AsNumber());
    const u32 block = services_.world->GetBlock(p);
    return ScriptValue::Num((f64)block);
  });

  install("world.block_id_at", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("world.block_id_at: world unavailable"); return ScriptValue::Num(0); }
    const Vec3 p((f32)args[0].AsNumber(), (f32)args[1].AsNumber(), (f32)args[2].AsNumber());
    const u32 block = services_.world->GetBlock(p);
    return ScriptValue::Num((f64)block);
  });

  install("world.get_biome", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("world.get_biome: world unavailable"); return ScriptValue::Str(""); }
    if (!services_.world->Seam()) { ctx.RaiseError("world.get_biome: no level seam bound"); return ScriptValue::Str(""); }
    const BlockPos p((i32)args[0].AsNumber(), (i32)args[1].AsNumber(), (i32)args[2].AsNumber());
    return ScriptValue::Str(services_.world->GetBiome(p));
  });

  install("world.get_light", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("world.get_light: world unavailable"); return ScriptValue::Num(-1); }
    if (!services_.world->Seam()) { ctx.RaiseError("world.get_light: no level seam bound"); return ScriptValue::Num(-1); }
    const BlockPos p((i32)args[0].AsNumber(), (i32)args[1].AsNumber(), (i32)args[2].AsNumber());
    return ScriptValue::Num((f64)services_.world->GetLight(p));
  });

  install("world.get_chunk_state", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("world.get_chunk_state: world unavailable"); return ScriptValue::Str("unloaded"); }
    if (!services_.world->Seam()) { ctx.RaiseError("world.get_chunk_state: no level seam bound"); return ScriptValue::Str("unloaded"); }
    const BlockPos p((i32)args[0].AsNumber(), (i32)args[1].AsNumber(), (i32)args[2].AsNumber());
    return ScriptValue::Str(ChunkStateName(services_.world->GetChunkState(p)));
  });

  install("world.entity_count", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.world) { ctx.RaiseError("world.entity_count: world unavailable"); return ScriptValue::Num(0); }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError("world.entity_count: no level seam bound"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)seam->AllEntities().size());
  });

  install("world.time", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.world) { ctx.RaiseError("world.time: world unavailable"); return ScriptValue::Nil(); }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError("world.time: no level seam bound"); return ScriptValue::Nil(); }
    const LevelTime t = seam->GetTime();
    ScriptTable out;
    out["absolute"] = ScriptValue::Num((f64)t.absolute);
    out["daylight"] = ScriptValue::Num((f64)t.daylight);
    out["day"] = ScriptValue::Bool(t.day);
    out["rain"] = ScriptValue::Num((f64)t.rainLevel);
    out["thunder"] = ScriptValue::Num((f64)t.thunderLevel);
    return ScriptValue::Tbl(std::move(out));
  });

  install("world.is_day", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.world) { ctx.RaiseError("world.is_day: world unavailable"); return ScriptValue::Bool(false); }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError("world.is_day: no level seam bound"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(seam->GetTime().day);
  });

  install("world.is_raining", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.world) { ctx.RaiseError("world.is_raining: world unavailable"); return ScriptValue::Bool(false); }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError("world.is_raining: no level seam bound"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(seam->GetTime().rainLevel > 0.0f);
  });

  //--- World: richer read-only queries --------------------------------------
  //
  // These return tables rather than counts so a script can build a HUD from
  // them without a second call per entity. Every one of them resolves through
  // the seam and fails closed when it is unbound, exactly like the scalar
  // reads above: no raw pointer ever leaves the core.
  install("world.entities_within", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("world.entities_within: world unavailable"); return ScriptValue::Nil(); }
    const f32 within = (f32)args[0].AsNumber();
    if (!std::isfinite(within) || within < 0) {
      ctx.RaiseError("world.entities_within: radius must be a non-negative number");
      return ScriptValue::Nil();
    }
    EntityType filter = EntityType::Unknown;
    if (!ParseTypeFilter(args[1].ToDisplay(), &filter)) {
      ctx.RaiseError("world.entities_within: unknown entity type '" + args[1].ToDisplay() + "'");
      return ScriptValue::Nil();
    }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError("world.entities_within: no level seam bound"); return ScriptValue::Nil(); }
    const EntitySnapshot self = services_.world->GetEntity(services_.localPlayer);
    if (!self.id.raw && !self.id.runtime) {
      ctx.RaiseError("world.entities_within: no local player bound");
      return ScriptValue::Nil();
    }
    // The seam returns ids; snapshot each and filter by type here so the shim
    // and the device adapter share one definition of "within and matching".
    std::vector<EntitySnapshot> found;
    for (const EntityId& id : seam->EntitiesInRadius(self.position, within)) {
      EntitySnapshot s;
      if (!seam->GetEntity(id, &s)) continue;
      if (filter != EntityType::Unknown && s.type != filter) continue;
      found.push_back(s);
    }
    std::sort(found.begin(), found.end(), [](const EntitySnapshot& a, const EntitySnapshot& b) {
      if (a.id.raw != b.id.raw) return a.id.raw < b.id.raw;
      return a.id.runtime < b.id.runtime;
    });
    ScriptTable out;
    int n = 0;
    for (const EntitySnapshot& s : found) {
      out[std::to_string(n++)] = EntityTableValue(s, &self.position);
    }
    out["count"] = ScriptValue::Num((f64)found.size());
    return ScriptValue::Tbl(std::move(out));
  });

  // A raycast needs an origin and a direction. Both casts use the local
  // player's eye (position + ~1.6 blocks) and look vector, so a script asks
  // "what am I looking at" without ever naming a direction the client could not
  // already see.
  auto localEye = [this](ScriptContext& ctx, const char* who, Vec3* eye,
                         Vec3* dir) -> bool {
    if (!services_.world) { ctx.RaiseError(std::string(who) + ": world unavailable"); return false; }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError(std::string(who) + ": no level seam bound"); return false; }
    const EntitySnapshot self = services_.world->GetEntity(services_.localPlayer);
    if (!self.id.raw && !self.id.runtime) {
      ctx.RaiseError(std::string(who) + ": no local player bound");
      return false;
    }
    *eye = self.position + Vec3(0.0f, 1.62f, 0.0f);
    *dir = LookDirection(self.rotation);
    return true;
  };

  install("world.raycast_block", [this, localEye](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    const f32 maxDist = (f32)args[0].AsNumber();
    if (!std::isfinite(maxDist) || maxDist <= 0) {
      ctx.RaiseError("world.raycast_block: distance must be positive");
      return ScriptValue::Nil();
    }
    Vec3 eye, dir;
    if (!localEye(ctx, "world.raycast_block", &eye, &dir)) return ScriptValue::Nil();
    auto seam = services_.world->Seam();
    Vec3 pos;
    u32 block = 0;
    const bool hit = seam->RaycastBlock(eye, dir, maxDist, &pos, &block);
    ScriptTable t;
    t["hit"] = ScriptValue::Bool(hit);
    t["x"] = ScriptValue::Num((f64)pos.x);
    t["y"] = ScriptValue::Num((f64)pos.y);
    t["z"] = ScriptValue::Num((f64)pos.z);
    t["block"] = ScriptValue::Num((f64)block);
    return ScriptValue::Tbl(std::move(t));
  });

  install("world.raycast_entity", [this, localEye](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    const f32 maxDist = (f32)args[0].AsNumber();
    if (!std::isfinite(maxDist) || maxDist <= 0) {
      ctx.RaiseError("world.raycast_entity: distance must be positive");
      return ScriptValue::Nil();
    }
    Vec3 eye, dir;
    if (!localEye(ctx, "world.raycast_entity", &eye, &dir)) return ScriptValue::Nil();
    auto seam = services_.world->Seam();
    EntityId id;
    f32 dist = 0;
    const bool hit = seam->RaycastEntity(eye, dir, maxDist, &id, &dist);
    ScriptTable t;
    t["hit"] = ScriptValue::Bool(hit);
    t["id"] = ScriptValue::Num((f64)id.raw);
    t["runtime"] = ScriptValue::Num((f64)id.runtime);
    t["distance"] = ScriptValue::Num((f64)dist);
    return ScriptValue::Tbl(std::move(t));
  });

  // A helper that snapshots a set of ids and renders them as one table keyed
  // "0","1",…, sorted by id so the order is stable across ticks. The type
  // filter is applied here so the shim and the device adapter share one
  // definition of "matching".
  auto entitiesToTable = [this](const std::vector<EntityId>& ids,
                                const EntityType& filter,
                                const Vec3& from) -> ScriptValue {
    auto seam = services_.world->Seam();
    std::vector<EntitySnapshot> found;
    for (const EntityId& id : ids) {
      EntitySnapshot s;
      if (!seam->GetEntity(id, &s)) continue;
      if (filter != EntityType::Unknown && s.type != filter) continue;
      found.push_back(s);
    }
    std::sort(found.begin(), found.end(), [](const EntitySnapshot& a, const EntitySnapshot& b) {
      if (a.id.raw != b.id.raw) return a.id.raw < b.id.raw;
      return a.id.runtime < b.id.runtime;
    });
    ScriptTable out;
    int n = 0;
    for (const EntitySnapshot& s : found) {
      out[std::to_string(n++)] = EntityTableValue(s, &from);
    }
    out["count"] = ScriptValue::Num((f64)found.size());
    return ScriptValue::Tbl(std::move(out));
  };

  install("world.entities_in_aabb", [this, entitiesToTable](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("world.entities_in_aabb: world unavailable"); return ScriptValue::Nil(); }
    const Vec3 mn((f32)args[0].AsNumber(), (f32)args[1].AsNumber(), (f32)args[2].AsNumber());
    const Vec3 mx((f32)args[3].AsNumber(), (f32)args[4].AsNumber(), (f32)args[5].AsNumber());
    if (mn.x > mx.x || mn.y > mx.y || mn.z > mx.z) {
      ctx.RaiseError("world.entities_in_aabb: min must not exceed max");
      return ScriptValue::Nil();
    }
    EntityType filter = EntityType::Unknown;
    if (!ParseTypeFilter(args[6].ToDisplay(), &filter)) {
      ctx.RaiseError("world.entities_in_aabb: unknown entity type '" + args[6].ToDisplay() + "'");
      return ScriptValue::Nil();
    }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError("world.entities_in_aabb: no level seam bound"); return ScriptValue::Nil(); }
    const EntitySnapshot self = services_.world->GetEntity(services_.localPlayer);
    return entitiesToTable(seam->EntitiesInAabb(Aabb(mn, mx), filter), filter, self.position);
  });

  install("world.entities_in_radius", [this, entitiesToTable](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("world.entities_in_radius: world unavailable"); return ScriptValue::Nil(); }
    const Vec3 center((f32)args[0].AsNumber(), (f32)args[1].AsNumber(), (f32)args[2].AsNumber());
    const f32 radius = (f32)args[3].AsNumber();
    if (!std::isfinite(radius) || radius < 0) {
      ctx.RaiseError("world.entities_in_radius: radius must be a non-negative number");
      return ScriptValue::Nil();
    }
    EntityType filter = EntityType::Unknown;
    if (!ParseTypeFilter(args[4].ToDisplay(), &filter)) {
      ctx.RaiseError("world.entities_in_radius: unknown entity type '" + args[4].ToDisplay() + "'");
      return ScriptValue::Nil();
    }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError("world.entities_in_radius: no level seam bound"); return ScriptValue::Nil(); }
    return entitiesToTable(seam->EntitiesInRadius(center, radius), filter, center);
  });

  install("world.raycast", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("world.raycast: world unavailable"); return ScriptValue::Nil(); }
    const Vec3 origin((f32)args[0].AsNumber(), (f32)args[1].AsNumber(), (f32)args[2].AsNumber());
    Vec3 dir((f32)args[3].AsNumber(), (f32)args[4].AsNumber(), (f32)args[5].AsNumber());
    const f32 maxDist = (f32)args[6].AsNumber();
    if (!std::isfinite(maxDist) || maxDist <= 0) {
      ctx.RaiseError("world.raycast: distance must be positive");
      return ScriptValue::Nil();
    }
    if (dir.Length() < 1e-6f) {
      ctx.RaiseError("world.raycast: direction must be non-zero");
      return ScriptValue::Nil();
    }
    dir = dir.Normalized();
    const std::string target = Lower(args[7].ToDisplay());
    const bool wantBlock = target == "block" || target == "any";
    const bool wantEntity = target == "entity" || target == "any";
    if (!wantBlock && !wantEntity) {
      ctx.RaiseError("world.raycast: target must be 'block', 'entity' or 'any'");
      return ScriptValue::Nil();
    }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError("world.raycast: no level seam bound"); return ScriptValue::Nil(); }

    ScriptTable t;
    bool hit = false;
    f32 bestDist = maxDist + 1.0f;
    Vec3 blockPos;
    u32 block = 0;
    if (wantBlock && seam->RaycastBlock(origin, dir, maxDist, &blockPos, &block)) {
      hit = true;
      bestDist = (blockPos - origin).Length();
    }
    EntityId entId;
    f32 entDist = 0;
    if (wantEntity && seam->RaycastEntity(origin, dir, maxDist, &entId, &entDist)) {
      // Prefer the nearer of the two hits, so a ray into a wall does not report
      // an entity behind it.
      if (!hit || entDist < bestDist) {
        hit = true;
        t["id"] = ScriptValue::Num((f64)entId.raw);
        t["runtime"] = ScriptValue::Num((f64)entId.runtime);
        t["distance"] = ScriptValue::Num((f64)entDist);
      }
    }
    t["hit"] = ScriptValue::Bool(hit);
    if (hit && !t.count("distance")) {
      t["x"] = ScriptValue::Num((f64)blockPos.x);
      t["y"] = ScriptValue::Num((f64)blockPos.y);
      t["z"] = ScriptValue::Num((f64)blockPos.z);
      t["block"] = ScriptValue::Num((f64)block);
      t["distance"] = ScriptValue::Num((f64)bestDist);
    }
    return ScriptValue::Tbl(std::move(t));
  });

  //--- Entity (read-only, by raw id) ---------------------------------------
  //
  // The id a script carries is the raw engine id: the one an entity table or an
  // event payload holds. Reads fail closed on a zero/unknown id or an unbound
  // seam, so a stale id yields "absent", never another entity's state.
  auto entitySnapshot = [this](ScriptContext& ctx, const char* who,
                               const std::vector<ScriptValue>& args,
                               EntitySnapshot* out) -> bool {
    if (!services_.world) { ctx.RaiseError(std::string(who) + ": world unavailable"); return false; }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError(std::string(who) + ": no level seam bound"); return false; }
    const f64 raw = args[0].AsNumber();
    if (!std::isfinite(raw) || raw <= 0) {
      ctx.RaiseError(std::string(who) + ": entity id must be a positive number");
      return false;
    }
    EntityId id;
    id.raw = (u64)raw;
    // Resolve the runtime id through the seam when the caller only has the raw
    // id: AllEntities gives the (raw, runtime) pairing the engine knows.
    for (const EntityId& cand : seam->AllEntities()) {
      if (cand.raw == id.raw) { id = cand; break; }
    }
    if (!seam->GetEntity(id, out)) {
      ctx.RaiseError(std::string(who) + ": entity not present");
      return false;
    }
    return true;
  };

  install("entity.get", [this, entitySnapshot](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    EntitySnapshot s;
    if (!entitySnapshot(ctx, "entity.get", args, &s)) return ScriptValue::Nil();
    return EntityTableValue(s);
  });

  install("entity.type_name", [this, entitySnapshot](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    EntitySnapshot s;
    if (!entitySnapshot(ctx, "entity.type_name", args, &s)) return ScriptValue::Str("");
    return ScriptValue::Str(EntityTypeName(s.type));
  });

  install("entity.position", [this, entitySnapshot](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    EntitySnapshot s;
    if (!entitySnapshot(ctx, "entity.position", args, &s)) return Vec3Value(Vec3());
    return Vec3Value(s.position);
  });

  install("entity.velocity", [this, entitySnapshot](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    EntitySnapshot s;
    if (!entitySnapshot(ctx, "entity.velocity", args, &s)) return Vec3Value(Vec3());
    return Vec3Value(s.velocity);
  });

  install("entity.health", [this, entitySnapshot](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    EntitySnapshot s;
    if (!entitySnapshot(ctx, "entity.health", args, &s)) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)s.health);
  });

  install("entity.alive", [this, entitySnapshot](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    EntitySnapshot s;
    if (!entitySnapshot(ctx, "entity.alive", args, &s)) return ScriptValue::Bool(false);
    return ScriptValue::Bool(s.alive);
  });

  install("entity.distance_to", [this, entitySnapshot](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    EntitySnapshot s;
    if (!entitySnapshot(ctx, "entity.distance_to", args, &s)) return ScriptValue::Num(-1);
    if (!services_.world) return ScriptValue::Num(-1);
    const EntitySnapshot self = services_.world->GetEntity(services_.localPlayer);
    if (!self.id.raw && !self.id.runtime) {
      ctx.RaiseError("entity.distance_to: no local player bound");
      return ScriptValue::Num(-1);
    }
    return ScriptValue::Num((f64)s.position.Distance(self.position));
  });

  install("entity.nearest", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.world) { ctx.RaiseError("entity.nearest: world unavailable"); return ScriptValue::Num(0); }
    EntityType filter = EntityType::Unknown;
    if (!ParseTypeFilter(args[0].ToDisplay(), &filter)) {
      ctx.RaiseError("entity.nearest: unknown entity type '" + args[0].ToDisplay() + "'");
      return ScriptValue::Num(0);
    }
    auto seam = services_.world->Seam();
    if (!seam) { ctx.RaiseError("entity.nearest: no level seam bound"); return ScriptValue::Num(0); }
    const EntitySnapshot self = services_.world->GetEntity(services_.localPlayer);
    if (!self.id.raw && !self.id.runtime) {
      ctx.RaiseError("entity.nearest: no local player bound");
      return ScriptValue::Num(0);
    }
    EntityId best;
    f32 bestDist = 0;
    bool found = false;
    for (const EntityId& id : seam->AllEntities()) {
      if (id.raw == self.id.raw) continue;   // never "the nearest entity is me"
      EntitySnapshot s;
      if (!seam->GetEntity(id, &s)) continue;
      if (!s.alive) continue;
      if (filter != EntityType::Unknown && s.type != filter) continue;
      const f32 d = s.position.Distance(self.position);
      if (!found || d < bestDist) { best = s.id; bestDist = d; found = true; }
    }
    return ScriptValue::Num(found ? (f64)best.raw : 0.0);
  });

  install("entity.get_hitbox", [this, entitySnapshot](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    EntitySnapshot s;
    if (!entitySnapshot(ctx, "entity.get_hitbox", args, &s)) {
      ScriptTable t;
      t["valid"] = ScriptValue::Bool(false);
      return ScriptValue::Tbl(std::move(t));
    }
    const Aabb box = EntityHitbox(s);
    ScriptTable t;
    t["valid"] = ScriptValue::Bool(true);
    t["min_x"] = ScriptValue::Num((f64)box.min.x);
    t["min_y"] = ScriptValue::Num((f64)box.min.y);
    t["min_z"] = ScriptValue::Num((f64)box.min.z);
    t["max_x"] = ScriptValue::Num((f64)box.max.x);
    t["max_y"] = ScriptValue::Num((f64)box.max.y);
    t["max_z"] = ScriptValue::Num((f64)box.max.z);
    return ScriptValue::Tbl(std::move(t));
  });

  install("entity.trace_line", [this, entitySnapshot](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    EntitySnapshot s;
    if (!entitySnapshot(ctx, "entity.trace_line", args, &s)) {
      ScriptTable t;
      t["hit"] = ScriptValue::Bool(false);
      return ScriptValue::Tbl(std::move(t));
    }
    const Vec3 origin((f32)args[1].AsNumber(), (f32)args[2].AsNumber(), (f32)args[3].AsNumber());
    Vec3 dir((f32)args[4].AsNumber(), (f32)args[5].AsNumber(), (f32)args[6].AsNumber());
    const f32 maxDist = (f32)args[7].AsNumber();
    if (!std::isfinite(maxDist) || maxDist <= 0) {
      ctx.RaiseError("entity.trace_line: distance must be positive");
      return ScriptValue::Nil();
    }
    if (dir.Length() < 1e-6f) {
      ctx.RaiseError("entity.trace_line: direction must be non-zero");
      return ScriptValue::Nil();
    }
    dir = dir.Normalized();
    const Aabb box = EntityHitbox(s);
    f32 dist = 0;
    const bool hit = RayHitsAabb(origin, dir, box, maxDist, &dist);
    ScriptTable t;
    t["hit"] = ScriptValue::Bool(hit);
    t["distance"] = ScriptValue::Num(hit ? (f64)dist : -1.0);
    if (hit) {
      const Vec3 p = origin + dir * dist;
      t["x"] = ScriptValue::Num((f64)p.x);
      t["y"] = ScriptValue::Num((f64)p.y);
      t["z"] = ScriptValue::Num((f64)p.z);
    }
    return ScriptValue::Tbl(std::move(t));
  });

  //--- Network (granted, host-scoped) --------------------------------------
  // Both calls go through the async layer's egress path, so the grant is
  // checked here and on every redirect hop. They return an HttpResult table
  // rather than raising, so a script can branch on the outcome.
  install("network.get", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    const std::string url = args[0].ToDisplay();
    const f64 timeout = args[1].AsNumber();
    return HttpRequestBlocking(ctx, "GET", url, "", (long)timeout,
                               services_.networkPolicy);
  });

  install("network.post", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    const std::string url = args[0].ToDisplay();
    const std::string body = args[1].ToDisplay();
    const f64 timeout = args[2].AsNumber();
    return HttpRequestBlocking(ctx, "POST", url, body, (long)timeout,
                               services_.networkPolicy);
  });

  //--- Ws (script-facing WebSocket client) ---------------------------------
  //
  // The pool owns the sockets and applies the same egress gate the HTTP path
  // uses, so a WebSocket cannot become a way around the SSRF rules. The
  // connect/send/pump calls run on the script worker, where blocking IO is
  // expected.
  install("ws.connect", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.wsPool) { ctx.RaiseError("ws.connect: websocket pool unavailable"); return ScriptValue::Num(0); }
    const std::string url = args[0].ToDisplay();
    const f64 timeout = args[1].AsNumber();
    int t = 8000;
    if (std::isfinite(timeout)) t = (int)timeout;
    if (t < 1000) t = 1000;
    if (t > 60000) t = 60000;
    WsClientError err = WsClientError::Ok;
    const u64 h = services_.wsPool->Connect(url, ctx.granted(), services_.networkPolicy,
                                            t, &err);
    if (h == 0) {
      ctx.RaiseError(std::string("ws.connect: ") + WsClientErrorName(err) +
                     " (" + url + ")");
      return ScriptValue::Num(0);
    }
    return ScriptValue::Num((f64)h);
  });

  install("ws.send_text", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.wsPool) { ctx.RaiseError("ws.send_text: websocket pool unavailable"); return ScriptValue::Bool(false); }
    const u64 handle = (u64)args[0].AsNumber();
    WsClientError err = WsClientError::Ok;
    const bool ok = services_.wsPool->SendText(handle, args[1].ToDisplay(), &err);
    if (!ok) ctx.RaiseError(std::string("ws.send_text: ") + WsClientErrorName(err));
    return ScriptValue::Bool(ok);
  });

  install("ws.send_binary", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.wsPool) { ctx.RaiseError("ws.send_binary: websocket pool unavailable"); return ScriptValue::Bool(false); }
    const u64 handle = (u64)args[0].AsNumber();
    const std::string data = args[1].ToDisplay();
    WsClientError err = WsClientError::Ok;
    const bool ok = services_.wsPool->SendBinary(handle, data.data(), data.size(), &err);
    if (!ok) ctx.RaiseError(std::string("ws.send_binary: ") + WsClientErrorName(err));
    return ScriptValue::Bool(ok);
  });

  install("ws.pump", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.wsPool) { ctx.RaiseError("ws.pump: websocket pool unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.wsPool->Pump());
  });

  install("ws.poll", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.wsPool) { ctx.RaiseError("ws.poll: websocket pool unavailable"); return ScriptValue::Nil(); }
    const u64 handle = (u64)args[0].AsNumber();
    WsClientMessage m;
    const bool got = services_.wsPool->Poll(handle, &m);
    ScriptTable t;
    t["ok"] = ScriptValue::Bool(got);
    t["opcode"] = ScriptValue::Str(got ? m.opcode : std::string());
    t["data"] = ScriptValue::Str(got ? m.data : std::string());
    t["binary"] = ScriptValue::Bool(got && m.binary);
    return ScriptValue::Tbl(std::move(t));
  });

  install("ws.pending", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.wsPool) { ctx.RaiseError("ws.pending: websocket pool unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.wsPool->Pending((u64)args[0].AsNumber()));
  });

  install("ws.is_open", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.wsPool) { ctx.RaiseError("ws.is_open: websocket pool unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.wsPool->IsOpen((u64)args[0].AsNumber()));
  });

  install("ws.close", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.wsPool) { ctx.RaiseError("ws.close: websocket pool unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.wsPool->Close((u64)args[0].AsNumber()));
  });

  install("ws.count", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.wsPool) { ctx.RaiseError("ws.count: websocket pool unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.wsPool->Count());
  });

  install("ws.url", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.wsPool) { ctx.RaiseError("ws.url: websocket pool unavailable"); return ScriptValue::Str(""); }
    return ScriptValue::Str(services_.wsPool->Url((u64)args[0].AsNumber()));
  });

  //--- JsonRpc (JSON-RPC 2.0 client) ---------------------------------------
  install("json_rpc.call", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    const std::string url = args[0].ToDisplay();
    const std::string method = args[1].ToDisplay();
    const std::string params = args[2].ToDisplay();
    const f64 timeout = args[3].AsNumber();
    const JsonRpcOutcome out =
        JsonRpcCallBlocking(ctx, url, method, params, (long)timeout,
                            services_.networkPolicy);
    ScriptTable t;
    t["ok"] = ScriptValue::Bool(out.ok);
    t["status"] = ScriptValue::Num((f64)out.http_status);
    t["error_code"] = ScriptValue::Num((f64)out.error_code);
    t["error_message"] = ScriptValue::Str(
        out.ok ? std::string()
               : (out.error_message.empty() ? out.transport_error : out.error_message));
    if (out.ok) {
      // Decode the result JSON into a table so a script reads fields directly.
      std::string derr;
      ScriptValue result = JsonTextToScriptValue(out.result_json, &derr);
      if (!derr.empty()) {
        // The envelope validated, so this should not happen; fail closed rather
        // than hand back a value the script would trust.
        t["ok"] = ScriptValue::Bool(false);
        t["error_message"] = ScriptValue::Str("json_rpc.call: result decode failed");
        t["result"] = ScriptValue::Nil();
      } else {
        t["result"] = std::move(result);
      }
    } else {
      t["result"] = ScriptValue::Nil();
    }
    return ScriptValue::Tbl(std::move(t));
  });

  //--- HUD (retained-mode widgets) -----------------------------------------
  //
  // create_* returns a script-side handle immediately; the real WidgetId is
  // assigned when the queued command runs on the render thread. Every later
  // call resolves handle -> WidgetId at drain time, so the order the script
  // issued the calls is the order they execute.
  install("hud.create_label", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.hud) { ctx.RaiseError("hud.create_label: hud unavailable"); return ScriptValue::Num(0); }
    const std::string text = args[0].ToDisplay();
    const u64 handle = AllocWidgetHandle();
    PostRender([this, handle, text] {
      if (services_.hud) SetWidgetId(handle, services_.hud->Create(WidgetKind::Label, text));
    });
    return ScriptValue::Num((f64)handle);
  });

  install("hud.create_panel", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.hud) { ctx.RaiseError("hud.create_panel: hud unavailable"); return ScriptValue::Num(0); }
    const f32 x = (f32)args[0].AsNumber();
    const f32 y = (f32)args[1].AsNumber();
    const u64 handle = AllocWidgetHandle();
    PostRender([this, handle, x, y] {
      if (!services_.hud) return;
      const WidgetId id = services_.hud->Create(WidgetKind::Panel);
      services_.hud->SetOffset(id, Vec2(x, y));
      SetWidgetId(handle, id);
    });
    return ScriptValue::Num((f64)handle);
  });

  install("hud.attach", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.hud) { ctx.RaiseError("hud.attach: hud unavailable"); return ScriptValue::Nil(); }
    const u64 handle = (u64)args[0].AsNumber();
    PostRender([this, handle] {
      WidgetId id = 0;
      if (services_.hud && ResolveWidgetId(handle, &id)) services_.hud->SetVisible(id, true);
    });
    return ScriptValue::Nil();
  });

  install("hud.set_visible", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.hud) { ctx.RaiseError("hud.set_visible: hud unavailable"); return ScriptValue::Nil(); }
    const u64 handle = (u64)args[0].AsNumber();
    const bool visible = args[1].Truthy();
    PostRender([this, handle, visible] {
      WidgetId id = 0;
      if (services_.hud && ResolveWidgetId(handle, &id)) services_.hud->SetVisible(id, visible);
    });
    return ScriptValue::Nil();
  });

  install("hud.tween", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.hud) { ctx.RaiseError("hud.tween: hud unavailable"); return ScriptValue::Nil(); }
    const u64 handle = (u64)args[0].AsNumber();
    const std::string prop = Lower(args[1].ToDisplay());
    const f32 to = (f32)args[2].AsNumber();
    const f32 ms = (f32)args[3].AsNumber();
    if (prop != "value" && prop != "opacity") {
      ctx.RaiseError("hud.tween: unknown property '" + prop + "'");
      return ScriptValue::Nil();
    }
    PostRender([this, handle, prop, to, ms] {
      WidgetId id = 0;
      if (!services_.hud || !ResolveWidgetId(handle, &id)) return;
      if (prop == "value") services_.hud->TweenValue(id, to, ms);
      else services_.hud->TweenOpacity(id, to, ms);
    });
    return ScriptValue::Nil();
  });

  //--- Perf (frame timing + quality controls) ------------------------------
  install("perf.frame_time_ms", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    const f32 published = frameTimeMs();
    if (published > 0.0f) return ScriptValue::Num((f64)published);
    if (services_.governor) return ScriptValue::Num((f64)services_.governor->stats().avgMs);
    return ScriptValue::Num(0);
  });

  install("perf.stats", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.governor) { ctx.RaiseError("perf.stats: governor unavailable"); return ScriptValue::Nil(); }
    const FrameStats st = services_.governor->stats();
    ScriptTable out;
    out["avg_ms"] = ScriptValue::Num(st.avgMs);
    out["p95_ms"] = ScriptValue::Num(st.p95Ms);
    out["p99_ms"] = ScriptValue::Num(st.p99Ms);
    out["min_ms"] = ScriptValue::Num(st.minMs);
    out["max_ms"] = ScriptValue::Num(st.maxMs);
    out["fps"] = ScriptValue::Num(st.Fps());
    out["samples"] = ScriptValue::Num((f64)st.samples);
    return ScriptValue::Tbl(std::move(out));
  });

  install("perf.quality", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.governor) { ctx.RaiseError("perf.quality: governor unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.governor->quality().Mean());
  });

  install("perf.set_platform", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.governor) { ctx.RaiseError("perf.set_platform: governor unavailable"); return ScriptValue::Nil(); }
    const std::string name = Lower(args[0].ToDisplay());
    PlatformClass cls;
    if (name == "generic") cls = PlatformClass::Generic;
    else if (name == "mobile-low") cls = PlatformClass::MobileLow;
    else if (name == "mobile-high") cls = PlatformClass::MobileHigh;
    else if (name == "desktop") cls = PlatformClass::Desktop;
    else {
      // An unknown profile name is refused rather than silently mapped to a
      // default: the script asked for something that does not exist.
      ctx.RaiseError("perf.set_platform: unknown profile '" + args[0].ToDisplay() + "'");
      return ScriptValue::Nil();
    }
    PostRender([this, cls] {
      if (services_.governor) services_.governor->SetPlatform(cls);
    });
    return ScriptValue::Nil();
  });

  install("perf.set_target_fps", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.governor) { ctx.RaiseError("perf.set_target_fps: governor unavailable"); return ScriptValue::Nil(); }
    const f64 fps = args[0].AsNumber();
    if (!std::isfinite(fps) || fps <= 0 || fps > 1000) {
      ctx.RaiseError("perf.set_target_fps: fps out of range");
      return ScriptValue::Nil();
    }
    PostRender([this, fps] {
      if (!services_.governor) return;
      GovernorConfig cfg = services_.governor->config();
      cfg.targetFps = (i32)fps;
      services_.governor->Configure(cfg);
    });
    return ScriptValue::Nil();
  });

  install("perf.set_thermal_mode", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.governor) { ctx.RaiseError("perf.set_thermal_mode: governor unavailable"); return ScriptValue::Nil(); }
    const bool enabled = args[0].Truthy();
    PostRender([this, enabled] {
      if (!services_.governor) return;
      // "Thermal mode" pins the governor to its low ceiling, the same action a
      // real Serious thermal report triggers, and clears it when disabled.
      services_.governor->OnThermal(enabled ? ThermalState::Serious : ThermalState::Nominal);
    });
    return ScriptValue::Nil();
  });

  //--- Profiler (flame graph) ----------------------------------------------
  //
  // Built on the module/hook spans in `Profiler`. The tree is aggregated since
  // the last reset, so a script can see where a frame actually goes rather than
  // just the frame time. Read-only: a script inspects the profiler, it cannot
  // fabricate spans.
  install("perf.flame", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.profiler) { ctx.RaiseError("perf.flame: profiler unavailable"); return ScriptValue::Nil(); }
    return FlameToTable(services_.profiler->Flame());
  });

  install("perf.spans", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.profiler) { ctx.RaiseError("perf.spans: profiler unavailable"); return ScriptValue::Nil(); }
    ScriptTable t;
    int i = 0;
    for (const auto& row : services_.profiler->Spans()) {
      ScriptTable e;
      e["name"] = ScriptValue::Str(row.first);
      e["total_ms"] = ScriptValue::Num(row.second.totalMs);
      e["count"] = ScriptValue::Num((f64)row.second.count);
      e["max_ms"] = ScriptValue::Num(row.second.maxMs);
      t[std::to_string(i++)] = ScriptValue::Tbl(std::move(e));
    }
    return ScriptValue::Tbl(std::move(t));
  });

  install("perf.flame_reset", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.profiler) { ctx.RaiseError("perf.flame_reset: profiler unavailable"); return ScriptValue::Nil(); }
    services_.profiler->FlameReset();
    return ScriptValue::Nil();
  });

  //--- Input (local, already-visible state) --------------------------------
  install("input.is_key_down", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    if (!services_.input) return ScriptValue::Bool(false);
    bool dummy = false;
    const char* field = KeyField(args[0].ToDisplay(), &dummy);
    if (!field) return ScriptValue::Bool(false);   // unrecognised key: fail closed
    const InputState& s = services_.input->state();
    const std::string f(field);
    if (f == "forward") return ScriptValue::Bool(s.forward);
    if (f == "back") return ScriptValue::Bool(s.back);
    if (f == "left") return ScriptValue::Bool(s.left);
    if (f == "right") return ScriptValue::Bool(s.right);
    if (f == "jump") return ScriptValue::Bool(s.jump);
    if (f == "sneak") return ScriptValue::Bool(s.sneak);
    return ScriptValue::Bool(s.sprint);
  });

  install("input.clicks_per_second", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.input) return ScriptValue::Num(0);
    return ScriptValue::Num(services_.input->clicksPerSecond(MouseButton::Left));
  });

  install("input.velocity", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    // Read-only local motion, exactly the value the client already renders
    // with. Fail closed: no source (or an unknown velocity) reports zero, so a
    // script sees "stationary" rather than a stale or fabricated vector.
    Vec3 v;
    if (!services_.velocity || !services_.velocity->LocalVelocity(&v)) {
      v = Vec3(0, 0, 0);
    }
    return Vec3Value(v);
  });

  //--- Cosmetic (local loadout; renders on this screen only) ---------------
  //
  // Cosmetics are client-side state a script may switch, exactly like the
  // zoom factor: no world write, no other player is affected. `list` and
  // `presets` return a newline-separated string so the surface needs no array
  // type in the IDL.
  install("cosmetic.list", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.cosmetics) { ctx.RaiseError("cosmetic.list: cosmetics unavailable"); return ScriptValue::Str(""); }
    std::string out;
    for (const auto& asset : CosmeticRegistry::Instance().All()) {
      if (!out.empty()) out += '\n';
      out += asset.id;
    }
    return ScriptValue::Str(out);
  });

  install("cosmetic.equipped", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.cosmetics) { ctx.RaiseError("cosmetic.equipped: cosmetics unavailable"); return ScriptValue::Str(""); }
    const CosmeticSlot slot = CosmeticSlotFromName(Lower(args[0].ToDisplay()));
    if (slot == CosmeticSlot::None) {
      ctx.RaiseError("cosmetic.equipped: unknown slot '" + args[0].ToDisplay() + "'");
      return ScriptValue::Str("");
    }
    return ScriptValue::Str(services_.cosmetics->Equipped(slot));
  });

  install("cosmetic.equip", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.cosmetics) { ctx.RaiseError("cosmetic.equip: cosmetics unavailable"); return ScriptValue::Bool(false); }
    const std::string asset = args[0].ToDisplay();
    CosmeticError err = CosmeticError::Ok;
    if (!services_.cosmetics->Equip(asset, &err)) {
      ctx.RaiseError("cosmetic.equip: " + std::string(CosmeticErrorName(err)) + " (" + asset + ")");
      return ScriptValue::Bool(false);
    }
    return ScriptValue::Bool(true);
  });

  install("cosmetic.unequip", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.cosmetics) { ctx.RaiseError("cosmetic.unequip: cosmetics unavailable"); return ScriptValue::Bool(false); }
    const CosmeticSlot slot = CosmeticSlotFromName(Lower(args[0].ToDisplay()));
    if (slot == CosmeticSlot::None) {
      ctx.RaiseError("cosmetic.unequip: unknown slot '" + args[0].ToDisplay() + "'");
      return ScriptValue::Bool(false);
    }
    return ScriptValue::Bool(services_.cosmetics->Unequip(slot));
  });

  install("cosmetic.save_preset", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.cosmetics) { ctx.RaiseError("cosmetic.save_preset: cosmetics unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.cosmetics->SavePreset(args[0].ToDisplay()));
  });

  install("cosmetic.apply_preset", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.cosmetics) { ctx.RaiseError("cosmetic.apply_preset: cosmetics unavailable"); return ScriptValue::Bool(false); }
    const std::string name = args[0].ToDisplay();
    if (!services_.cosmetics->ApplyPreset(name)) {
      ctx.RaiseError("cosmetic.apply_preset: unknown preset '" + name + "'");
      return ScriptValue::Bool(false);
    }
    return ScriptValue::Bool(true);
  });

  install("cosmetic.presets", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.cosmetics) { ctx.RaiseError("cosmetic.presets: cosmetics unavailable"); return ScriptValue::Str(""); }
    std::string out;
    for (const auto& name : services_.cosmetics->PresetNames()) {
      if (!out.empty()) out += '\n';
      out += name;
    }
    return ScriptValue::Str(out);
  });

  install("cosmetic.set_local_only", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.cosmetics) { ctx.RaiseError("cosmetic.set_local_only: cosmetics unavailable"); return ScriptValue::Nil(); }
    // Turning this off only enables rendering of REMOTE loadouts, and those are
    // still gated on verified ownership; the sync channel is what supplies
    // them. A script cannot widen what a peer owns by flipping this.
    services_.cosmetics->SetLocalOnly(args[0].Truthy());
    return ScriptValue::Nil();
  });

  install("cosmetic.reload_packs", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.packs) { ctx.RaiseError("cosmetic.reload_packs: pack loader unavailable"); return ScriptValue::Num(0); }
    // A failed pack does not stop the others, and every pack is applied
    // atomically; the return value is how many packs loaded cleanly.
    const PackLoadReport report = services_.packs->LoadDirectory();
    return ScriptValue::Num((f64)report.packsLoaded);
  });

  //--- Intent (what may be done on this server) ----------------------------
  //
  // The read side of the intent layer. `intent.allowed` is what a module
  // consults before it acts, so a server profile that disables a group
  // genuinely disables it. Fails closed when no validator is bound: an unknown
  // policy must not read as "allowed".
  install("intent.allowed", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    if (!services_.intents) return ScriptValue::Bool(false);
    const IntentKind k = IntentKindFromName(args[0].AsString());
    return ScriptValue::Bool(services_.intents->Allowed(k));
  });

  install("intent.classify", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    const IntentKind k = IntentKindFromName(args[0].AsString());
    return ScriptValue::Str(IntentClassName(ClassifyIntent(k)));
  });

  install("intent.server", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.intents) return ScriptValue::Str("");
    return ScriptValue::Str(services_.intents->server());
  });

  install("intent.profile", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.intents) return ScriptValue::Str("");
    return ScriptValue::Str(services_.intents->profile().name);
  });

  install("intent.disabled_groups", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.intents) return ScriptValue::Str("");
    return ScriptValue::Str(IntentGroupList(services_.intents->disabledGroups()));
  });

  install("intent.list", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    // Every named intent, comma separated, so a script can enumerate the
    // vocabulary without hard-coding it.
    std::string out;
    for (u32 i = 1; i < (u32)IntentKind::Count; ++i) {
      const IntentKind k = (IntentKind)i;
      if (k == IntentKind::Unknown) continue;
      if (!out.empty()) out += ",";
      out += IntentKindName(k);
    }
    return ScriptValue::Str(out);
  });

  //--- Shader (script-registered post-processing) --------------------------
  //
  // Registration is validated before any GL call; a device that refuses the
  // shader keeps the previous program. Every binding fails closed when no
  // registry is bound.
  install("shader.register", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.shaders) { ctx.RaiseError("shader.register: shader registry unavailable"); return ScriptValue::Bool(false); }
    ShaderSource s;
    s.name = args[0].AsString();
    s.stage = ShaderStage::Fragment;
    s.source = args[1].AsString();
    std::string error;
    const ShaderReject r = services_.shaders->Register(s, &error);
    if (r != ShaderReject::None) {
      ctx.RaiseError(std::string("shader.register: ") + ShaderRejectName(r));
      return ScriptValue::Bool(false);
    }
    // A device compile failure is reported as false, but the previous program
    // (if any) is still in place; surface the reason to the script.
    if (!error.empty()) ctx.RaiseError("shader.register: " + error);
    return ScriptValue::Bool(error.empty());
  });

  install("shader.unregister", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    if (!services_.shaders) return ScriptValue::Bool(false);
    return ScriptValue::Bool(services_.shaders->Unregister(args[0].AsString()));
  });

  install("shader.has", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    if (!services_.shaders) return ScriptValue::Bool(false);
    return ScriptValue::Bool(services_.shaders->Has(args[0].AsString()));
  });

  install("shader.list", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.shaders) return ScriptValue::Str("");
    std::string out;
    for (const auto& n : services_.shaders->Names()) {
      if (!out.empty()) out += ",";
      out += n;
    }
    return ScriptValue::Str(out);
  });

  install("shader.uniforms", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    if (!services_.shaders) return ScriptValue::Str("");
    return ScriptValue::Str(services_.shaders->UniformNames(args[0].AsString()));
  });

  install("shader.enable", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    if (!services_.shaders) return ScriptValue::Bool(false);
    return ScriptValue::Bool(
        services_.shaders->SetEffectEnabled(args[0].AsString(), args[1].Truthy()));
  });

  install("shader.chain", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.shaders) return ScriptValue::Str("");
    return ScriptValue::Str(services_.shaders->ChainNames());
  });

  install("shader.set_param", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    if (!services_.shaders) return ScriptValue::Bool(false);
    const f64 v = args[2].AsNumber();
    if (!std::isfinite(v)) return ScriptValue::Bool(false);
    return ScriptValue::Bool(
        services_.shaders->SetParam(args[0].AsString(), args[1].AsString(), (f32)v));
  });

  install("shader.param", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    if (!services_.shaders) return ScriptValue::Num(0);
    f32 v = 0;
    if (!services_.shaders->Param(args[0].AsString(), args[1].AsString(), &v)) {
      return ScriptValue::Num(0);
    }
    return ScriptValue::Num((f64)v);
  });

  install("shader.clear", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (services_.shaders) services_.shaders->Clear();
    return ScriptValue::Nil();
  });

  //--- Package (registry + resolver) ---------------------------------------
  //
  // Resolution is pure: it reads only the registered manifests, never the disk.
  // Every binding fails closed when no registry is bound.
  install("package.add", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.packages) {
      ctx.RaiseError("package.add: package registry unavailable");
      return ScriptValue::Bool(false);
    }
    std::string error;
    const PackageError pe = services_.packages->AddJson(args[0].AsString(), &error);
    if (pe != PackageError::Ok) {
      ctx.RaiseError(std::string("package.add: ") + PackageErrorName(pe) +
                     (error.empty() ? "" : (": " + error)));
      return ScriptValue::Bool(false);
    }
    return ScriptValue::Bool(true);
  });

  install("package.has", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    if (!services_.packages) return ScriptValue::Bool(false);
    return ScriptValue::Bool(services_.packages->Has(args[0].AsString()));
  });

  install("package.versions", [this](ScriptContext&, const std::vector<ScriptValue>& args) {
    if (!services_.packages) return ScriptValue::Str("");
    std::string out;
    for (const SemVer& v : services_.packages->Versions(args[0].AsString())) {
      if (!out.empty()) out += ",";
      out += v.ToString();
    }
    return ScriptValue::Str(out);
  });

  install("package.resolve", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.packages) {
      ctx.RaiseError("package.resolve: package registry unavailable");
      return ScriptValue::Str("");
    }
    PackageDependency root;
    root.name = args[0].AsString();
    root.raw = args[1].AsString();
    std::string cerr;
    if (!VersionConstraint::Parse(root.raw, &root.constraint, &cerr)) {
      ctx.RaiseError("package.resolve: bad constraint: " + cerr);
      return ScriptValue::Str("");
    }
    const ResolveResult r = services_.packages->Resolve({root});
    if (!r.ok()) {
      ctx.RaiseError(std::string("package.resolve: ") + ResolveErrorName(r.error) +
                     (r.detail.empty() ? "" : (": " + r.detail)));
      return ScriptValue::Str("");
    }
    std::string out;
    for (const auto& rp : r.order) {
      if (!out.empty()) out += ",";
      out += rp.name + "@" + rp.version;
    }
    return ScriptValue::Str(out);
  });

  install("package.require_path", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.packages) {
      ctx.RaiseError("package.require_path: package registry unavailable");
      return ScriptValue::Str("");
    }
    std::string name, entry, error;
    if (!ResolveRequire(*services_.packages, args[0].AsString(), &name, &entry, &error)) {
      ctx.RaiseError("package.require_path: " + error);
      return ScriptValue::Str("");
    }
    return ScriptValue::Str(name + "/" + entry);
  });

  install("package.clear", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (services_.packages) services_.packages->Clear();
    return ScriptValue::Nil();
  });

  //--- Record (record & replay the world stream) ----------------------------
  //
  // The recorder is driven by the World module tick; these bindings only
  // control the session and read the tape. `record.save` returns a whole JSON
  // document a script can hand to `fs.write`, so a failing session is captured
  // for a bug report. Everything fails closed when no recorder is bound.
  install("record.start", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.recorder) { ctx.RaiseError("record.start: recorder unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.recorder->Start());
  });

  install("record.stop", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.recorder) { ctx.RaiseError("record.stop: recorder unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.recorder->Stop());
  });

  install("record.recording", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.recorder) return ScriptValue::Bool(false);
    return ScriptValue::Bool(services_.recorder->recording());
  });

  install("record.frames", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.recorder) return ScriptValue::Num(0);
    return ScriptValue::Num((f64)services_.recorder->frames());
  });

  install("record.overflowed", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.recorder) return ScriptValue::Bool(false);
    return ScriptValue::Bool(services_.recorder->overflowed());
  });

  install("record.save", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.recorder) { ctx.RaiseError("record.save: recorder unavailable"); return ScriptValue::Str(""); }
    return ScriptValue::Str(services_.recorder->Save());
  });

  install("record.load", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.recorder) { ctx.RaiseError("record.load: recorder unavailable"); return ScriptValue::Bool(false); }
    // The whole document is validated before anything is adopted, so a bad tape
    // never half-loads and never disturbs the current one.
    return ScriptValue::Bool(services_.recorder->Load(args[0].ToDisplay()));
  });

  install("record.clear", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (services_.recorder) services_.recorder->Clear();
    return ScriptValue::Nil();
  });

  install("record.replay_step", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.recorder) return ScriptValue::Bool(false);
    return ScriptValue::Bool(services_.recorder->ReplayStep());
  });

  install("record.replay_seek", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.recorder) { ctx.RaiseError("record.replay_seek: recorder unavailable"); return ScriptValue::Bool(false); }
    const f64 n = args[0].AsNumber();
    // A negative or non-finite index cannot name a frame; refuse it rather than
    // casting an out-of-range float to size_t.
    if (!std::isfinite(n) || n < 0) return ScriptValue::Bool(false);
    return ScriptValue::Bool(services_.recorder->ReplaySeek((size_t)n));
  });

  install("record.replay_rewind", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (services_.recorder) services_.recorder->ReplayRewind();
    return ScriptValue::Nil();
  });

  install("record.replay_tick", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.recorder) return ScriptValue::Num(0);
    return ScriptValue::Num((f64)services_.recorder->ReplayTick());
  });

  install("record.replay_index", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.recorder) return ScriptValue::Num(-1);
    return ScriptValue::Num((f64)services_.recorder->ReplayIndex());
  });

  install("record.replay_end", [this](ScriptContext&, const std::vector<ScriptValue>&) {
    if (!services_.recorder) return ScriptValue::Bool(true);
    return ScriptValue::Bool(services_.recorder->ReplayAtEnd());
  });

  //--- Fs (sandboxed file store) --------------------------------------------
  //
  // The store validates every path and enforces the size/count caps, so these
  // bindings only translate values. A refused path or an over-cap write reports
  // false; nothing here throws for a script's own bad input.
  install("fs.write", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.files) { ctx.RaiseError("fs.write: file store unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.files->Write(args[0].ToDisplay(), args[1].ToDisplay()));
  });

  install("fs.read", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.files) { ctx.RaiseError("fs.read: file store unavailable"); return ScriptValue::Str(""); }
    std::string data;
    if (!services_.files->Read(args[0].ToDisplay(), &data)) return ScriptValue::Str("");
    return ScriptValue::Str(data);
  });

  install("fs.append", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.files) { ctx.RaiseError("fs.append: file store unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.files->Append(args[0].ToDisplay(), args[1].ToDisplay()));
  });

  install("fs.exists", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.files) { ctx.RaiseError("fs.exists: file store unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.files->Exists(args[0].ToDisplay()));
  });

  install("fs.remove", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.files) { ctx.RaiseError("fs.remove: file store unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.files->Remove(args[0].ToDisplay()));
  });

  install("fs.size", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.files) { ctx.RaiseError("fs.size: file store unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.files->Size(args[0].ToDisplay()));
  });

  install("fs.list", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.files) { ctx.RaiseError("fs.list: file store unavailable"); return ScriptValue::Str(""); }
    return ScriptValue::Str(services_.files->List(args[0].ToDisplay()));
  });

  install("fs.count", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.files) { ctx.RaiseError("fs.count: file store unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.files->FileCount());
  });

  install("fs.total_bytes", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.files) { ctx.RaiseError("fs.total_bytes: file store unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.files->TotalBytes());
  });

  install("fs.clear", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.files) { ctx.RaiseError("fs.clear: file store unavailable"); return ScriptValue::Nil(); }
    services_.files->Clear();
    return ScriptValue::Nil();
  });

  //--- Audio (client-side cues) ---------------------------------------------
  install("audio.play", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.audio) { ctx.RaiseError("audio.play: audio unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.audio->Play(args[0].ToDisplay(),
                                                   (f32)args[1].AsNumber(),
                                                   (f32)args[2].AsNumber()));
  });

  install("audio.set_volume", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.audio) { ctx.RaiseError("audio.set_volume: audio unavailable"); return ScriptValue::Nil(); }
    services_.audio->SetVolume((f32)args[0].AsNumber());
    return ScriptValue::Nil();
  });

  install("audio.volume", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.audio) { ctx.RaiseError("audio.volume: audio unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.audio->volume());
  });

  install("audio.set_muted", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.audio) { ctx.RaiseError("audio.set_muted: audio unavailable"); return ScriptValue::Nil(); }
    services_.audio->SetMuted(args[0].Truthy());
    return ScriptValue::Nil();
  });

  install("audio.muted", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.audio) { ctx.RaiseError("audio.muted: audio unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.audio->muted());
  });

  install("audio.history", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.audio) { ctx.RaiseError("audio.history: audio unavailable"); return ScriptValue::Str(""); }
    std::string out;
    for (const AudioCue& c : services_.audio->History()) {
      if (!out.empty()) out += '\n';
      out += c.name + ":" + std::to_string(c.volume) + ":" + std::to_string(c.pitch);
    }
    return ScriptValue::Str(out);
  });

  install("audio.history_count", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.audio) { ctx.RaiseError("audio.history_count: audio unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.audio->HistoryCount());
  });

  install("audio.clear_history", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.audio) { ctx.RaiseError("audio.clear_history: audio unavailable"); return ScriptValue::Nil(); }
    services_.audio->ClearHistory();
    return ScriptValue::Nil();
  });

  //--- Pvp (the client-side overlay read-outs) ------------------------------
  //
  // The overlay owns the widgets and turns local data into draw calls; these
  // bindings expose its read-outs and its section switches. Nothing here writes
  // game state: a script reads what a normal client already shows.
  install("pvp.cps", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.pvp) { ctx.RaiseError("pvp.cps: pvp overlay unavailable"); return ScriptValue::Num(0); }
    const std::string b = Lower(args[0].ToDisplay());
    MouseButton button;
    if (b == "left") button = MouseButton::Left;
    else if (b == "right") button = MouseButton::Right;
    else {
      ctx.RaiseError("pvp.cps: button must be \"left\" or \"right\"");
      return ScriptValue::Num(0);
    }
    return ScriptValue::Num(services_.pvp->clicksPerSecond(button));
  });

  install("pvp.combo", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.combo: pvp overlay unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.pvp->combo());
  });

  install("pvp.combo_active", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.combo_active: pvp overlay unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.pvp->comboActive());
  });

  install("pvp.session", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.session: pvp overlay unavailable"); return ScriptValue::Nil(); }
    const SessionStats& s = services_.pvp->session();
    ScriptTable t;
    t["damage_dealt"] = ScriptValue::Num(s.damageDealt);
    t["damage_taken"] = ScriptValue::Num(s.damageTaken);
    t["kills"] = ScriptValue::Num((f64)s.kills);
    t["deaths"] = ScriptValue::Num((f64)s.deaths);
    t["hits"] = ScriptValue::Num((f64)s.hits);
    t["peak_dps"] = ScriptValue::Num((f64)s.peakDps);
    t["kd"] = ScriptValue::Num(s.Kd());
    return ScriptValue::Tbl(std::move(t));
  });

  install("pvp.armor", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.armor: pvp overlay unavailable"); return ScriptValue::Nil(); }
    ScriptTable t;
    int n = 0;
    for (const ArmorPiece& p : services_.pvp->armor()) {
      ScriptTable e;
      e["name"] = ScriptValue::Str(p.name);
      e["durability"] = ScriptValue::Num((f64)p.durability);
      e["max_durability"] = ScriptValue::Num((f64)p.maxDurability);
      e["fraction"] = ScriptValue::Num((f64)p.Fraction());
      t[std::to_string(n++)] = ScriptValue::Tbl(std::move(e));
    }
    return ScriptValue::Tbl(std::move(t));
  });

  install("pvp.effects", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.effects: pvp overlay unavailable"); return ScriptValue::Nil(); }
    ScriptTable t;
    int n = 0;
    for (const EffectState& e : services_.pvp->effects()) {
      ScriptTable s;
      s["name"] = ScriptValue::Str(e.name);
      s["amplifier"] = ScriptValue::Num((f64)e.amplifier);
      s["seconds_left"] = ScriptValue::Num((f64)e.secondsLeft);
      s["ambient"] = ScriptValue::Bool(e.ambient);
      t[std::to_string(n++)] = ScriptValue::Tbl(std::move(s));
    }
    return ScriptValue::Tbl(std::move(t));
  });

  install("pvp.net", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.net: pvp overlay unavailable"); return ScriptValue::Nil(); }
    const NetStats& s = services_.pvp->netStats();
    ScriptTable t;
    t["ping_ms"] = ScriptValue::Num((f64)s.pingMs);
    t["tps"] = ScriptValue::Num((f64)s.tps);
    t["fps"] = ScriptValue::Num((f64)s.fps);
    return ScriptValue::Tbl(std::move(t));
  });

  install("pvp.target", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.target: pvp overlay unavailable"); return ScriptValue::Nil(); }
    const TargetInfo& g = services_.pvp->target();
    ScriptTable t;
    t["name"] = ScriptValue::Str(g.name);
    t["health"] = ScriptValue::Num((f64)g.health);
    t["max_health"] = ScriptValue::Num((f64)g.maxHealth);
    t["distance"] = ScriptValue::Num((f64)g.distance);
    t["armor"] = ScriptValue::Num((f64)g.armorPoints);
    t["valid"] = ScriptValue::Bool(g.valid);
    return ScriptValue::Tbl(std::move(t));
  });

  install("pvp.health", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.health: pvp overlay unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.pvp->health());
  });

  install("pvp.max_health", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.max_health: pvp overlay unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.pvp->maxHealth());
  });

  install("pvp.set_config", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.pvp) { ctx.RaiseError("pvp.set_config: pvp overlay unavailable"); return ScriptValue::Bool(false); }
    // The call fully describes the config: start from all-off, enable the named
    // sections. An unknown key is refused and nothing changes, so a typo cannot
    // silently turn a section off.
    PvpConfig c;
    c.showCps = c.showKeystrokes = c.showCombo = c.showArmor = false;
    c.showEffects = c.showNetStats = c.showTarget = c.showSession = false;
    c.showDamagePopups = c.hitFlash = c.lowHealthVignette = false;

    std::string cur;
    auto apply = [&](const std::string& key) -> bool {
      if (key.empty()) return true;
      if (key == "cps") c.showCps = true;
      else if (key == "keystrokes") c.showKeystrokes = true;
      else if (key == "combo") c.showCombo = true;
      else if (key == "armor") c.showArmor = true;
      else if (key == "effects") c.showEffects = true;
      else if (key == "net") c.showNetStats = true;
      else if (key == "target") c.showTarget = true;
      else if (key == "session") c.showSession = true;
      else if (key == "popups") c.showDamagePopups = true;
      else if (key == "hitflash") c.hitFlash = true;
      else if (key == "vignette") c.lowHealthVignette = true;
      else return false;
      return true;
    };
    bool ok = true;
    for (char ch : args[0].ToDisplay()) {
      if (ch == '\n') {
        if (!apply(cur)) { ok = false; break; }
        cur.clear();
      } else {
        cur += ch;
      }
    }
    if (ok && !apply(cur)) ok = false;
    if (!ok) {
      ctx.RaiseError("pvp.set_config: unknown section key");
      return ScriptValue::Bool(false);
    }
    c.lowHealthThreshold = services_.pvp->config().lowHealthThreshold;
    services_.pvp->SetConfig(c);
    return ScriptValue::Bool(true);
  });

  install("pvp.config", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.config: pvp overlay unavailable"); return ScriptValue::Str(""); }
    const PvpConfig& c = services_.pvp->config();
    std::string out;
    auto add = [&](const char* k, bool on) {
      if (!on) return;
      if (!out.empty()) out += '\n';
      out += k;
    };
    add("cps", c.showCps);
    add("keystrokes", c.showKeystrokes);
    add("combo", c.showCombo);
    add("armor", c.showArmor);
    add("effects", c.showEffects);
    add("net", c.showNetStats);
    add("target", c.showTarget);
    add("session", c.showSession);
    add("popups", c.showDamagePopups);
    add("hitflash", c.hitFlash);
    add("vignette", c.lowHealthVignette);
    return ScriptValue::Str(out);
  });

  install("pvp.attach", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp || !services_.hud) { ctx.RaiseError("pvp.attach: pvp overlay unavailable"); return ScriptValue::Bool(false); }
    services_.pvp->Attach(*services_.hud);
    return ScriptValue::Bool(services_.pvp->attached());
  });

  install("pvp.detach", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.detach: pvp overlay unavailable"); return ScriptValue::Nil(); }
    services_.pvp->Detach();
    return ScriptValue::Nil();
  });

  install("pvp.attached", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.attached: pvp overlay unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.pvp->attached());
  });

  install("pvp.low_health", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.pvp) { ctx.RaiseError("pvp.low_health: pvp overlay unavailable"); return ScriptValue::Bool(false); }
    const f32 maxH = services_.pvp->maxHealth();
    if (maxH <= 0) return ScriptValue::Bool(false);
    const f32 frac = services_.pvp->health() / maxH;
    const PvpConfig& c = services_.pvp->config();
    return ScriptValue::Bool(c.lowHealthVignette && frac < c.lowHealthThreshold);
  });

  //--- Packets (read-only metadata inspection) ------------------------------
  //
  // The log holds headers, never payloads, so this cannot leak anyone's data.
  // A script reads it to diagnose a connection; it can never write to it.
  install("packets.recent", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.packets) { ctx.RaiseError("packets.recent: packet log unavailable"); return ScriptValue::Nil(); }
    const f64 n = args[0].AsNumber();
    // A negative or non-finite count means "all held"; otherwise take the last
    // n records (newest first).
    size_t max = 0;
    if (std::isfinite(n) && n > 0) max = (size_t)n;
    ScriptTable t;
    int i = 0;
    for (const PacketRecord& r : services_.packets->Recent(max)) {
      ScriptTable e;
      e["seq"] = ScriptValue::Num((f64)r.seq);
      e["dir"] = ScriptValue::Str(PacketDirName(r.dir));
      e["id"] = ScriptValue::Num((f64)r.id);
      e["name"] = ScriptValue::Str(r.name);
      e["bytes"] = ScriptValue::Num((f64)r.payloadLen);
      e["time_ms"] = ScriptValue::Num((f64)r.timeMs);
      t[std::to_string(i++)] = ScriptValue::Tbl(std::move(e));
    }
    return ScriptValue::Tbl(std::move(t));
  });

  install("packets.last", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.packets) { ctx.RaiseError("packets.last: packet log unavailable"); return ScriptValue::Nil(); }
    PacketRecord r;
    if (!services_.packets->Last(&r)) return ScriptValue::Nil();
    ScriptTable e;
    e["seq"] = ScriptValue::Num((f64)r.seq);
    e["dir"] = ScriptValue::Str(PacketDirName(r.dir));
    e["id"] = ScriptValue::Num((f64)r.id);
    e["name"] = ScriptValue::Str(r.name);
    e["bytes"] = ScriptValue::Num((f64)r.payloadLen);
    e["time_ms"] = ScriptValue::Num((f64)r.timeMs);
    return ScriptValue::Tbl(std::move(e));
  });

  install("packets.totals", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.packets) { ctx.RaiseError("packets.totals: packet log unavailable"); return ScriptValue::Nil(); }
    const PacketTotals t = services_.packets->totals();
    ScriptTable out;
    out["inbound"] = ScriptValue::Num((f64)t.inbound);
    out["outbound"] = ScriptValue::Num((f64)t.outbound);
    out["inbound_bytes"] = ScriptValue::Num((f64)t.inboundBytes);
    out["outbound_bytes"] = ScriptValue::Num((f64)t.outboundBytes);
    out["dropped"] = ScriptValue::Num((f64)t.dropped);
    out["total"] = ScriptValue::Num((f64)t.total());
    return ScriptValue::Tbl(std::move(out));
  });

  install("packets.summary", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.packets) { ctx.RaiseError("packets.summary: packet log unavailable"); return ScriptValue::Nil(); }
    const std::string d = Lower(args[0].AsString());
    PacketDir dir = PacketDir::Count;
    if (d == "inbound") dir = PacketDir::Inbound;
    else if (d == "outbound") dir = PacketDir::Outbound;
    else if (!d.empty()) {
      ctx.RaiseError("packets.summary: dir must be \"inbound\", \"outbound\" or \"\"");
      return ScriptValue::Nil();
    }
    ScriptTable t;
    int i = 0;
    for (const PacketIdStat& s : services_.packets->Summary(dir)) {
      ScriptTable e;
      e["id"] = ScriptValue::Num((f64)s.id);
      e["name"] = ScriptValue::Str(s.name);
      e["dir"] = ScriptValue::Str(PacketDirName(s.dir));
      e["count"] = ScriptValue::Num((f64)s.count);
      e["bytes"] = ScriptValue::Num((f64)s.bytes);
      t[std::to_string(i++)] = ScriptValue::Tbl(std::move(e));
    }
    return ScriptValue::Tbl(std::move(t));
  });

  install("packets.count_of", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.packets) { ctx.RaiseError("packets.count_of: packet log unavailable"); return ScriptValue::Num(0); }
    const f64 n = args[0].AsNumber();
    if (!std::isfinite(n) || n < 0 || n > 4294967295.0) {
      ctx.RaiseError("packets.count_of: id must be a non-negative 32-bit integer");
      return ScriptValue::Num(0);
    }
    return ScriptValue::Num((f64)services_.packets->CountOf((u32)n));
  });

  install("packets.size", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.packets) { ctx.RaiseError("packets.size: packet log unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.packets->size());
  });

  install("packets.clear", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.packets) { ctx.RaiseError("packets.clear: packet log unavailable"); return ScriptValue::Nil(); }
    services_.packets->Clear();
    return ScriptValue::Nil();
  });

  //--- Mod (script modules + typed settings) --------------------------------
  //
  // The registry is pure data; a script declares its module and settings, the
  // client renders them. Every write is validated against the declared kind and
  // range inside the registry, so a bad value fails closed here too.
  install("mod.create", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.create: module registry unavailable"); return ScriptValue::Num(0); }
    const u64 h = services_.modules->Create(args[0].ToDisplay(), args[1].ToDisplay(),
                                            "", "");
    if (h == 0) {
      // A zero handle means the name was empty or already taken; report why
      // rather than handing back a handle that names nothing.
      ctx.RaiseError("mod.create: module name is empty or already in use");
      return ScriptValue::Num(0);
    }
    return ScriptValue::Num((f64)h);
  });

  install("mod.set_meta", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.set_meta: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->SetMeta((u64)args[0].AsNumber(),
                                                        args[1].ToDisplay(),
                                                        args[2].ToDisplay()));
  });

  install("mod.add_toggle", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.add_toggle: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->AddToggle((u64)args[0].AsNumber(),
                                                          args[1].ToDisplay(),
                                                          args[2].ToDisplay(),
                                                          args[3].Truthy()));
  });

  install("mod.add_slider", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.add_slider: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->AddSlider((u64)args[0].AsNumber(),
                                                          args[1].ToDisplay(),
                                                          args[2].ToDisplay(),
                                                          args[3].AsNumber(),
                                                          args[4].AsNumber(),
                                                          args[5].AsNumber()));
  });

  install("mod.add_select", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.add_select: module registry unavailable"); return ScriptValue::Bool(false); }
    // Options arrive as a newline-separated list, the same shape the settings
    // and module listings use, so a script builds one string either way.
    std::vector<std::string> options;
    std::string cur;
    for (char c : args[3].ToDisplay()) {
      if (c == '\n') { options.push_back(cur); cur.clear(); }
      else cur += c;
    }
    options.push_back(cur);
    return ScriptValue::Bool(services_.modules->AddSelect((u64)args[0].AsNumber(),
                                                          args[1].ToDisplay(),
                                                          args[2].ToDisplay(),
                                                          options,
                                                          (i64)args[4].AsNumber()));
  });

  install("mod.add_keybind", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.add_keybind: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->AddKeybind((u64)args[0].AsNumber(),
                                                           args[1].ToDisplay(),
                                                           args[2].ToDisplay(),
                                                           args[3].ToDisplay()));
  });

  install("mod.add_text", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.add_text: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->AddText((u64)args[0].AsNumber(),
                                                        args[1].ToDisplay(),
                                                        args[2].ToDisplay(),
                                                        args[3].ToDisplay()));
  });

  install("mod.add_button", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.add_button: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->AddButton((u64)args[0].AsNumber(),
                                                          args[1].ToDisplay(),
                                                          args[2].ToDisplay()));
  });

  install("mod.set_number", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.set_number: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->SetNumber((u64)args[0].AsNumber(),
                                                          args[1].ToDisplay(),
                                                          args[2].AsNumber()));
  });

  install("mod.set_text", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.set_text: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->SetText((u64)args[0].AsNumber(),
                                                        args[1].ToDisplay(),
                                                        args[2].ToDisplay()));
  });

  install("mod.press", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.press: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->Press((u64)args[0].AsNumber(),
                                                      args[1].ToDisplay()));
  });

  install("mod.number", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.number: module registry unavailable"); return ScriptValue::Num(0); }
    f64 v = 0;
    if (!services_.modules->GetNumber((u64)args[0].AsNumber(), args[1].ToDisplay(), &v)) {
      ctx.RaiseError("mod.number: no such setting");
      return ScriptValue::Num(0);
    }
    return ScriptValue::Num(v);
  });

  install("mod.text", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.text: module registry unavailable"); return ScriptValue::Str(""); }
    std::string v;
    if (!services_.modules->GetText((u64)args[0].AsNumber(), args[1].ToDisplay(), &v)) {
      ctx.RaiseError("mod.text: no such setting");
      return ScriptValue::Str("");
    }
    return ScriptValue::Str(v);
  });

  install("mod.enabled", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.enabled: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->IsEnabled((u64)args[0].AsNumber()));
  });

  install("mod.set_enabled", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.set_enabled: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->SetEnabled((u64)args[0].AsNumber(),
                                                           args[1].Truthy()));
  });

  install("mod.count", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.modules) { ctx.RaiseError("mod.count: module registry unavailable"); return ScriptValue::Num(0); }
    return ScriptValue::Num((f64)services_.modules->Count());
  });

  install("mod.get", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.get: module registry unavailable"); return ScriptValue::Nil(); }
    const u64 h = (u64)args[0].AsNumber();
    const std::string key = args[1].ToDisplay();
    ScriptModuleInfo info;
    if (!services_.modules->GetModule(h, &info)) {
      ctx.RaiseError("mod.get: no such module");
      return ScriptValue::Nil();
    }
    const SettingDef* def = nullptr;
    for (const SettingDef& d : info.settings) {
      if (d.key == key) { def = &d; break; }
    }
    if (!def) {
      ctx.RaiseError("mod.get: no such setting");
      return ScriptValue::Nil();
    }
    SettingValue v;
    if (!services_.modules->Get(h, key, &v)) {
      ctx.RaiseError("mod.get: no such setting");
      return ScriptValue::Nil();
    }
    ScriptTable t;
    t["key"] = ScriptValue::Str(def->key);
    t["label"] = ScriptValue::Str(def->label);
    t["kind"] = ScriptValue::Str(SettingKindName(def->kind));
    t["number"] = ScriptValue::Num(v.number);
    t["text"] = ScriptValue::Str(v.text);
    t["pressed"] = ScriptValue::Bool(v.pressed);
    t["min"] = ScriptValue::Num(def->min);
    t["max"] = ScriptValue::Num(def->max);
    std::string opts;
    for (size_t i = 0; i < def->options.size(); ++i) {
      if (i) opts += '\n';
      opts += def->options[i];
    }
    t["options"] = ScriptValue::Str(opts);
    return ScriptValue::Tbl(std::move(t));
  });

  install("mod.info", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.info: module registry unavailable"); return ScriptValue::Nil(); }
    ScriptModuleInfo info;
    if (!services_.modules->GetModule((u64)args[0].AsNumber(), &info)) {
      ctx.RaiseError("mod.info: no such module");
      return ScriptValue::Nil();
    }
    ScriptTable t;
    t["handle"] = ScriptValue::Num((f64)info.handle);
    t["name"] = ScriptValue::Str(info.name);
    t["description"] = ScriptValue::Str(info.description);
    t["version"] = ScriptValue::Str(info.version);
    t["author"] = ScriptValue::Str(info.author);
    t["enabled"] = ScriptValue::Bool(info.enabled);
    t["count"] = ScriptValue::Num((f64)info.settings.size());
    ScriptTable settings;
    int n = 0;
    for (const SettingDef& d : info.settings) {
      SettingValue v;
      services_.modules->Get(info.handle, d.key, &v);
      ScriptTable s;
      s["key"] = ScriptValue::Str(d.key);
      s["label"] = ScriptValue::Str(d.label);
      s["kind"] = ScriptValue::Str(SettingKindName(d.kind));
      s["number"] = ScriptValue::Num(v.number);
      s["text"] = ScriptValue::Str(v.text);
      s["pressed"] = ScriptValue::Bool(v.pressed);
      s["min"] = ScriptValue::Num(d.min);
      s["max"] = ScriptValue::Num(d.max);
      settings[std::to_string(n++)] = ScriptValue::Tbl(std::move(s));
    }
    t["settings"] = ScriptValue::Tbl(std::move(settings));
    return ScriptValue::Tbl(std::move(t));
  });

  install("mod.list", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.modules) { ctx.RaiseError("mod.list: module registry unavailable"); return ScriptValue::Str(""); }
    return ScriptValue::Str(services_.modules->List());
  });

  install("mod.settings", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.settings: module registry unavailable"); return ScriptValue::Str(""); }
    return ScriptValue::Str(services_.modules->SettingsOf((u64)args[0].AsNumber()));
  });

  install("mod.clear", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.modules) { ctx.RaiseError("mod.clear: module registry unavailable"); return ScriptValue::Nil(); }
    services_.modules->Clear();
    return ScriptValue::Nil();
  });

  install("mod.set_overlay", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    if (!services_.modules) { ctx.RaiseError("mod.set_overlay: module registry unavailable"); return ScriptValue::Nil(); }
    services_.modules->SetOverlayVisible(args[0].Truthy());
    return ScriptValue::Nil();
  });

  install("mod.overlay", [this](ScriptContext& ctx, const std::vector<ScriptValue>&) {
    if (!services_.modules) { ctx.RaiseError("mod.overlay: module registry unavailable"); return ScriptValue::Bool(false); }
    return ScriptValue::Bool(services_.modules->overlayVisible());
  });

  return installed_.size();
}

size_t ScriptApi::DrainOnRenderThread() { return queue_.Drain(); }

void ScriptApi::PublishFrame(f32 frameMs, const Vec2& screen) {
  frameMs_.store(frameMs);
  screenW_.store(screen.x);
  screenH_.store(screen.y);
  if (services_.governor) services_.governor->OnFrame(frameMs);
  if (services_.hud) services_.hud->Update(frameMs);
}

Vec2 ScriptApi::screenSize() const { return Vec2(screenW_.load(), screenH_.load()); }
f32 ScriptApi::frameTimeMs() const { return frameMs_.load(); }

bool ScriptApi::installed(const std::string& name) const {
  return std::find(installed_.begin(), installed_.end(), name) != installed_.end();
}

size_t ScriptApi::installedCount() const { return installed_.size(); }

void ScriptApi::PostRender(RenderCommandQueue::Command cmd) { queue_.Post(std::move(cmd)); }

u64 ScriptApi::AllocWidgetHandle() {
  std::lock_guard<std::mutex> lk(mutex_);
  return nextWidgetHandle_++;
}

void ScriptApi::SetWidgetId(u64 handle, WidgetId id) {
  std::lock_guard<std::mutex> lk(mutex_);
  widgetIds_[handle] = id;
}

bool ScriptApi::ResolveWidgetId(u64 handle, WidgetId* out) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = widgetIds_.find(handle);
  if (it == widgetIds_.end() || it->second == 0) return false;
  if (out) *out = it->second;
  return true;
}

//----------------------------------------------------------------------------
// Combat telemetry cache
//----------------------------------------------------------------------------

void ScriptApi::InitCombatCache() {
  combatListener_ = EventBus::Instance().On(
      "berry:combat", [this](const EventContext& e) {
        const CombatEvent* evt = static_cast<const CombatEvent*>(e.data);
        if (!evt) return EventResult::Continue;
        const f32 dmg = evt->finalDamage > 0 ? evt->finalDamage : evt->baseDamage;
        lastHitDamage_.store(dmg);
        std::lock_guard<std::mutex> lk(combatMutex_);
        observed_.push_back(ObservedHit{evt->tick, dmg});
        while (observed_.size() > kMaxObserved) observed_.pop_front();
        return EventResult::Continue;
      });
}

void ScriptApi::InitCombatStateHook() {
  // The tracker is a client-side service, not a strict module, so the bridge
  // feeds it from the same events everything else sees:
  //
  //   berry:tick    -> record the current tick's hitboxes and age predictions
  //   berry:combat  -> validate reach, confirm a predicted hit, record a swing
  //
  // Both run on the game-tick thread and read only seam data / event payloads,
  // never game memory. When the service is not wired the hook is a no-op.
  tickListener_ = EventBus::Instance().On(
      "berry:tick", [this](const EventContext& e) {
        if (!services_.combatState) return EventResult::Continue;
        const GameTick* t = static_cast<const GameTick*>(e.data);
        if (!t) return EventResult::Continue;
        const u64 tick = (u64)*t;
        // Snapshot every entity the world already knows about, so the reach
        // check has a box to measure against. This is the same cache the World
        // module refreshes; nothing new is read from the engine.
        if (services_.world) {
          auto seam = services_.world->Seam();
          if (seam) {
            for (const EntityId& id : seam->AllEntities()) {
              EntitySnapshot snap;
              if (seam->GetEntity(id, &snap)) {
                services_.combatState->ObserveEntity(snap, tick);
              }
            }
          }
        }
        // Age pending hit predictions: anything past the window becomes a miss.
        services_.combatState->Tick(tick);
        return EventResult::Continue;
      });

  combatStateListener_ = EventBus::Instance().On(
      "berry:combat", [this](const EventContext& e) {
        if (!services_.combatState) return EventResult::Continue;
        const CombatEvent* evt = static_cast<const CombatEvent*>(e.data);
        if (!evt) return EventResult::Continue;
        // Reach validation, swing timing and hit confirmation, all from the one
        // applied-hit observation. The tracker is read-only: it never changes
        // the event or the damage. `ConfirmHit` publishes the confirmed event,
        // so it runs outside `ObserveHit`'s lock.
        services_.combatState->ObserveHit(*evt, evt->tick);
        services_.combatState->ConfirmHit(*evt, evt->tick);
        services_.combatState->ObserveSwing(evt->attacker, evt->tick);
        return EventResult::Continue;
      });
}

//----------------------------------------------------------------------------
// Frame hook
//----------------------------------------------------------------------------

}  // namespace Berry
