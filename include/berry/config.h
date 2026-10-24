#pragma once
// BerryAPI C ABI configuration structs.
//
// The injector entry points are plain C so any loader tool (dlopen, JNI,
// preloader, or a C host) can build a config value without dragging in C++
// headers or relying on the C++ ABI. Every public config struct starts with
//   uint32_t size;        // sizeof(*this), verified by the library
//   uint32_t abi_version; // BERRY_ABI_VERSION, verified by the library
//
// A caller initializes the struct, sets `size = sizeof(cfg)`,
// `abi_version = BERRY_ABI_VERSION`, fills the fields it understands, and
// passes the pointer. The library rejects mismatches (fail closed).

#include "berry/version.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// 0 = default (keeps the library's compiled-in level); 1..5 explicit levels.
enum BerryLogLevelC {
  BerryLogDefaultC = 0,
  BerryLogTraceC = 1,
  BerryLogDebugC = 2,
  BerryLogInfoC = 3,
  BerryLogWarnC = 4,
  BerryLogErrorC = 5
};

// Canonical injector configuration. C-compatible; every field has a safe
// default so zero-initializing the struct is equivalent to defaults.
typedef struct BerryConfigC {
  uint32_t size;         // must be sizeof(BerryConfigC)
  uint32_t abi_version;  // must be BERRY_ABI_VERSION
  int log_level;         // BerryLogLevelC; -1 keeps the default (info)
  int capture_log;       // 0/1 keep a capture buffer for the log viewer
  int auto_attach_modules;  // 0/1 register + attach the four strict modules
  int enable_render;        // 0/1 allow the render overlay
  int enable_client;        // 0/1 build the client composition root (services)
  int dev_mode;             // 0/1 skip manifest signature/source-hash checks
  // The directory the client loads a script from, or NULL for none. The string
  // is borrowed for the duration of the call and copied by the client.
  const char* script_directory;
  // The local player's runtime entity id, for player/world reads and the voice
  // proximity check. 0 means unknown (those reads fail closed).
  uint64_t local_player_id;
  // --- LAN channels (opt-in; each binds a private multicast socket) ---
  // 0/1 enable the cosmetic-sync channel: a private multicast transport that
  // carries the local loadout and applies verified remote ones.
  int enable_cosmetic_sync;
  // The key used to verify inbound cosmetic loadouts, hex-encoded (an Ed25519
  // public key, or an HMAC secret when sync_use_hmac is set). NULL or empty
  // leaves the channel unconfigured, so every inbound message is refused.
  // Borrowed for the call; copied by the client.
  const char* sync_verify_key;
  // This client's own id on the sync channel, or NULL to only verify/apply.
  const char* sync_self_id;
  // The secret used to sign the local loadout before publishing, or NULL to
  // never publish. A 64-byte Ed25519 secret as hex, or an HMAC secret when
  // sync_use_hmac is set.
  const char* sync_sign_secret;
  // 0 = Ed25519 (production), 1 = HMAC (local development).
  int sync_use_hmac;
  // 0/1 enable LAN proximity voice (binds a private multicast socket).
  int enable_voice;
  // This client's voice session id, or NULL to leave the session stopped.
  const char* voice_self_id;
  // The interface the LAN channels join and send on, or NULL for the system
  // multicast route. A host test passes "127.0.0.1" to stay on loopback.
  const char* lan_iface;
  // Root directory for the sandboxed `fs.*` store, or NULL for in-memory only.
  // When set, the client binds the store to a per-script subdirectory (named
  // from the loaded manifest) so a script's data survives a restart. The store
  // refuses a symlink on every path component and writes through a temp file
  // plus rename, so it cannot escape the directory or expose a partial write.
  const char* script_data_directory;
} BerryConfigC;

#ifdef __cplusplus
}
#endif