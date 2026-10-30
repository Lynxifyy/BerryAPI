#!/usr/bin/env python3
"""Keep the vendored mbedTLS honest: version pin, config trim and unit set.

mbedTLS is a security dependency, so three things must not drift silently.

  1. The version pin agrees in all three places it is written: this script's
     PINNED constant, `third_party/mbedtls/include/mbedtls/build_info.h` (what
     actually compiles) and `third_party/mbedtls/PROVENANCE.md` (what a human
     reads). A half-applied upgrade — headers moved but the library sources
     not, or vice versa — is exactly what this catches.
  2. The vendored config is the *trimmed* one, not upstream's default: the
     client-only features `Berry::TlsSocket` needs are defined, and the server
     role, DTLS, the legacy ciphers and the PSK key exchanges are not. Vendoring
     the full config would silently ship a TLS server and code we never test.
  3. Every `library/*.c` on disk is either compiled (listed in
     `tools/sources.mk`) or one of the documented exclusions that compile to an
     empty object under the trimmed config. A new unit in a point release that
     nobody adds to sources.mk would otherwise link-fail at best and drop a
     security fix at worst.

Exit code 0 when everything agrees, 1 on a mismatch, 2 on a setup error.

With `--upstream` it additionally asks the GitHub releases API whether the
pinned 3.6 patch is the newest in the line and prints an advisory when it is
not. That half needs the network, so it is opt-in and never runs in `make test`.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MBEDTLS = os.path.join(ROOT, "third_party", "mbedtls")
BUILD_INFO = os.path.join(MBEDTLS, "include", "mbedtls", "build_info.h")
CONFIG = os.path.join(MBEDTLS, "include", "mbedtls", "mbedtls_config.h")
PROVENANCE = os.path.join(MBEDTLS, "PROVENANCE.md")
SOURCES_MK = os.path.join(ROOT, "tools", "sources.mk")
MAKEFILE = os.path.join(ROOT, "Makefile")

# The pinned 3.6 LTS patch. Bump this, build_info.h and PROVENANCE.md together.
PINNED = "3.6.7"
LTS_LINE = "3.6"

# Units kept on disk but not compiled by the shipping library: under the
# trimmed config every one of them `#if`s out to an empty object, so compiling
# them would only add empty .o files. The set is documented in PROVENANCE.md
# and mirrored here so the two cannot diverge. `ssl_tls12_server.c` /
# `ssl_tls13_server.c` are not in this set: the host tests compile them with
# the test config (see MBEDTLS_TEST_EXTRA_SRC in the Makefile).
EXCLUDED_UNITS = {
    "camellia.c", "memory_buffer_alloc.c", "mps_trace.c", "net_sockets.c",
    "ripemd160.c", "ssl_cookie.c", "ssl_ticket.c", "x509_create.c",
    "x509_crl.c", "x509_csr.c", "x509write.c", "x509write_crt.c",
    "x509write_csr.c",
}

# Features the verified TLS client cannot work without.
REQUIRED_FEATURES = [
    "MBEDTLS_SSL_CLI_C",
    "MBEDTLS_SSL_PROTO_TLS1_2",
    "MBEDTLS_SSL_PROTO_TLS1_3",
    "MBEDTLS_X509_CRT_PARSE_C",
    "MBEDTLS_X509_USE_C",
    "MBEDTLS_HAVE_ASM",
    "MBEDTLS_PSA_CRYPTO_C",
]

# Features trimmed on purpose. Their presence means the full upstream config
# leaked in and the library now carries code no test exercises.
FORBIDDEN_FEATURES = [
    "MBEDTLS_SSL_SRV_C",
    "MBEDTLS_SSL_PROTO_DTLS",
    "MBEDTLS_NET_C",
    "MBEDTLS_TIMING_C",
    "MBEDTLS_DEBUG_C",
    "MBEDTLS_MD5_C",
    "MBEDTLS_SHA3_C",
    "MBEDTLS_DES_C",
    "MBEDTLS_KEY_EXCHANGE_PSK_ENABLED",
    "MBEDTLS_X509_CRT_WRITE_C",
]


def defines(text: str) -> set[str]:
    return set(re.findall(r"^#define (MBEDTLS_[A-Z0-9_]+)", text, re.M))


def read(path: str) -> str:
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def check_version(problems: list[str]) -> None:
    m = re.search(r'MBEDTLS_VERSION_STRING\s+"([^"]+)"', read(BUILD_INFO))
    if not m:
        problems.append("build_info.h has no MBEDTLS_VERSION_STRING")
        return
    compiled = m.group(1)
    if compiled != PINNED:
        problems.append(
            f"build_info.h says {compiled} but the pin is {PINNED}")

    m = re.search(r"^Release:\s*v([0-9.]+)", read(PROVENANCE), re.M)
    if not m:
        problems.append("PROVENANCE.md has no 'Release: vX.Y.Z' line")
        return
    documented = m.group(1)
    if documented != PINNED:
        problems.append(
            f"PROVENANCE.md says {documented} but the pin is {PINNED}")
    if not PINNED.startswith(LTS_LINE + "."):
        problems.append(f"the pin {PINNED} left the {LTS_LINE} LTS line")


def check_config(problems: list[str]) -> None:
    have = defines(read(CONFIG))
    for feat in REQUIRED_FEATURES:
        if feat not in have:
            problems.append(f"{feat} is off; the TLS client needs it")
    for feat in FORBIDDEN_FEATURES:
        if feat in have:
            problems.append(
                f"{feat} is on; the trimmed client-only config is not applied")


def check_units(problems: list[str]) -> None:
    lib = os.path.join(MBEDTLS, "library")
    on_disk = {n for n in os.listdir(lib) if n.endswith(".c")}
    compiled = set(re.findall(
        r"third_party/mbedtls/library/([a-z0-9_]+\.c)", read(SOURCES_MK)))
    # The host tests compile two more server units with the test config.
    test_extra = set(re.findall(
        r"third_party/mbedtls/library/([a-z0-9_]+\.c)", read(MAKEFILE)))
    test_extra -= compiled

    missing = compiled - on_disk
    for name in sorted(missing):
        problems.append(f"{name} is compiled but not vendored")

    unlisted = on_disk - compiled - test_extra - EXCLUDED_UNITS
    for name in sorted(unlisted):
        problems.append(
            f"{name} is vendored but neither compiled nor excluded "
            f"(add it to sources.mk or to EXCLUDED_UNITS)")

    for name in sorted(test_extra & EXCLUDED_UNITS):
        problems.append(
            f"{name} is both a test-only unit and excluded")

    # The exclusion set must match what PROVENANCE.md documents, or a reader
    # cannot tell why a unit is on disk and not built.
    doc = read(PROVENANCE)
    for name in sorted(EXCLUDED_UNITS):
        stem = name[:-2]
        if stem not in doc:
            problems.append(f"{name} is excluded but PROVENANCE.md omits it")


def check_upstream() -> None:
    url = ("https://api.github.com/repos/Mbed-TLS/mbedtls/releases"
           "?per_page=30")
    try:
        req = urllib.request.Request(
            url, headers={"User-Agent": "berry-mbedtls-pin"})
        with urllib.request.urlopen(req, timeout=15) as resp:
            releases = json.load(resp)
    except Exception as exc:  # network is best-effort
        print(f"check-mbedtls: upstream check skipped ({exc})")
        return
    line = re.compile(r"^mbedtls-(" + re.escape(LTS_LINE) + r"\.\d+)$")
    tags = [m.group(1) for r in releases
            if (m := line.match(r.get("tag_name", "")))]
    if not tags:
        print("check-mbedtls: upstream check: no releases found for the line")
        return
    newest = max(tags, key=lambda v: tuple(int(x) for x in v.split(".")))
    if newest != PINNED:
        print(f"check-mbedtls: ADVISORY the {LTS_LINE} line has {newest}, "
              f"this tree pins {PINNED}; review its advisories")
    else:
        print(f"check-mbedtls: {PINNED} is the newest {LTS_LINE} release")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--upstream", action="store_true",
                    help="ask GitHub for the newest release in the line")
    args = ap.parse_args()

    if not os.path.isdir(MBEDTLS):
        sys.stderr.write("check-mbedtls: third_party/mbedtls is missing\n")
        return 2

    problems: list[str] = []
    check_version(problems)
    check_config(problems)
    check_units(problems)

    if problems:
        for p in problems:
            sys.stderr.write(f"check-mbedtls: {p}\n")
        return 1

    print(f"== mbedTLS {PINNED} pin, config and unit set agree ==")
    if args.upstream:
        check_upstream()
    return 0


if __name__ == "__main__":
    sys.exit(main())
