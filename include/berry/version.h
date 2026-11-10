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
// v8: the C surface grew the world-seam entry points (BERRY_SetWorldSeam /
//     BERRY_ClearWorldSeam / BERRY_HasWorldSeam and the BerrySeamC struct), so
//     a host that is not linked against the C++ ABI can bind a LevelSeam and
//     the World module's entity reads stop failing closed.
// v9: BerryConfigC grew the cosmetic fields (cosmetic_pack_directory,
//     cosmetic_asset_directory), so a host can point the client at its cosmetic
//     packs and asset data through the C ABI. The cosmetic surface itself lives
//     in berry/cosmetic_c.h under its own BERRY_COSMETIC_ABI_VERSION, and the
//     world seam now reports BERRY_SEAM_ABI_VERSION (berry/seam.h) instead of
//     this number, so a config-only growth no longer moves the seam's ABI.
#define BERRY_ABI_VERSION 9

// Legacy internal SDK ABI identifier (kept for compatibility).
#define BERRY_SDK_ABI 1

// Strict Berry:: module identifiers.
#define BERRY_MOD_COMBAT "Berry::Combat"
#define BERRY_MOD_RENDER "Berry::Render"
#define BERRY_MOD_WORLD "Berry::World"
#define BERRY_MOD_NETWORK "Berry::Network"