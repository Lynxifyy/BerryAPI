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
// v5: BerryConfigC grew the voice-security fields (voice_identity_seed,
//     voice_peer_keys, voice_room_key, voice_group, voice_port), so voice is
//     authenticated and encrypted rather than plaintext.
// v6: BerryConfigC grew the script trust anchor (script_anchor_key), so a
//     signed manifest can be verified through the C ABI rather than only in a
//     test that constructs ClientConfig directly.
// v7: BerryConfigC grew the voice-relay fields (voice_relay_host,
//     voice_relay_port, voice_room, voice_room_secret), so a client can select
//     the relay media plane through the C ABI, not only the LAN multicast one.
#define BERRY_ABI_VERSION 7

// Legacy internal SDK ABI identifier (kept for compatibility).
#define BERRY_SDK_ABI 1

// Strict Berry:: module identifiers.
#define BERRY_MOD_COMBAT "Berry::Combat"
#define BERRY_MOD_RENDER "Berry::Render"
#define BERRY_MOD_WORLD "Berry::World"
#define BERRY_MOD_NETWORK "Berry::Network"