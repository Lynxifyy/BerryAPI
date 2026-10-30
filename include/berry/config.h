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
  // The Ed25519 public key (64 hex chars) a script manifest's signature is
  // verified against, or NULL for no anchor. Without an anchor, and outside
  // dev mode, every manifest is refused (the loader requires a signature), so
  // this is what makes production script loading possible. Borrowed for the
  // call; copied by the client.
  const char* script_anchor_key;
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
  // Presence-backend media plane for cosmetic sync. When sync_relay_host is
  // set, the sync transport is a UDP relay transport instead of the LAN
  // multicast transport, so two clients on different networks reach each other
  // through one relay. The room is keyed by sync_room_secret, which is what a
  // room token (RoomTokenClient) delivers over HTTPS. NULL/empty
  // sync_relay_host keeps the LAN multicast transport.
  const char* sync_relay_host;    // numeric IPv4; NULL/empty = LAN multicast
  unsigned short sync_relay_port;  // 0 = the default (47826)
  const char* sync_room;           // relay room name; NULL = "default"
  const char* sync_room_secret;    // raw room secret (>= 16 bytes)
  // 0/1 enable LAN proximity voice (binds a private multicast socket).
  int enable_voice;
  // This client's voice session id, or NULL to leave the session stopped.
  const char* voice_self_id;
  // The 32-byte Ed25519 seed (raw) that identifies this client on the voice
  // channel, or NULL to disable voice (there is no plaintext fallback). Hex
  // (64 chars) is also accepted.
  const char* voice_identity_seed;
  // The Ed25519 public keys (hex, comma-separated) a peer's voice announce is
  // verified against, or NULL/empty to accept none. Ignored when
  // voice_room_key is set.
  const char* voice_peer_keys;
  // A backend-issued room key (raw bytes) that authenticates voice peers when
  // no per-peer key is distributed. NULL/empty disables room-key mode.
  const char* voice_room_key;
  // The voice multicast group, or NULL for the default. A different group from
  // the cosmetic channel.
  const char* voice_group;
  // The voice port, or 0 for the default.
  unsigned short voice_port;
  // Presence-backend media plane for voice. When voice_relay_host is set, the
  // voice transport is a UDP relay transport instead of the LAN multicast one,
  // so two phones on different networks hear each other through one relay. The
  // room is keyed by voice_room_secret, which a room token (RoomTokenClient)
  // delivers over HTTPS. The relay moves only the end-to-end sealed frame, so
  // it never sees plaintext and holds no key. NULL/empty keeps LAN multicast.
  const char* voice_relay_host;    // numeric IPv4; NULL/empty = LAN multicast
  unsigned short voice_relay_port;  // 0 = the default (47825)
  const char* voice_room;           // relay room name; NULL = "default"
  const char* voice_room_secret;    // raw room secret (>= 16 bytes)
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