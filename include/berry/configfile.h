#pragma once
// A validated config file (improvement 89).
//
// Loads a JSON document into the engine config, with environment overrides,
// and validates it: an unknown key, a wrong type or an out-of-range value is a
// hard error (fail closed), not a silently-ignored field. The parser is the
// same json_min used everywhere else, so no dependency is added.
//
// Recognised keys (all optional):
//   {
//     "log_level": "info" | "trace" | "debug" | "warn" | "error",
//     "capture_log": true,
//     "auto_attach_modules": true,
//     "enable_render": true,
//     "enable_client": true,
//     "dev_mode": false,
//     "script_directory": "/path",
//     "script_anchor_key": "<64 hex chars>",
//     "script_data_directory": "/path",
//     "safe_mode": false,
//     "enable_cosmetic_sync": false,
//     "enable_voice": false,
//     "lan_iface": "127.0.0.1",
//     "voice_group": "239.1.2.3",
//     "voice_port": 47825,
//     "voice_relay_host": "203.0.113.9",
//     "voice_relay_port": 47825,
//     "voice_room": "room-1",
//     "voice_room_secret": "<hex or raw>"
//   }

#include "berry/export.h"
#include "berry/types.h"

#include <string>
#include <vector>

namespace Berry {

/// The validated, parsed config. Strings own their storage (the C config
/// borrows pointers, so the caller must keep this alive while it is applied).
struct BERRY_API ConfigFile {
  bool present = false;   // false when the file did not exist
  std::string logLevel;
  bool captureLog = false;
  bool autoAttachModules = true;
  bool enableRender = true;
  bool enableClient = true;
  bool devMode = false;
  bool safeMode = false;
  bool enableCosmeticSync = false;
  bool enableVoice = false;
  std::string scriptDirectory;
  std::string scriptAnchorKey;
  std::string scriptDataDirectory;
  std::string lanIface;
  std::string voiceGroup;
  unsigned voicePort = 0;
  std::string voiceRelayHost;
  unsigned voiceRelayPort = 0;
  std::string voiceRoom;
  std::string voiceRoomSecret;

  /// Parses `text` as JSON. Returns false and sets `error` on a syntax error,
  /// an unknown key, a wrong type or an out-of-range value.
  bool Parse(const std::string& text, std::string* error = nullptr);

  /// Loads and parses the file at `path`. A missing file returns true with
  /// `present = false` (a config file is optional); a present-but-invalid file
  /// returns false.
  bool Load(const std::string& path, std::string* error = nullptr);

  /// Applies `BERRY_*` environment overrides on top of the parsed values. Only
  /// a small, explicit allow-list is honoured, so the environment cannot inject
  /// an arbitrary key (improvement 89). Returns the number applied.
  int ApplyEnvOverrides();

  /// Re-reads `path` only when its modification time has advanced since the
  /// last `Load`/`LoadIfChanged` (improvement 89 hot reload). Returns true when
  /// the file was (re)read, and sets `changed` to whether the parsed content
  /// differs from the previous one. A missing file is not an error.
  bool LoadIfChanged(const std::string& path, bool* changed,
                     std::string* error = nullptr);

 private:
  /// The mtime of the last successful load, for LoadIfChanged.
  long long lastMtimeNs_ = 0;
  /// The raw text of the last load, so a change can be detected by content.
  std::string lastText_;
};

}  // namespace Berry
