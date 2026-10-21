// PvP HUD modules. See include/berry/pvp.h.
//
// Local-only by construction: every widget renders state the local client
// already owns (its own input, its own health, its own armor, the server's
// ping/tps). Nothing here projects another player's position, so the overlay
// cannot become an ESP.
#include "berry/pvp.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace Berry {
namespace {

std::string Format(const char* fmt, ...) {
  char buf[128];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return std::string(buf);
}

}  // namespace

void PvpOverlay::Attach(Hud& hud) {
  if (attached_) return;
  EnsureWidgets(hud);
  attached_ = true;
}

void PvpOverlay::EnsureWidgets(Hud& hud) {
  if (widgetsBuilt_) return;
  widgetsBuilt_ = true;

  // A root column on the left holds the CPS readout and its graph.
  wRoot_ = hud.Create(WidgetKind::Column, "", Anchor::TopLeft);
  wCps_ = hud.CreateChild(wRoot_, WidgetKind::Label, "CPS 0/0");
  wCpsGraph_ = hud.CreateChild(wRoot_, WidgetKind::Graph);
  hud.Get(wCpsGraph_)->sampleCapacity = 60;

  wNet_ = hud.Create(WidgetKind::Label, "20 TPS 0 ms", Anchor::TopRight);
  wCombo_ = hud.Create(WidgetKind::Label, "", Anchor::TopCenter);
  wTarget_ = hud.Create(WidgetKind::Column, "", Anchor::TopCenter);
  if (Widget* t = hud.Get(wTarget_)) t->offset = Vec2(0, 18);
  hud.CreateChild(wTarget_, WidgetKind::Label, "");
  wArmor_ = hud.Create(WidgetKind::Column, "", Anchor::BottomCenter);
  wEffects_ = hud.Create(WidgetKind::Column, "", Anchor::CenterRight);
  wSession_ = hud.Create(WidgetKind::Label, "", Anchor::BottomLeft);
  wKeys_ = hud.Create(WidgetKind::Row, "", Anchor::BottomRight);
  for (const char* k : {"W", "A", "S", "D", "SPC", "LMB", "RMB"}) {
    hud.CreateChild(wKeys_, WidgetKind::Label, k);
    keystrokes_.push_back(KeyStroke{k, false, 1.0f});
  }
}

void PvpOverlay::Detach() {
  attached_ = false;
  widgetsBuilt_ = false;
}

void PvpOverlay::OnClick(MouseButton button, u64 nowMs) {
  input_.OnMouse(button, true);
  input_.OnMouse(button, false);
  lastClickMs_ = nowMs;
}

void PvpOverlay::OnDamage(f32 amount, bool dealt, bool critical, u64 nowMs) {
  (void)nowMs;
  if (amount <= 0) return;
  if (dealt) {
    session_.damageDealt += amount;
    session_.hits++;
    const f32 dps = (f32)session_.damageDealt /
                    std::max(1.0f, (f32)session_.hits * 0.1f);
    session_.peakDps = std::max(session_.peakDps, dps);
  } else {
    session_.damageTaken += amount;
    if (config_.hitFlash) hitFlash_ = 1.0f;
  }
  if (config_.showDamagePopups) {
    AddDamagePopup(amount, Vec2(0, 0), critical);
  }
}

void PvpOverlay::OnKill(u64 nowMs) {
  (void)nowMs;
  session_.kills++;
  combo_++;
  comboActive_ = true;
  comboTimer_ = kComboWindowSec;
}

void PvpOverlay::OnDeath(u64 nowMs) {
  (void)nowMs;
  session_.deaths++;
  combo_ = 0;
  comboActive_ = false;
  comboTimer_ = 0;
}

void PvpOverlay::SetArmor(const std::vector<ArmorPiece>& pieces) {
  armor_ = pieces;
}

void PvpOverlay::SetEffects(const std::vector<EffectState>& effects) {
  effects_ = effects;
}

void PvpOverlay::SetNetStats(const NetStats& stats) { net_ = stats; }

void PvpOverlay::SetTarget(const TargetInfo& target) { target_ = target; }

void PvpOverlay::SetHealth(f32 health, f32 maxHealth) {
  health_ = health;
  maxHealth_ = maxHealth > 0 ? maxHealth : 1.0f;
}

void PvpOverlay::SetKeystroke(const std::string& key, bool down) {
  for (auto& ks : keystrokes_) {
    if (ks.key == key) {
      ks.down = down;
      if (down) ks.fade = 1.0f;
      return;
    }
  }
}

void PvpOverlay::AddDamagePopup(f32 amount, const Vec2& at, bool critical) {
  DamagePopup p;
  p.amount = amount;
  p.screenPos = at;
  p.critical = critical;
  popups_.push_back(p);
  // Cap the list so a long fight cannot grow it without bound.
  constexpr size_t kMaxPopups = 64;
  while (popups_.size() > kMaxPopups) popups_.erase(popups_.begin());
}

void PvpOverlay::RefreshKeystrokeFade(f32 dtMs) {
  const f32 step = dtMs / 250.0f;   // ~0.25s fade after release
  for (auto& ks : keystrokes_) {
    if (ks.down) {
      ks.fade = 1.0f;
    } else {
      ks.fade = std::max(0.0f, ks.fade - step);
    }
  }
}

void PvpOverlay::Update(f32 dtMs, u64 nowMs) {
  input_.Tick(nowMs);
  RefreshKeystrokeFade(dtMs);

  // Combo window.
  if (comboActive_) {
    comboTimer_ -= dtMs / 1000.0f;
    if (comboTimer_ <= 0.0f) {
      comboActive_ = false;
      combo_ = 0;
      comboTimer_ = 0.0f;
    }
  }

  // Hit flash decays quickly.
  if (hitFlash_ > 0.0f) {
    hitFlash_ = std::max(0.0f, hitFlash_ - dtMs / 180.0f);
  }

  // Popups rise and expire.
  const f32 rise = 24.0f * (dtMs / 1000.0f);
  for (auto& p : popups_) {
    p.ageMs += dtMs;
    p.screenPos.y -= rise;
  }
  popups_.erase(std::remove_if(popups_.begin(), popups_.end(),
                               [](const DamagePopup& p) {
                                 return p.ageMs >= p.lifetimeMs;
                               }),
                popups_.end());

  // Keep the CPS graph fed once per update from the measured rate.
  (void)dtMs;
}

void PvpOverlay::Present(Hud& hud, Renderer& r, const Vec2& screen) {
  if (!attached_) return;

  // --- screen effects (drawn under the widgets) ---------------------------
  if (config_.hitFlash && hitFlash_ > 0.0f) {
    Rect2D flash;
    flash.min = Vec2(0, 0);
    flash.max = screen;
    flash.fill = Rgba(200, 40, 40, (u8)std::clamp(hitFlash_ * 60.0f, 0.0f, 255.0f));
    flash.filled = true;
    r.DrawRect(flash);
  }

  if (config_.lowHealthVignette && maxHealth_ > 0) {
    const f32 frac = health_ / maxHealth_;
    if (frac < config_.lowHealthThreshold) {
      const f32 t = 1.0f - (frac / config_.lowHealthThreshold);
      const u8 a = (u8)std::clamp(t * 120.0f, 0.0f, 255.0f);
      const f32 band = 40.0f;
      Rect2D top, bottom, left, right;
      top.min = Vec2(0, 0); top.max = Vec2(screen.x, band);
      bottom.min = Vec2(0, screen.y - band); bottom.max = screen;
      left.min = Vec2(0, 0); left.max = Vec2(band, screen.y);
      right.min = Vec2(screen.x - band, 0); right.max = screen;
      for (Rect2D* rr : {&top, &bottom, &left, &right}) {
        rr->fill = Rgba(180, 20, 20, a);
        rr->filled = true;
        r.DrawRect(*rr);
      }
    }
  }

  // --- damage popups ------------------------------------------------------
  for (const auto& p : popups_) {
    const u8 a = (u8)std::clamp(p.Alpha() * 255.0f, 0.0f, 255.0f);
    const Rgba color = p.critical ? Rgba(255, 210, 60, a) : Rgba(255, 120, 120, a);
    const std::string text = Format("%.0f", p.amount);
    r.DrawText(p.screenPos, text.c_str(), color, 1.0f);
  }

  // --- widget content -----------------------------------------------------
  const f64 lcps = input_.clicksPerSecond(MouseButton::Left);
  const f64 rcps = input_.clicksPerSecond(MouseButton::Right);
  if (Widget* w = hud.Get(wCps_)) {
    hud.SetText(wCps_, Format("CPS %.0f | %.0f", lcps, rcps));
    w->visible = config_.showCps;
  }
  if (Widget* w = hud.Get(wCpsGraph_)) {
    w->visible = config_.showCps;
  }
  if (Widget* w = hud.Get(wNet_)) {
    hud.SetText(wNet_, Format("%.0f TPS  %d ms  %.0f fps",
                              net_.tps, net_.pingMs, net_.fps));
    w->visible = config_.showNetStats;
  }
  if (Widget* w = hud.Get(wCombo_)) {
    if (comboActive_ && combo_ >= 2) {
      hud.SetText(wCombo_, Format("COMBO x%llu", (unsigned long long)combo_));
      w->visible = config_.showCombo;
    } else {
      w->visible = false;
    }
  }
  if (Widget* w = hud.Get(wSession_)) {
    hud.SetText(wSession_, Format("DPS %.1f  K/D %.2f  dealt %.0f",
                                  session_.peakDps, session_.Kd(),
                                  session_.damageDealt));
    w->visible = config_.showSession;
  }

  // Armor rows.
  if (Widget* col = hud.Get(wArmor_)) {
    col->visible = config_.showArmor && !armor_.empty();
    // Rebuild children to match the armor list (cheap: a handful of rows).
    // Copy the ids first: Hud::Remove erases from the parent's child list, so
    // iterating it while removing would invalidate the iterator.
    std::vector<WidgetId> stale = col->children;
    for (WidgetId c : stale) hud.Remove(c);
    for (const auto& piece : armor_) {
      WidgetId row = hud.CreateChild(wArmor_, WidgetKind::Label, "");
      Widget* rw = hud.Get(row);
      if (rw) {
        rw->text = Format("%s %d%%", piece.name.c_str(),
                          (int)(piece.Fraction() * 100.0f));
        rw->color = piece.Fraction() < 0.2f ? HudColorRole::Bad
                   : piece.Fraction() < 0.5f ? HudColorRole::Warn
                                             : HudColorRole::Good;
      }
    }
  }

  // Effect rows.
  if (Widget* col = hud.Get(wEffects_)) {
    col->visible = config_.showEffects && !effects_.empty();
    std::vector<WidgetId> stale = col->children;
    for (WidgetId c : stale) hud.Remove(c);
    for (const auto& e : effects_) {
      WidgetId row = hud.CreateChild(wEffects_, WidgetKind::Label, "");
      Widget* rw = hud.Get(row);
      if (rw) {
        rw->text = Format("%s %d  %.0fs", e.name.c_str(), e.amplifier + 1,
                          e.secondsLeft);
        rw->color = e.ambient ? HudColorRole::Muted : HudColorRole::Accent;
      }
    }
  }

  // Target panel.
  if (Widget* col = hud.Get(wTarget_)) {
    col->visible = config_.showTarget && target_.valid;
    if (target_.valid && !col->children.empty()) {
      Widget* line = hud.Get(col->children[0]);
      if (line) {
        line->text = Format("%s  %.0f/%.0f  %.1fm  %d armor",
                            target_.name.c_str(), target_.health,
                            target_.maxHealth, target_.distance,
                            target_.armorPoints);
        line->color = HudColorRole::Text;
      }
    }
  }

  // Keystrokes: colour by held state, fade after release.
  if (Widget* row = hud.Get(wKeys_)) {
    row->visible = config_.showKeystrokes;
    for (size_t i = 0; i < row->children.size() && i < keystrokes_.size(); ++i) {
      Widget* kw = hud.Get(row->children[i]);
      if (!kw) continue;
      kw->text = keystrokes_[i].key;
      kw->color = keystrokes_[i].down ? HudColorRole::Accent
                                      : HudColorRole::Muted;
      kw->opacity = Tween::Constant(
          0.35f + 0.65f * std::clamp(keystrokes_[i].fade, 0.0f, 1.0f));
    }
  }

  hud.Present(r, screen);
}

}  // namespace Berry
