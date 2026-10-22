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
  core/events/event.cpp \
  core/hooks/sigscan.cpp \
  core/hooks/backend_shim.cpp \
  core/modules/module.cpp \
  core/modules/combat.cpp \
  core/modules/render.cpp \
  core/modules/render_backend.cpp \
  core/modules/services.cpp \
  core/modules/world.cpp \
  core/modules/world_query.cpp \
  core/modules/hud.cpp \
  core/modules/input.cpp \
  core/modules/pvp.cpp \
  core/modules/cosmetic.cpp \
  core/modules/shader.cpp \
  core/modules/governor.cpp \
  core/lifecycle/engine.cpp \
  core/lifecycle/thread.cpp \
  core/net/net.cpp \
  core/net/ws_client.cpp \
  core/net/cosmetic_sync.cpp \
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

TEST_SRC := \
  tests/test_math.cpp \
  tests/test_modules.cpp \
  tests/test_net.cpp \
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
  tests/test_session_recorder.cpp
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
