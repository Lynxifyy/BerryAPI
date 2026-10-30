#!/usr/bin/env python3
"""Verify the Android build's source list against tools/sources.mk.

The on-device build (tools/build-android.sh) and the host build both read
tools/sources.mk, so they cannot drift. This script proves that claim without
an NDK: it asks the build script for the exact set of units it would compile
(`--sources-only`) and checks it three ways.

  1. Every portable unit under core/ and util/ is compiled. A new .cpp that is
     added to the tree but never listed would otherwise ship in neither build.
     The device-only Dobby glue (core/hooks/backend_dobby.cpp) is the one
     deliberate exception, and it must be present too.
  2. The subsystems that only matter on a device are present: mbedTLS (verified
     TLS), lz4 (replay blocks), ed25519 (signature verification), the replay
     container, the client host, the client-side modules and the secure voice
     path. A device build that silently dropped one of these would attach but
     fail at runtime, which is exactly the bug this catches.
  3. The vendored C units a device needs are present (mbedTLS TLS/x509/ChaCha20
     -Poly1305, lz4, ed25519, the Luau VM).

Exit code 0 when the lists agree, 1 when a unit is missing, 2 on a setup error.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD_SCRIPT = os.path.join(ROOT, "tools", "build-android.sh")

# Compiled only for the device; it is guarded by BERRY_HOOK_HAVE_DOBBY so the
# host build skips it rather than compiling an empty object.
DEVICE_ONLY = {"core/hooks/backend_dobby.cpp"}

# Device-only units whose absence would leave a feature that exists in the tree
# unreachable on the device. Each is (path, why) so a failure explains itself.
REQUIRED_CPP = [
    ("core/client/client.cpp", "the client host"),
    ("core/script/sigpack.cpp", "signature-pack verification"),
    ("core/script/scripttest.cpp", "the script test framework"),
    ("core/script/fs.cpp", "the sandboxed script filesystem"),
    ("core/script/intent.cpp", "the capability/policy intent system"),
    ("core/script/crypto.cpp", "signing primitives"),
    ("core/script/ed25519.cpp", "Ed25519 verification"),
    ("core/replay/replay_format.cpp", "the replay container"),
    ("core/replay/replay_capture.cpp", "replay capture"),
    ("core/replay/replay_world.cpp", "replay world playback"),
    ("core/replay/replay_review.cpp", "the replay review overlay"),
    ("core/replay/replay_service.cpp", "the replay service"),
    ("core/modules/cosmetic.cpp", "the cosmetic service"),
    ("core/modules/cosmetic_assets.cpp", "the geometry/texture loader"),
    ("core/modules/cosmetic_device.cpp", "the device cosmetic render seam"),
    ("core/modules/cosmetic_gles.cpp", "the GLES cosmetic backend"),
    ("core/modules/animator.cpp", "cosmetic animation"),
    ("core/modules/crystal.cpp", "the crystal advisor"),
    ("core/modules/voice.cpp", "proximity voice"),
    ("core/modules/voice_crypto.cpp", "voice encryption"),
    ("core/net/voice_secure_transport.cpp", "the authenticated voice channel"),
    ("core/net/tls.cpp", "verified TLS"),
    ("core/net/multicast_lock.cpp", "the Android multicast lock"),
]

# Vendored C the device build must carry, as "<category> <path>".
REQUIRED_VENDORED = [
    ("TLS", "third_party/mbedtls/library/ssl_tls.c", "the TLS handshake"),
    ("TLS", "third_party/mbedtls/library/x509_crt.c", "certificate-chain verification"),
    ("TLS", "third_party/mbedtls/library/chachapoly.c", "ChaCha20-Poly1305 (voice)"),
    ("LZ4", "third_party/lz4/lib/lz4.c", "replay block compression"),
    ("CC", "third_party/ed25519/verify.c", "Ed25519 signature verification"),
]

LUAU_VM_RE = re.compile(r"^LUAU third_party/luau/VM/src/lvmexecute\.c(pp)?$")


def read_source_list() -> list[tuple[str, str]]:
    """Returns [(category, path)] from `build-android.sh --sources-only`."""
    proc = subprocess.run(
        ["bash", BUILD_SCRIPT, "--sources-only"],
        cwd=ROOT, capture_output=True, text=True,
    )
    if proc.returncode != 0:
        sys.stderr.write(proc.stderr)
        sys.stderr.write("check-android-sources: --sources-only failed\n")
        raise SystemExit(2)
    entries: list[tuple[str, str]] = []
    for line in proc.stdout.splitlines():
        line = line.strip()
        if not line:
            continue
        cat, _, path = line.partition(" ")
        entries.append((cat, path))
    if not entries:
        sys.stderr.write("check-android-sources: the source list is empty\n")
        raise SystemExit(2)
    return entries


def host_units() -> list[str]:
    """Every .cpp under core/ and util/, relative to the repo root, sorted."""
    out: list[str] = []
    for base in ("core", "util"):
        for dirpath, _dirs, files in os.walk(os.path.join(ROOT, base)):
            for name in files:
                if name.endswith(".cpp"):
                    rel = os.path.relpath(os.path.join(dirpath, name), ROOT)
                    out.append(rel.replace(os.sep, "/"))
    return sorted(out)


def main() -> int:
    entries = read_source_list()
    by_cat: dict[str, set[str]] = {}
    for cat, path in entries:
        by_cat.setdefault(cat, set()).add(path)

    cxx = by_cat.get("CXX", set())
    problems: list[str] = []

    # 1. Completeness: every portable unit, plus the device-only glue.
    for unit in host_units():
        if unit in DEVICE_ONLY:
            continue
        if unit not in cxx:
            problems.append(f"{unit} is not compiled by the Android build")
    for unit in sorted(DEVICE_ONLY):
        if unit not in cxx:
            problems.append(f"{unit} is missing (the Dobby hook backend)")

    # 2. The device-relevant subsystems.
    for unit, why in REQUIRED_CPP:
        if unit not in cxx:
            problems.append(f"{unit} is missing ({why})")

    # 3. The vendored C.
    for cat, unit, why in REQUIRED_VENDORED:
        if unit not in by_cat.get(cat, set()):
            problems.append(f"{unit} is missing from the {cat} set ({why})")
    if not any(LUAU_VM_RE.match(f"{c} {p}") for c, p in entries):
        problems.append("the Luau VM (lvmexecute) is missing")

    if problems:
        for p in problems:
            sys.stderr.write(f"check-android-sources: {p}\n")
        return 1

    counts = ", ".join(f"{len(by_cat.get(c, ()))} {c}" for c in
                       ("CXX", "LUAU", "CC", "LZ4", "TLS"))
    print(f"== Android source list matches tools/sources.mk ({counts}) ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
