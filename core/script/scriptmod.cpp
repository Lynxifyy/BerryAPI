#include "berry/scriptmod.h"

#include "berry/render.h"

#include <algorithm>
#include <cmath>
#include <sstream>

namespace Berry {

const char* SettingKindName(SettingKind k) {
  switch (k) {
    case SettingKind::Toggle: return "toggle";
    case SettingKind::Slider: return "slider";
    case SettingKind::Select: return "select";
    case SettingKind::Keybind: return "keybind";
    case SettingKind::Text: return "text";
    case SettingKind::Button: return "button";
  }
  return "unknown";
}

std::string ScriptModuleRegistry::ValueKey(u64 handle, const std::string& key) {
  return std::to_string(handle) + "/" + key;
}

ScriptModuleInfo* ScriptModuleRegistry::Find(u64 handle) {
  for (ScriptModuleInfo& m : modules_) {
    if (m.handle == handle) return &m;
  }
  return nullptr;
}

const ScriptModuleInfo* ScriptModuleRegistry::Find(u64 handle) const {
  for (const ScriptModuleInfo& m : modules_) {
    if (m.handle == handle) return &m;
  }
  return nullptr;
}

SettingDef* ScriptModuleRegistry::FindSetting(ScriptModuleInfo* m,
                                              const std::string& key) {
  if (!m) return nullptr;
  for (SettingDef& d : m->settings) {
    if (d.key == key) return &d;
  }
  return nullptr;
}

const SettingDef* ScriptModuleRegistry::FindSetting(const ScriptModuleInfo* m,
                                                    const std::string& key) const {
  if (!m) return nullptr;
  for (const SettingDef& d : m->settings) {
    if (d.key == key) return &d;
  }
  return nullptr;
}

u64 ScriptModuleRegistry::Create(const std::string& name,
                                 const std::string& description,
                                 const std::string& version,
                                 const std::string& author) {
  if (name.empty()) return 0;
  std::lock_guard<std::mutex> lk(mutex_);
  for (const ScriptModuleInfo& m : modules_) {
    if (m.name == name) return 0;   // names are unique
  }
  ScriptModuleInfo m;
  m.handle = nextHandle_++;
  m.name = name;
  m.description = description;
  m.version = version.empty() ? "1.0.0" : version;
  m.author = author;
  m.enabled = false;
  modules_.push_back(std::move(m));
  return modules_.back().handle;
}

bool ScriptModuleRegistry::Has(u64 handle) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return Find(handle) != nullptr;
}

size_t ScriptModuleRegistry::Count() const {
  std::lock_guard<std::mutex> lk(mutex_);
  return modules_.size();
}

void ScriptModuleRegistry::Clear() {
  std::lock_guard<std::mutex> lk(mutex_);
  modules_.clear();
  values_.clear();
  nextHandle_ = 1;
}

//--- setting declaration ----------------------------------------------------

namespace {

/// A key is usable when it is non-empty and free of the separators the
// introspection strings use, so a key can never be confused with a value.
bool KeyOk(const std::string& key) {
  if (key.empty() || key.size() > 128) return false;
  for (char c : key) {
    if (c == '\n' || c == '=' || c == '/') return false;
  }
  return true;
}

}  // namespace

bool ScriptModuleRegistry::AddToggle(u64 handle, const std::string& key,
                                     const std::string& label, bool def) {
  if (!KeyOk(key)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  if (!m || FindSetting(m, key)) return false;
  SettingDef d;
  d.key = key;
  d.label = label;
  d.kind = SettingKind::Toggle;
  d.number = def ? 1 : 0;
  m->settings.push_back(std::move(d));
  values_[ValueKey(handle, key)] = SettingValue{def ? 1.0 : 0.0, "", false};
  return true;
}

bool ScriptModuleRegistry::AddSlider(u64 handle, const std::string& key,
                                     const std::string& label, f64 min, f64 max,
                                     f64 def) {
  if (!KeyOk(key)) return false;
  if (!std::isfinite(min) || !std::isfinite(max) || !std::isfinite(def)) return false;
  if (max < min) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  if (!m || FindSetting(m, key)) return false;
  SettingDef d;
  d.key = key;
  d.label = label;
  d.kind = SettingKind::Slider;
  d.min = min;
  d.max = max;
  d.number = std::min(std::max(def, min), max);   // a default outside the range is clamped
  m->settings.push_back(std::move(d));
  values_[ValueKey(handle, key)] = SettingValue{d.number, "", false};
  return true;
}

bool ScriptModuleRegistry::AddSelect(u64 handle, const std::string& key,
                                     const std::string& label,
                                     const std::vector<std::string>& options,
                                     i64 defIndex) {
  if (!KeyOk(key) || options.empty()) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  if (!m || FindSetting(m, key)) return false;
  SettingDef d;
  d.key = key;
  d.label = label;
  d.kind = SettingKind::Select;
  d.options = options;
  if (defIndex < 0 || (size_t)defIndex >= options.size()) defIndex = 0;
  d.number = (f64)defIndex;
  m->settings.push_back(std::move(d));
  values_[ValueKey(handle, key)] = SettingValue{(f64)defIndex, "", false};
  return true;
}

bool ScriptModuleRegistry::AddKeybind(u64 handle, const std::string& key,
                                      const std::string& label,
                                      const std::string& def) {
  if (!KeyOk(key)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  if (!m || FindSetting(m, key)) return false;
  SettingDef d;
  d.key = key;
  d.label = label;
  d.kind = SettingKind::Keybind;
  d.text = def;
  m->settings.push_back(std::move(d));
  values_[ValueKey(handle, key)] = SettingValue{0.0, def, false};
  return true;
}

bool ScriptModuleRegistry::AddText(u64 handle, const std::string& key,
                                   const std::string& label,
                                   const std::string& def) {
  if (!KeyOk(key)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  if (!m || FindSetting(m, key)) return false;
  SettingDef d;
  d.key = key;
  d.label = label;
  d.kind = SettingKind::Text;
  d.text = def;
  m->settings.push_back(std::move(d));
  values_[ValueKey(handle, key)] = SettingValue{0.0, def, false};
  return true;
}

bool ScriptModuleRegistry::AddButton(u64 handle, const std::string& key,
                                     const std::string& label) {
  if (!KeyOk(key)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  if (!m || FindSetting(m, key)) return false;
  SettingDef d;
  d.key = key;
  d.label = label;
  d.kind = SettingKind::Button;
  m->settings.push_back(std::move(d));
  values_[ValueKey(handle, key)] = SettingValue{0.0, "", false};
  return true;
}

//--- value access -----------------------------------------------------------

bool ScriptModuleRegistry::SetNumber(u64 handle, const std::string& key, f64 value) {
  if (!std::isfinite(value)) return false;
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  const SettingDef* d = FindSetting(m, key);
  if (!d) return false;
  switch (d->kind) {
    case SettingKind::Toggle:
      if (value != 0.0 && value != 1.0) return false;
      break;
    case SettingKind::Slider:
      if (value < d->min || value > d->max) return false;   // fail closed, not clamp
      break;
    case SettingKind::Select:
      if (value < 0 || value >= (f64)d->options.size()) return false;
      break;
    default:
      return false;   // a keybind/text/button is not written as a number
  }
  values_[ValueKey(handle, key)].number = value;
  return true;
}

bool ScriptModuleRegistry::SetText(u64 handle, const std::string& key,
                                   const std::string& value) {
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  const SettingDef* d = FindSetting(m, key);
  if (!d) return false;
  if (d->kind != SettingKind::Keybind && d->kind != SettingKind::Text) return false;
  if (value.size() > 1024) return false;
  values_[ValueKey(handle, key)].text = value;
  return true;
}

bool ScriptModuleRegistry::Press(u64 handle, const std::string& key) {
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  const SettingDef* d = FindSetting(m, key);
  if (!d || d->kind != SettingKind::Button) return false;
  values_[ValueKey(handle, key)].pressed = true;
  return true;
}

bool ScriptModuleRegistry::GetNumber(u64 handle, const std::string& key,
                                     f64* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  const ScriptModuleInfo* m = Find(handle);
  const SettingDef* d = FindSetting(m, key);
  if (!d) return false;
  if (d->kind == SettingKind::Keybind || d->kind == SettingKind::Text) return false;
  auto it = values_.find(ValueKey(handle, key));
  if (it == values_.end()) return false;
  if (out) *out = it->second.number;
  return true;
}

bool ScriptModuleRegistry::GetText(u64 handle, const std::string& key,
                                   std::string* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  const ScriptModuleInfo* m = Find(handle);
  const SettingDef* d = FindSetting(m, key);
  if (!d) return false;
  if (d->kind != SettingKind::Keybind && d->kind != SettingKind::Text) return false;
  auto it = values_.find(ValueKey(handle, key));
  if (it == values_.end()) return false;
  if (out) *out = it->second.text;
  return true;
}

bool ScriptModuleRegistry::Get(u64 handle, const std::string& key,
                               SettingValue* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  const ScriptModuleInfo* m = Find(handle);
  const SettingDef* d = FindSetting(m, key);
  if (!d) return false;
  auto it = values_.find(ValueKey(handle, key));
  if (it == values_.end()) return false;
  if (out) {
    *out = it->second;
    // A Button press is a one-shot edge: reading it consumes it, so a caller
    // that polls cannot see the same press twice.
    if (d->kind == SettingKind::Button) {
      const_cast<SettingValue&>(it->second).pressed = false;
    }
  }
  return true;
}

//--- enable/disable ---------------------------------------------------------

bool ScriptModuleRegistry::Enable(u64 handle) { return SetEnabled(handle, true); }
bool ScriptModuleRegistry::Disable(u64 handle) { return SetEnabled(handle, false); }

bool ScriptModuleRegistry::SetEnabled(u64 handle, bool on) {
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  if (!m) return false;
  m->enabled = on;
  return true;
}

bool ScriptModuleRegistry::IsEnabled(u64 handle) const {
  std::lock_guard<std::mutex> lk(mutex_);
  const ScriptModuleInfo* m = Find(handle);
  return m && m->enabled;
}

//--- introspection ----------------------------------------------------------

bool ScriptModuleRegistry::SetMeta(u64 handle, const std::string& version,
                                   const std::string& author) {
  std::lock_guard<std::mutex> lk(mutex_);
  ScriptModuleInfo* m = Find(handle);
  if (!m) return false;
  if (!version.empty()) m->version = version;
  m->author = author;
  return true;
}

bool ScriptModuleRegistry::GetModule(u64 handle, ScriptModuleInfo* out) const {
  std::lock_guard<std::mutex> lk(mutex_);
  const ScriptModuleInfo* m = Find(handle);
  if (!m) return false;
  if (out) *out = *m;
  return true;
}

std::string ScriptModuleRegistry::List() const {
  std::lock_guard<std::mutex> lk(mutex_);
  std::string out;
  for (const ScriptModuleInfo& m : modules_) {
    if (!out.empty()) out += '\n';
    out += std::to_string(m.handle) + ":" + m.name + ":" +
           (m.enabled ? "on" : "off");
  }
  return out;
}

std::string ScriptModuleRegistry::SettingsOf(u64 handle) const {
  std::lock_guard<std::mutex> lk(mutex_);
  const ScriptModuleInfo* m = Find(handle);
  if (!m) return std::string();
  std::string out;
  for (const SettingDef& d : m->settings) {
    auto it = values_.find(ValueKey(handle, d.key));
    if (it == values_.end()) continue;
    if (!out.empty()) out += '\n';
    out += d.key + "=";
    if (d.kind == SettingKind::Keybind || d.kind == SettingKind::Text) {
      out += it->second.text;
    } else if (d.kind == SettingKind::Select) {
      const i64 idx = (i64)it->second.number;
      out += (idx >= 0 && (size_t)idx < d.options.size()) ? d.options[(size_t)idx]
                                                          : std::string();
    } else if (d.kind == SettingKind::Button) {
      out += it->second.pressed ? "pressed" : "idle";
    } else {
      std::ostringstream ss;
      ss << it->second.number;
      out += ss.str();
    }
  }
  return out;
}

void ScriptModuleRegistry::DrawOverlay(Renderer& renderer, const Vec2& screen) const {
  if (!overlayVisible()) return;

  // Snapshot under the lock, draw without it: the render thread must not hold
  // the registry mutex across a draw call (a script on the worker could block
  // behind it), and a module declared mid-draw is simply next frame's line.
  std::vector<std::pair<std::string, bool>> rows;
  {
    std::lock_guard<std::mutex> lk(mutex_);
    rows.reserve(modules_.size());
    for (const ScriptModuleInfo& m : modules_) {
      rows.emplace_back(m.name, m.enabled);
    }
  }
  if (rows.empty()) return;

  const f32 scale = 1.0f;
  const f32 lineH = 12.0f;
  const f32 x = 8.0f;
  f32 y = std::max(8.0f, screen.y - lineH * (f32)(rows.size() + 1) - 8.0f);
  const Rgba bg = Rgba(20, 20, 20, 180);
  const Rgba on = Rgba(60, 255, 120);
  const Rgba off = Rgba(160, 160, 160);

  Rect2D panel;
  panel.min = Vec2(x - 4.0f, y - 4.0f);
  panel.max = Vec2(x + 180.0f, y + lineH * (f32)(rows.size() + 1));
  panel.color = bg;
  panel.fill = bg;
  panel.filled = true;
  renderer.DrawRect(panel);
  renderer.DrawText(Vec2(x, y), "modules", Rgba::White(), scale);
  y += lineH;
  for (const auto& [name, enabled] : rows) {
    renderer.DrawText(Vec2(x, y), name.c_str(), enabled ? on : off, scale);
    y += lineH;
  }
}

}  // namespace Berry
