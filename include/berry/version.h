#pragma once
// BerryAPI versioning and ABI identifiers.
#define BERRY_VERSION_MAJOR 2
#define BERRY_VERSION_MINOR 0
#define BERRY_VERSION_PATCH 0
#define BERRY_VERSION_STRING "2.0.0"

// The BerryAPI C ABI version. Bump whenever the exported C surface or the
// layout of a public config / ingest struct changes. Loader tools and the
// library both check this; mismatches are rejected at attach time.
// v3: BerryConfigC grew the client fields (enable_client, dev_mode,
//     script_directory, local_player_id).
// v4: BerryConfigC grew the LAN + persistence fields (enable_cosmetic_sync,
//     sync_verify_key, sync_self_id, sync_sign_secret, sync_use_hmac,
//     enable_voice, voice_self_id, lan_iface, script_data_directory).
#define BERRY_ABI_VERSION 4

// Legacy internal SDK ABI identifier (kept for compatibility).
#define BERRY_SDK_ABI 1

// Strict Berry:: module identifiers.
#define BERRY_MOD_COMBAT "Berry::Combat"
#define BERRY_MOD_RENDER "Berry::Render"
#define BERRY_MOD_WORLD "Berry::World"
#define BERRY_MOD_NETWORK "Berry::Network"