#pragma once
// Script module + settings system.
//
// A script ships a MODULE: a named unit with metadata (version, author), a set
// of typed SETTINGS, and an enable/disable switch. The settings system is what
// turns "a pile of globals" into something a user can configure: a toggle is a
// boolean with a label, a slider carries a range, a select carries its options,
// a keybind carries a key name. A setting's type is fixed when it is declared,
// so a later write is validated against it rather than trusted.
//
// Two deliberate choices:
//
//   * The registry is pure data. It never reads game memory and never draws;
//     it is the model the UI (and the frame hook's overlay) renders. That keeps
//     it unit-testable with no game running.
//   * Every mutation is validated and fails closed. Declaring two settings with
//     the same key, a slider whose max is below its min, a select with no
//     options, or writing a non-finite number all fail rather than storing
//     something the UI cannot represent.
//
// This is a client-side service, deliberately not a new strict Berry:: module:
// it draws (when its overlay is on) through Berry::Render and holds no engine
// pointers of its own.

#include "berry/export.h"
#include "berry/types.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace Berry {

class Renderer;
struct Vec2;

/// The kind of a setting. The kind fixes what a value means and which writes
/// are valid, so a toggle can never be given an out-of-range slider value.
enum class SettingKind : u32 {
  Toggle = 0,   // boolean; value is 0 or 1
  Slider,       // number in [min, max]
  Select,       // index into `options`
  Keybind,      // a key name (text)
  Text,         // free text
  Button,       // an action; has no stored value, only a press
};

const char* SettingKindName(SettingKind k);

/// A setting's declaration. Fixed at Add* time.
struct BERRY_API SettingDef {
  std::string key;              // stable id a script reads/writes
  std::string label;            // display name
  SettingKind kind = SettingKind::Toggle;
  f64 number = 0;               // toggle default, slider default, select index
  f64 min = 0;                  // slider
  f64 max = 0;                  // slider
  std::string text;             // keybind/text default
  std::vector<std::string> options;   // select
};

/// A setting's current value.
struct BERRY_API SettingValue {
  f64 number = 0;
  std::string text;
  bool pressed = false;         // Button: set for one read after a press
};

/// One script module: metadata, an enabled switch and its settings.
struct BERRY_API ScriptModuleInfo {
  u64 handle = 0;
  std::string name;
  std::string description;
  std::string version = "1.0.0";
  std::string author;
  bool enabled = false;
  std::vector<SettingDef> settings;
};

/// Owns the modules a script registered. Thread-safe: the script worker
/// declares and reads, the render thread reads for the overlay.
class BERRY_API ScriptModuleRegistry {
 public:
  /// Declares a module. Returns its handle (0 on a rejected name: empty, or one
  /// already taken by another module).
  u64 Create(const std::string& name, const std::string& description,
             const std::string& version, const std::string& author);

  bool Has(u64 handle) const;
  size_t Count() const;
  void Clear();

  //--- setting declaration --------------------------------------------------
  // Each returns false when the module is unknown, the key is empty or already
  // declared, or the declaration is not representable (a slider with max < min,
  // a select with no options, a non-finite bound).
  bool AddToggle(u64 handle, const std::string& key, const std::string& label, bool def);
  bool AddSlider(u64 handle, const std::string& key, const std::string& label,
                 f64 min, f64 max, f64 def);
  bool AddSelect(u64 handle, const std::string& key, const std::string& label,
                 const std::vector<std::string>& options, i64 defIndex);
  bool AddKeybind(u64 handle, const std::string& key, const std::string& label,
                  const std::string& def);
  bool AddText(u64 handle, const std::string& key, const std::string& label,
               const std::string& def);
  bool AddButton(u64 handle, const std::string& key, const std::string& label);

  //--- value access ---------------------------------------------------------
  // A write is validated against the setting's declared kind and range. A
  // Button press is recorded through Press().
  bool SetNumber(u64 handle, const std::string& key, f64 value);
  bool SetText(u64 handle, const std::string& key, const std::string& value);
  bool Press(u64 handle, const std::string& key);

  bool GetNumber(u64 handle, const std::string& key, f64* out) const;
  bool GetText(u64 handle, const std::string& key, std::string* out) const;
  /// Reads a setting's current value by key, or false when unknown.
  bool Get(u64 handle, const std::string& key, SettingValue* out) const;

  //--- enable/disable -------------------------------------------------------
  bool Enable(u64 handle);
  bool Disable(u64 handle);
  bool SetEnabled(u64 handle, bool on);
  bool IsEnabled(u64 handle) const;

  /// Sets a module's version and author after creation (the IDL's `mod.set_meta`).
  /// Fails closed on an unknown handle.
  bool SetMeta(u64 handle, const std::string& version, const std::string& author);

  //--- introspection --------------------------------------------------------
  bool GetModule(u64 handle, ScriptModuleInfo* out) const;
  std::string List() const;                    // "handle:name:enabled\n…"
  std::string SettingsOf(u64 handle) const;    // "key=value\n…"

  /// The overlay: when on, the frame hook draws the module list + settings.
  void SetOverlayVisible(bool on) { overlayVisible_.store(on); }
  bool overlayVisible() const { return overlayVisible_.load(); }

  /// Draws the module list (name + on/off) through the Renderer when the
  /// overlay is on. Called on the render thread from the frame hook; a no-op
  /// when the overlay is off or there is nothing to show. Reads a snapshot of
  /// the registry, so a script declaring a module mid-frame cannot tear it.
  void DrawOverlay(Renderer& renderer, const Vec2& screen) const;

 private:
  mutable std::mutex mutex_;
  std::vector<ScriptModuleInfo> modules_;
  // Values live beside the defs, keyed by (handle, key), so a setting's type
  // and its value cannot drift apart.
  std::unordered_map<std::string, SettingValue> values_;
  u64 nextHandle_ = 1;
  std::atomic<bool> overlayVisible_{false};

  static std::string ValueKey(u64 handle, const std::string& key);
  ScriptModuleInfo* Find(u64 handle);
  const ScriptModuleInfo* Find(u64 handle) const;
  SettingDef* FindSetting(ScriptModuleInfo* m, const std::string& key);
  const SettingDef* FindSetting(const ScriptModuleInfo* m, const std::string& key) const;
};

}  // namespace Berry
