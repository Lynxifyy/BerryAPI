# Single source of truth for the BerryAPI translation units.
#
# Both the host Makefile and tools/build-android.sh read this file, so the two
# builds cannot drift apart (a source added for tests is automatically added to
# the injectable library). `make check-sources` fails if any .cpp under core/
# or util/ is missing from these lists.
#
# CORE_SRC           portable units, built for host tests and the shared lib.
# ANDROID_EXTRA_SRC  units compiled only for the on-device build. They are
#                    guarded by BERRY_HOOK_HAVE_DOBBY, so the host build skips
#                    them rather than compiling empty objects.

CORE_SRC := \
  core/client/client.cpp \
  core/events/event.cpp \
  core/hooks/sigscan.cpp \
  core/hooks/backend_shim.cpp \
  core/modules/module.cpp \
  core/modules/combat.cpp \
  core/modules/combat_state.cpp \
  core/modules/animator.cpp \
  core/modules/replay_camera.cpp \
  core/modules/radial.cpp \
  core/modules/crystal.cpp \
  core/modules/voice.cpp \
  core/modules/render.cpp \
  core/modules/render_backend.cpp \
  core/modules/pipeline.cpp \
  core/modules/services.cpp \
  core/modules/world.cpp \
  core/modules/world_query.cpp \
  core/modules/hud.cpp \
  core/modules/ui.cpp \
  core/modules/input.cpp \
  core/modules/pvp.cpp \
  core/modules/cosmetic.cpp \
  core/modules/shader.cpp \
  core/modules/governor.cpp \
  core/lifecycle/engine.cpp \
  core/lifecycle/thread.cpp \
  core/net/net.cpp \
  core/net/tls.cpp \
  core/net/ws_client.cpp \
  core/net/cosmetic_sync.cpp \
  core/net/lan_transport.cpp \
  core/net/lan_discovery.cpp \
  core/net/voice_transport.cpp \
  core/script/crypto.cpp \
  core/script/ed25519.cpp \
  core/script/manifest.cpp \
  core/script/runtime.cpp \
  core/script/vm_ref.cpp \
  core/script/luau_vm.cpp \
  core/script/value_codec.cpp \
  core/script/host.cpp \
  core/script/reload.cpp \
  core/script/loader.cpp \
  core/script/pack.cpp \
  core/script/async.cpp \
  core/script/url.cpp \
  core/script/api_bindings.cpp \
  core/script/intent.cpp \
  core/script/package.cpp \
  core/script/scriptmod.cpp \
  core/script/fs.cpp \
  core/script/audio.cpp \
  core/script/packet.cpp \
  core/script/record.cpp \
  core/replay/replay_format.cpp \
  core/replay/replay_capture.cpp \
  core/replay/replay_world.cpp \
  core/replay/replay_review.cpp \
  core/replay/replay_server.cpp \
  core/replay/replay_service.cpp \
  core/script/jsonrpc_client.cpp \
  core/net/rpc.cpp \
  core/script/layout.cpp \
  core/script/sigpack.cpp \
  core/script/session_recorder.cpp \
  util/logger.cpp \
  util/mem.cpp

ANDROID_EXTRA_SRC := \
  core/hooks/backend_dobby.cpp

# Vendored Ed25519 (C). Compiled into the same objects as everything else, so
# the injectable library keeps its no-external-deps property.
ED25519_SRC := \
  third_party/ed25519/fe.c \
  third_party/ed25519/ge.c \
  third_party/ed25519/sc.c \
  third_party/ed25519/sha512.c \
  third_party/ed25519/keypair.c \
  third_party/ed25519/sign.c \
  third_party/ed25519/verify.c

# Vendored LZ4 (C): the block compressor behind the replay container
# (berry/replay_format.h). Only lib/ is vendored (BSD-2-Clause); the block API
# is used directly, so no LZ4 frame layer is needed. See
# third_party/lz4/PROVENANCE.md.
LZ4_SRC := \
  third_party/lz4/lib/lz4.c \
  third_party/lz4/lib/lz4hc.c

# Vendored mbedTLS (C): the client-side TLS 1.2/1.3 stack behind Berry::TlsSocket.
# It is compiled with the trimmed third_party/mbedtls/include/mbedtls/
# mbedtls_config.h, so a unit that is entirely `#if`-ed out is not listed here
# (see third_party/mbedtls/PROVENANCE.md). Compiled as C99 with the vendored
# include roots; MBEDTLS_CONFIG_FILE points at the vendored config.
MBEDTLS_SRC := \
  third_party/mbedtls/library/aes.c \
  third_party/mbedtls/library/asn1parse.c \
  third_party/mbedtls/library/asn1write.c \
  third_party/mbedtls/library/base64.c \
  third_party/mbedtls/library/bignum.c \
  third_party/mbedtls/library/bignum_core.c \
  third_party/mbedtls/library/bignum_mod.c \
  third_party/mbedtls/library/bignum_mod_raw.c \
  third_party/mbedtls/library/block_cipher.c \
  third_party/mbedtls/library/ccm.c \
  third_party/mbedtls/library/chacha20.c \
  third_party/mbedtls/library/chachapoly.c \
  third_party/mbedtls/library/cipher.c \
  third_party/mbedtls/library/cipher_wrap.c \
  third_party/mbedtls/library/cmac.c \
  third_party/mbedtls/library/constant_time.c \
  third_party/mbedtls/library/ctr_drbg.c \
  third_party/mbedtls/library/dhm.c \
  third_party/mbedtls/library/ecdh.c \
  third_party/mbedtls/library/ecdsa.c \
  third_party/mbedtls/library/ecp.c \
  third_party/mbedtls/library/ecp_curves.c \
  third_party/mbedtls/library/ecp_curves_new.c \
  third_party/mbedtls/library/entropy.c \
  third_party/mbedtls/library/entropy_poll.c \
  third_party/mbedtls/library/error.c \
  third_party/mbedtls/library/gcm.c \
  third_party/mbedtls/library/hkdf.c \
  third_party/mbedtls/library/hmac_drbg.c \
  third_party/mbedtls/library/md.c \
  third_party/mbedtls/library/oid.c \
  third_party/mbedtls/library/pem.c \
  third_party/mbedtls/library/pk.c \
  third_party/mbedtls/library/pk_ecc.c \
  third_party/mbedtls/library/pk_wrap.c \
  third_party/mbedtls/library/pkcs5.c \
  third_party/mbedtls/library/pkparse.c \
  third_party/mbedtls/library/platform.c \
  third_party/mbedtls/library/platform_util.c \
  third_party/mbedtls/library/poly1305.c \
  third_party/mbedtls/library/psa_crypto.c \
  third_party/mbedtls/library/psa_crypto_aead.c \
  third_party/mbedtls/library/psa_crypto_cipher.c \
  third_party/mbedtls/library/psa_crypto_client.c \
  third_party/mbedtls/library/psa_crypto_driver_wrappers_no_static.c \
  third_party/mbedtls/library/psa_crypto_ecp.c \
  third_party/mbedtls/library/psa_crypto_ffdh.c \
  third_party/mbedtls/library/psa_crypto_hash.c \
  third_party/mbedtls/library/psa_crypto_mac.c \
  third_party/mbedtls/library/psa_crypto_pake.c \
  third_party/mbedtls/library/psa_crypto_rsa.c \
  third_party/mbedtls/library/psa_crypto_se.c \
  third_party/mbedtls/library/psa_crypto_slot_management.c \
  third_party/mbedtls/library/psa_crypto_storage.c \
  third_party/mbedtls/library/psa_its_file.c \
  third_party/mbedtls/library/psa_util.c \
  third_party/mbedtls/library/rsa.c \
  third_party/mbedtls/library/rsa_alt_helpers.c \
  third_party/mbedtls/library/sha1.c \
  third_party/mbedtls/library/sha256.c \
  third_party/mbedtls/library/sha512.c \
  third_party/mbedtls/library/ssl_ciphersuites.c \
  third_party/mbedtls/library/ssl_client.c \
  third_party/mbedtls/library/ssl_debug_helpers_generated.c \
  third_party/mbedtls/library/ssl_msg.c \
  third_party/mbedtls/library/ssl_tls.c \
  third_party/mbedtls/library/ssl_tls12_client.c \
  third_party/mbedtls/library/ssl_tls13_client.c \
  third_party/mbedtls/library/ssl_tls13_generic.c \
  third_party/mbedtls/library/ssl_tls13_keys.c \
  third_party/mbedtls/library/threading.c \
  third_party/mbedtls/library/version.c \
  third_party/mbedtls/library/version_features.c \
  third_party/mbedtls/library/x509.c \
  third_party/mbedtls/library/x509_crt.c

# Include roots mbedTLS expects. MBEDTLS_CONFIG_FILE names the vendored config
# explicitly so a system mbedTLS config on the include path cannot leak in.
MBEDTLS_INCLUDES := -Ithird_party/mbedtls/include -DMBEDTLS_CONFIG_FILE='"mbedtls/mbedtls_config.h"'

TEST_SRC := \
  tests/test_math.cpp \
  tests/test_modules.cpp \
  tests/test_net.cpp \
  tests/test_lan_transport.cpp \
  tests/test_jsonrpc.cpp \
  tests/test_render.cpp \
  tests/test_sample.cpp \
  tests/test_sigscan.cpp \
  tests/test_thread.cpp \
  tests/test_fuzz.cpp \
  tests/test_manifest.cpp \
  tests/test_script.cpp \
  tests/test_reload.cpp \
  tests/test_async.cpp \
  tests/test_luau.cpp \
  tests/test_idl.cpp \
  tests/test_hud.cpp \
  tests/test_ui.cpp \
  tests/test_governor.cpp \
  tests/test_url.cpp \
  tests/test_loader.cpp \
  tests/test_api_bindings.cpp \
  tests/test_cosmetic.cpp \
  tests/test_pack.cpp \
  tests/test_intent.cpp \
  tests/test_shader.cpp \
  tests/test_package.cpp \
  tests/test_layout.cpp \
  tests/test_world_query.cpp \
  tests/test_record.cpp \
  tests/test_rpc.cpp \
  tests/test_scriptmod.cpp \
  tests/test_fs.cpp \
  tests/test_audio.cpp \
  tests/test_packet.cpp \
  tests/test_profiler.cpp \
  tests/test_sigpack.cpp \
  tests/test_session_recorder.cpp \
  tests/test_combat_state.cpp \
  tests/test_pipeline.cpp \
  tests/test_animator.cpp \
  tests/test_replay_camera.cpp \
  tests/test_radial.cpp \
  tests/test_crystal.cpp \
  tests/test_voice.cpp \
  tests/test_replay_format.cpp \
  tests/test_replay_capture.cpp \
  tests/test_replay_world.cpp \
  tests/test_replay_review.cpp \
  tests/test_replay_server.cpp \
  tests/test_replay_service.cpp

# Vendored Luau (MIT). See third_party/luau/PROVENANCE.md for the pinned
# version and why these units and not the CLI. Compiled with -w because upstream
# is only warning-clean under its own flag set; our code stays -Wall -Wextra clean.
LUAU_AST_SRC := \
  third_party/luau/Ast/src/Ast.cpp \
  third_party/luau/Ast/src/Confusables.cpp \
  third_party/luau/Ast/src/Lexer.cpp \
  third_party/luau/Ast/src/Location.cpp \
  third_party/luau/Ast/src/Parser.cpp \
  third_party/luau/Ast/src/StringUtils.cpp \
  third_party/luau/Ast/src/TimeTrace.cpp

LUAU_COMPILER_SRC := \
  third_party/luau/Compiler/src/BuiltinFolding.cpp \
  third_party/luau/Compiler/src/Builtins.cpp \
  third_party/luau/Compiler/src/BytecodeBuilder.cpp \
  third_party/luau/Compiler/src/Compiler.cpp \
  third_party/luau/Compiler/src/ConstantFolding.cpp \
  third_party/luau/Compiler/src/CostModel.cpp \
  third_party/luau/Compiler/src/TableShape.cpp \
  third_party/luau/Compiler/src/Types.cpp \
  third_party/luau/Compiler/src/ValueTracking.cpp \
  third_party/luau/Compiler/src/lcode.cpp

LUAU_VM_SRC := \
  third_party/luau/VM/src/lapi.cpp \
  third_party/luau/VM/src/laux.cpp \
  third_party/luau/VM/src/lbaselib.cpp \
  third_party/luau/VM/src/lbitlib.cpp \
  third_party/luau/VM/src/lbuffer.cpp \
  third_party/luau/VM/src/lbuflib.cpp \
  third_party/luau/VM/src/lbuiltins.cpp \
  third_party/luau/VM/src/lcorolib.cpp \
  third_party/luau/VM/src/ldblib.cpp \
  third_party/luau/VM/src/ldebug.cpp \
  third_party/luau/VM/src/ldo.cpp \
  third_party/luau/VM/src/lfunc.cpp \
  third_party/luau/VM/src/lgc.cpp \
  third_party/luau/VM/src/lgcdebug.cpp \
  third_party/luau/VM/src/linit.cpp \
  third_party/luau/VM/src/lmathlib.cpp \
  third_party/luau/VM/src/lmem.cpp \
  third_party/luau/VM/src/lnumprint.cpp \
  third_party/luau/VM/src/lobject.cpp \
  third_party/luau/VM/src/loslib.cpp \
  third_party/luau/VM/src/lperf.cpp \
  third_party/luau/VM/src/lstate.cpp \
  third_party/luau/VM/src/lstring.cpp \
  third_party/luau/VM/src/lstrlib.cpp \
  third_party/luau/VM/src/ltable.cpp \
  third_party/luau/VM/src/ltablib.cpp \
  third_party/luau/VM/src/ltm.cpp \
  third_party/luau/VM/src/ludata.cpp \
  third_party/luau/VM/src/lutf8lib.cpp \
  third_party/luau/VM/src/lvmexecute.cpp \
  third_party/luau/VM/src/lvmload.cpp \
  third_party/luau/VM/src/lvmutils.cpp

LUAU_SRC := $(LUAU_AST_SRC) $(LUAU_COMPILER_SRC) $(LUAU_VM_SRC)

# Include roots Luau expects (Common is header-only).
LUAU_INCLUDES := -Ithird_party/luau/Common/include -Ithird_party/luau/Ast/include -Ithird_party/luau/Compiler/include -Ithird_party/luau/VM/include

# Include root LZ4 expects (its headers are used as "lz4.h").
LZ4_INCLUDES := -Ithird_party/lz4/lib
