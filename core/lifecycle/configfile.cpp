// Validated config file. See include/berry/configfile.h.
#include "berry/configfile.h"

#include "../script/json_min.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace Berry {

namespace {

using detail::json::Value;

/// True when every character is a hex digit and the length is `want`.
bool IsHex(const std::string& s, size_t want) {
  if (s.size() != want) return false;
  for (char c : s) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
          (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return true;
}

bool GetBool(const Value& obj, const char* key, bool* out, std::string* err) {
  const Value* v = obj.Get(key);
  if (!v) return true;   // absent: keep the default
  if (!v->IsBool()) {
    if (err) *err = std::string("config key '") + key + "' must be a boolean";
    return false;
  }
  *out = v->boolean;
  return true;
}

bool GetString(const Value& obj, const char* key, std::string* out,
               std::string* err) {
  const Value* v = obj.Get(key);
  if (!v) return true;
  if (!v->IsString()) {
    if (err) *err = std::string("config key '") + key + "' must be a string";
    return false;
  }
  *out = v->str;
  return true;
}

bool GetPort(const Value& obj, const char* key, unsigned* out, std::string* err) {
  const Value* v = obj.Get(key);
  if (!v) return true;
  if (!v->IsNumber() || v->number < 0 || v->number > 65535 ||
      v->number != (double)(unsigned)v->number) {
    if (err) *err = std::string("config key '") + key +
                    "' must be an integer port in [0, 65535]";
    return false;
  }
  *out = (unsigned)v->number;
  return true;
}

}  // namespace

bool ConfigFile::Parse(const std::string& text, std::string* error) {
  Value root;
  std::string perr;
  if (!detail::json::Parse(text, &root, &perr) || !root.IsObject()) {
    if (error) *error = "config is not a JSON object" +
                        (perr.empty() ? std::string() : ": " + perr);
    return false;
  }

  // An unknown key is refused: a typo must not silently keep a default.
  static const char* kKnown[] = {
      "log_level", "capture_log", "auto_attach_modules", "enable_render",
      "enable_client", "dev_mode", "safe_mode", "script_directory",
      "script_anchor_key", "script_data_directory", "enable_cosmetic_sync",
      "enable_voice", "lan_iface", "voice_group", "voice_port",
      "voice_relay_host", "voice_relay_port", "voice_room",
      "voice_room_secret"};
  for (const auto& kv : root.object) {
    bool known = false;
    for (const char* k : kKnown) {
      if (kv.first == k) { known = true; break; }
    }
    if (!known) {
      if (error) *error = "config has unknown key '" + kv.first + "'";
      return false;
    }
  }

  if (!GetString(root, "log_level", &logLevel, error)) return false;
  if (!logLevel.empty()) {
    if (logLevel != "trace" && logLevel != "debug" && logLevel != "info" &&
        logLevel != "warn" && logLevel != "error") {
      if (error) *error = "config log_level must be trace|debug|info|warn|error";
      return false;
    }
  }
  if (!GetBool(root, "capture_log", &captureLog, error)) return false;
  if (!GetBool(root, "auto_attach_modules", &autoAttachModules, error)) return false;
  if (!GetBool(root, "enable_render", &enableRender, error)) return false;
  if (!GetBool(root, "enable_client", &enableClient, error)) return false;
  if (!GetBool(root, "dev_mode", &devMode, error)) return false;
  if (!GetBool(root, "safe_mode", &safeMode, error)) return false;
  if (!GetBool(root, "enable_cosmetic_sync", &enableCosmeticSync, error)) return false;
  if (!GetBool(root, "enable_voice", &enableVoice, error)) return false;
  if (!GetString(root, "script_directory", &scriptDirectory, error)) return false;
  if (!GetString(root, "script_anchor_key", &scriptAnchorKey, error)) return false;
  if (!scriptAnchorKey.empty() && !IsHex(scriptAnchorKey, 64)) {
    if (error) *error = "config script_anchor_key must be 64 hex characters";
    return false;
  }
  if (!GetString(root, "script_data_directory", &scriptDataDirectory, error)) return false;
  if (!GetString(root, "lan_iface", &lanIface, error)) return false;
  if (!GetString(root, "voice_group", &voiceGroup, error)) return false;
  if (!GetPort(root, "voice_port", &voicePort, error)) return false;
  if (!GetString(root, "voice_relay_host", &voiceRelayHost, error)) return false;
  if (!GetPort(root, "voice_relay_port", &voiceRelayPort, error)) return false;
  if (!GetString(root, "voice_room", &voiceRoom, error)) return false;
  if (!GetString(root, "voice_room_secret", &voiceRoomSecret, error)) return false;
  present = true;
  if (error) error->clear();
  return true;
}

bool ConfigFile::Load(const std::string& path, std::string* error) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    present = false;   // a config file is optional
    return true;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  return Parse(ss.str(), error);
}

namespace {

/// A tiny env lookup that returns false when unset.
bool Env(const char* name, std::string* out) {
  const char* v = getenv(name);
  if (!v) return false;
  *out = v;
  return true;
}

bool ParseBoolEnv(const std::string& v, bool* out) {
  if (v == "1" || v == "true" || v == "on" || v == "yes") { *out = true; return true; }
  if (v == "0" || v == "false" || v == "off" || v == "no") { *out = false; return true; }
  return false;
}

}  // namespace

int ConfigFile::ApplyEnvOverrides() {
  int applied = 0;
  std::string v;
  bool b = false;
  if (Env("BERRY_LOG_LEVEL", &v)) { logLevel = v; ++applied; }
  if (Env("BERRY_DEV_MODE", &v) && ParseBoolEnv(v, &b)) { devMode = b; ++applied; }
  if (Env("BERRY_SAFE_MODE", &v) && ParseBoolEnv(v, &b)) { safeMode = b; ++applied; }
  if (Env("BERRY_SCRIPT_DIR", &v)) { scriptDirectory = v; ++applied; }
  if (Env("BERRY_ANCHOR_KEY", &v)) { scriptAnchorKey = v; ++applied; }
  if (Env("BERRY_LAN_IFACE", &v)) { lanIface = v; ++applied; }
  return applied;
}

}  // namespace Berry
