#pragma once
// BerryAPI PvP HUD modules.
//
// Client-side, local-only overlays built on the Hud/Input/Combat services:
// CPS + keystrokes, a combo counter, armor/durability, effects, ping/TPS/FPS,
// a target panel, session stats, hit colour and low-health vignette.
//
// Everything here shows the local player their OWN state. Nothing reveals
// information the client was not already sent by the server (no ESP, no reach,
// no aim assist), which is the line that keeps users unbanned.

#include "berry/export.h"
#include "berry/types.h"
#include "berry/combat.h"
#include "berry/hud.h"
#include "berry/input.h"

#include <deque>
#include <string>
#include <vector>

namespace Berry {

/// One row of the keystroke display.
struct BERRY_API KeyStroke {
  std::string key;     // "W", "A", "S", "D", "LMB", "RMB", "SPC"
  bool down = false;
  f32 fade = 1.0f;     // 1 while held, decays after release
};

/// Armor piece with durability, as shown in the armor HUD.
struct BERRY_API ArmorPiece {
  std::string name;    // "helmet", "chestplate", ...
  i32 durability = 0;
  i32 maxDurability = 0;
  /// 0..1 remaining fraction; 1 when there is no durability bar.
  f32 Fraction() const {
    if (maxDurability <= 0) return 1.0f;
    f32 f = (f32)durability / (f32)maxDurability;
    return f < 0 ? 0.0f : (f > 1 ? 1.0f : f);
  }
};

/// An active status effect with its remaining duration.
struct BERRY_API EffectState {
  std::string name;     // "speed", "strength", ...
  i32 amplifier = 0;
  f32 secondsLeft = 0;
  bool ambient = false;
};

/// Network/frame telemetry shown in the info line.
struct BERRY_API NetStats {
  i32 pingMs = 0;
  f32 tps = 20.0f;
  f32 fps = 0.0f;
};

/// Live target readout for the current fight.
struct BERRY_API TargetInfo {
  std::string name;
  f32 health = 0;
  f32 maxHealth = 0;
  f32 distance = 0;
  i32 armorPoints = 0;
  bool valid = false;
};

/// A floating damage number.
struct BERRY_API DamagePopup {
  f32 amount = 0;
  Vec2 screenPos;
  f32 ageMs = 0;
  f32 lifetimeMs = 900.0f;
  bool critical = false;
  f32 Alpha() const {
    if (lifetimeMs <= 0) return 0;
    f32 t = ageMs / lifetimeMs;
    return t >= 1 ? 0 : (1 - t);
  }
};

/// Rolling session statistics.
struct BERRY_API SessionStats {
  f64 damageDealt = 0;
  f64 damageTaken = 0;
  u64 kills = 0;
  u64 deaths = 0;
  u64 hits = 0;
  f32 peakDps = 0;

  f64 Kd() const { return deaths ? (f64)kills / (f64)deaths : (f64)kills; }
};

/// Configuration for the PvP overlay set.
struct BERRY_API PvpConfig {
  bool showCps = true;
  bool showKeystrokes = true;
  bool showCombo = true;
  bool showArmor = true;
  bool showEffects = true;
  bool showNetStats = true;
  bool showTarget = true;
  bool showSession = true;
  bool showDamagePopups = true;
  bool hitFlash = true;
  bool lowHealthVignette = true;
  f32 lowHealthThreshold = 0.35f;   // fraction of max health
};

/// The PvP overlay: owns the widgets and turns game/combat/input data into
/// draw calls. Local-only; it never writes game state.
class BERRY_API PvpOverlay {
 public:
  PvpOverlay() = default;

  /// Builds the widget tree once. Idempotent.
  void Attach(Hud& hud);
  void Detach();
  bool attached() const { return attached_; }

  void SetConfig(const PvpConfig& c) { config_ = c; }
  const PvpConfig& config() const { return config_; }

  //--- data feeds (all local, all read-only) --------------------------------
  /// Records a left/right click for the CPS counter.
  void OnClick(MouseButton button, u64 nowMs);
  /// Records a damage event the player dealt or took.
  void OnDamage(f32 amount, bool dealt, bool critical, u64 nowMs);
  /// Records a kill (combo increments) or a death (combo resets).
  void OnKill(u64 nowMs);
  void OnDeath(u64 nowMs);

  void SetArmor(const std::vector<ArmorPiece>& pieces);
  void SetEffects(const std::vector<EffectState>& effects);
  void SetNetStats(const NetStats& stats);
  void SetTarget(const TargetInfo& target);
  void SetHealth(f32 health, f32 maxHealth);
  void SetKeystroke(const std::string& key, bool down);
  /// Adds a floating damage number at a screen position.
  void AddDamagePopup(f32 amount, const Vec2& at, bool critical);

  /// Advances animation: combo timeout, popups, keystroke fade, hit flash.
  void Update(f32 dtMs, u64 nowMs);

  /// Draws the overlay into `r`. Call between BeginFrame/EndFrame.
  void Present(Hud& hud, Renderer& r, const Vec2& screen);

  //--- introspection (tests + other modules) --------------------------------
  const SessionStats& session() const { return session_; }
  u64 combo() const { return combo_; }
  bool comboActive() const { return comboActive_; }
  size_t popupCount() const { return popups_.size(); }
  f32 hitFlash() const { return hitFlash_; }
  const std::vector<KeyStroke>& keystrokes() const { return keystrokes_; }

  /// Seconds a combo stays alive after the last kill.
  static constexpr f32 kComboWindowSec = 4.0f;

 private:
  void EnsureWidgets(Hud& hud);
  void RefreshKeystrokeFade(f32 dtMs);

  PvpConfig config_;
  bool attached_ = false;
  bool widgetsBuilt_ = false;

  // Widget handles.
  WidgetId wRoot_ = 0;
  WidgetId wCps_ = 0;
  WidgetId wCpsGraph_ = 0;
  WidgetId wKeys_ = 0;
  WidgetId wCombo_ = 0;
  WidgetId wArmor_ = 0;
  WidgetId wEffects_ = 0;
  WidgetId wNet_ = 0;
  WidgetId wTarget_ = 0;
  WidgetId wSession_ = 0;

  InputObserver input_;
  SessionStats session_;
  NetStats net_;
  TargetInfo target_;
  std::vector<ArmorPiece> armor_;
  std::vector<EffectState> effects_;
  std::vector<KeyStroke> keystrokes_;
  std::vector<DamagePopup> popups_;

  u64 combo_ = 0;
  bool comboActive_ = false;
  f32 comboTimer_ = 0;
  f32 hitFlash_ = 0;
  f32 health_ = 1.0f;
  f32 maxHealth_ = 1.0f;
  u64 lastClickMs_ = 0;
};

}  // namespace Berry
