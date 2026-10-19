#!/usr/bin/env bash
# Build BerryAPI for Android (aarch64) using the Android NDK + Dobby.
#
# Usage:
#   ./tools/build-android.sh [NDK_PATH]
#
# Env:
#   NDK_PATH       Android NDK root (or pass as $1). If unset, uses
#                  $ANDROID_NDK_HOME / $ANDROID_NDK_ROOT.
#   BERRY_ABI      target ABI: arm64-v8a (default) | armeabi-v7a
#   BERRY_DOBBY    1 to compile the vendored Dobby from source (default);
#                  0 to link an existing libdobby.so (must be in NDK sysroot).
#
# The output is build-android/<ABI>/libBerryAPI.so — a PIC shared library with
# only BERRY_* exports. Host builds are unaffected (this script is separate).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------------------------------------------------------------------------
# Locate NDK
# ---------------------------------------------------------------------------
find_ndk() {
  local cands=()
  [[ -n "${1:-}" ]] && cands+=("$1")
  [[ -n "${ANDROID_NDK_HOME:-}" ]] && cands+=("$ANDROID_NDK_HOME")
  [[ -n "${ANDROID_NDK_ROOT:-}" ]] && cands+=("$ANDROID_NDK_ROOT")
  [[ -n "${ANDROID_HOME:-}" ]] && cands+=("$ANDROID_HOME/ndk/$(ls -1 "$ANDROID_HOME/ndk" 2>/dev/null | head -1)")
  for c in "${cands[@]}"; do
    if [[ -n "$c" && -x "$c/toolchains/llvm/prebuilt/linux-x86_64/bin/clang" ]]; then
      echo "$c"
      return 0
    fi
  done
  return 1
}

NDK="$(find_ndk "${1:-}" || true)"
if [[ -z "$NDK" ]]; then
  echo "build-android: Android NDK not found." >&2
  echo "  Install one and re-run, e.g.:" >&2
  echo "    ./tools/build-android.sh ~/Android/Sdk/ndk/27.0.12077973" >&2
  echo "  Or export ANDROID_NDK_HOME." >&2
  exit 2
fi
echo "build-android: NDK = $NDK"

ABI="${BERRY_ABI:-arm64-v8a}"
case "$ABI" in
  arm64-v8a)  TOOL="aarch64-linux-android24-clang";  TRIPLE="aarch64-linux-android" ;;
  armeabi-v7a) TOOL="armv7a-linux-androideabi24-clang"; TRIPLE="armv7a-linux-androideabi" ;;
  *) echo "Unsupported BERRY_ABI=$ABI (use arm64-v8a|armeabi-v7a)" >&2; exit 2 ;;
esac

BIN="$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
CC="$BIN/$TOOL"
CXX="$BIN/$TOOL++"
AR="$BIN/llvm-ar"
RANLIB="$BIN/llvm-ranlib"
STRIP="$BIN/llvm-strip"

OUT="$ROOT/build-android/$ABI"
mkdir -p "$OUT"
echo "build-android: ABI=$ABI out=$OUT"

COMMON_FLAGS=(-std=c++17 -O2 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti
              "-DBERRY_BUILD_SHARED" "-DBERRY_PLATFORM_ANDROID=1"
              "-I$ROOT/include")
LDFLAGS=(-shared -fvisibility=hidden -Wl,--gc-sections -Wl,-z,relro -Wl,-z,now)

# ---------------------------------------------------------------------------
# Dobby (vendored). Compile from source by default.
# ---------------------------------------------------------------------------
DOBBY_OBJS=()
if [[ "${BERRY_DOBBY:-1}" == "1" ]]; then
  if [[ ! -d "$ROOT/third_party/Dobby/source" ]]; then
    echo "build-android: third_party/Dobby missing (run vendor script?)" >&2
    exit 2
  fi
  # Dobby full build needs its CMake (thunks / platform glue per ABI). Use it.
  if command -v cmake >/dev/null 2>&1; then
    DOBBY_PREFIX="$ROOT/build-android/dobby-$ABI"
    if ! "$CC" --version >/dev/null 2>&1; then
      echo "build-android: clang missing for $ABI" >&2
      exit 2
    fi
    # Most NDKs bundle a CMake toolchain; Dobby also works with a direct
    # NDK cross file. We drive CMake with the clang toolchain manually.
    cmake -S "$ROOT/third_party/Dobby" -B "$DOBBY_PREFIX" \
      -DCMAKE_SYSTEM_NAME=Android \
      -DCMAKE_SYSTEM_VERSION=24 \
      -DCMAKE_ANDROID_ARCH_ABI="$ABI" \
      -DCMAKE_ANDROID_NDK="$NDK" \
      -DCMAKE_ANDROID_STL_TYPE=c++_static \
      -DCMAKE_BUILD_TYPE=Release \
      -DBUILD_TESTING=OFF -DDOBBY_GENERATE_SHARED=ON \
      >/dev/null
    cmake --build "$DOBBY_PREFIX" --target dobby -- -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
    DOBBY_LIB="$DOBBY_PREFIX/libdobby.so"
    LDFLAGS+=("$DOBBY_LIB")
  else
    echo "build-android: cmake required to build Dobby from source" >&2
    exit 2
  fi
  COMMON_FLAGS+=("-DBERRY_HOOK_ENGINE_DOBBY=1" "-DHOOK_MACHINE_ARCH_ARM64")
  LDFLAGS+=("-ldl" "-llog")
fi

# ---------------------------------------------------------------------------
# Compile BerryAPI core.
# ---------------------------------------------------------------------------
SRCS=(
  core/events/event.cpp
  core/hooks/sigscan.cpp
  core/hooks/backend_shim.cpp
  core/hooks/backend_dobby.cpp
  core/modules/module.cpp
  core/modules/combat.cpp
  core/modules/render.cpp
  core/modules/render_backend.cpp
  core/modules/services.cpp
  core/modules/world.cpp
  core/lifecycle/engine.cpp
  core/net/net.cpp
  util/logger.cpp
  util/mem.cpp
)

OBJS=()
for s in "${SRCS[@]}"; do
  obj="$OUT/${s//\//_}.o"
  echo "  CC $s"
  "$CXX" "${COMMON_FLAGS[@]}" -c "$ROOT/$s" -o "$obj"
  OBJS+=("$obj")
done

"$CXX" "${LDFLAGS[@]}" "${OBJS[@]}" -o "$OUT/libBerryAPI.so"
"$STRIP" --strip-unneeded "$OUT/libBerryAPI.so"
echo "build-android: OK -> $OUT/libBerryAPI.so"
file "$OUT/libBerryAPI.so"