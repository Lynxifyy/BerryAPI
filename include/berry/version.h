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
#define BERRY_ABI_VERSION 3

// Legacy internal SDK ABI identifier (kept for compatibility).
#define BERRY_SDK_ABI 1

// Strict Berry:: module identifiers.
#define BERRY_MOD_COMBAT "Berry::Combat"
#define BERRY_MOD_RENDER "Berry::Render"
#define BERRY_MOD_WORLD "Berry::World"
#define BERRY_MOD_NETWORK "Berry::Network"