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
  core/modules/hud.cpp \
  core/modules/input.cpp \
  core/modules/pvp.cpp \
  core/modules/governor.cpp \
  core/lifecycle/engine.cpp \
  core/lifecycle/thread.cpp \
  core/net/net.cpp \
  core/script/crypto.cpp \
  core/script/manifest.cpp \
  core/script/runtime.cpp \
  core/script/vm_ref.cpp \
  core/script/host.cpp \
  core/script/reload.cpp \
  core/script/async.cpp \
  util/logger.cpp \
  util/mem.cpp

ANDROID_EXTRA_SRC := \
  core/hooks/backend_dobby.cpp

TEST_SRC := \
  tests/test_math.cpp \
  tests/test_modules.cpp \
  tests/test_net.cpp \
  tests/test_render.cpp \
  tests/test_sample.cpp \
  tests/test_sigscan.cpp \
  tests/test_thread.cpp \
  tests/test_fuzz.cpp \
  tests/test_manifest.cpp \
  tests/test_script.cpp \
  tests/test_reload.cpp \
  tests/test_async.cpp \
  tests/test_idl.cpp \
  tests/test_hud.cpp \
  tests/test_governor.cpp
