// Bindings from the declared IDL surface to the C++ services. See
// include/berry/api_bindings.h for the threading contract.
#include "berry/api_bindings.h"

#include "berry/async.h"
#include "berry/engine.h"
#include "berry/event.h"
#include "berry/generated_bindings.h"
#include "berry/logger.h"

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

}  // namespace

//----------------------------------------------------------------------------
// ScriptApi
//----------------------------------------------------------------------------

ScriptApi::ScriptApi() { InitHooks(); }
ScriptApi::ScriptApi(const Services& services) : services_(services) { InitHooks(); }
ScriptApi::~ScriptApi() { RemoveHooks(); }

void ScriptApi::InitHooks() {
  InitCombatCache();
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
    if (!services_.world) { ctx.RaiseError("world.block_at: world unavailable"); return ScriptValue::Str(""); }
    const Vec3 p((f32)args[0].AsNumber(), (f32)args[1].AsNumber(), (f32)args[2].AsNumber());
    const u32 block = services_.world->GetBlock(p);
    return ScriptValue::Str(std::to_string(block));
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

  //--- Network (granted, host-scoped) --------------------------------------
  // Shares the async layer's egress path, so the grant is checked here and on
  // every redirect hop. Returns the body, exactly as `await Http.get` does.
  install("network.get", [this](ScriptContext& ctx, const std::vector<ScriptValue>& args) {
    const std::string url = args[0].ToDisplay();
    const f64 timeout = args[1].AsNumber();
    return HttpGetBlocking(ctx, url, (long)timeout, services_.networkPolicy);
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

//----------------------------------------------------------------------------
// Frame hook
//----------------------------------------------------------------------------

}  // namespace Berry
