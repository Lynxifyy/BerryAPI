#!/usr/bin/env python3
"""Generate a CycloneDX SBOM for BerryAPI (improvement 100).

Reads the vendored dependency versions (mbedTLS from the pinned build_info.h,
and the licences/versions recorded in third_party/*/PROVENANCE.md where
present) and emits a minimal, valid CycloneDX 1.5 JSON document on stdout.

No third-party Python deps: a build machine only needs python3.
"""

from __future__ import annotations

import json
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

VERSION_RE = re.compile(r"#define\s+MBEDTLS_VERSION_STRING\s+\"([^\"]+)\"")


def read(path: str) -> str:
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            return fh.read()
    except OSError:
        return ""


def mbedtls_version() -> str:
    txt = read(os.path.join(ROOT, "third_party", "mbedtls", "include",
                            "mbedtls", "build_info.h"))
    m = VERSION_RE.search(txt)
    return m.group(1) if m else "unknown"


def provenance_version(name: str) -> str:
    txt = read(os.path.join(ROOT, "third_party", name, "PROVENANCE.md"))
    m = re.search(r"(?:version|release)[^\n]*?([0-9]+\.[0-9]+(?:\.[0-9]+)?)",
                  txt, re.IGNORECASE)
    return m.group(1) if m else "vendored"


def main() -> int:
    lib_version = read(os.path.join(ROOT, "include", "berry", "version.h"))
    m = re.search(r'BERRY_VERSION_STRING\s+"([^"]+)"', lib_version)
    berry_version = m.group(1) if m else "0.0.0"

    components = [
        {"type": "library", "name": "mbedTLS", "version": mbedtls_version(),
         "licenses": [{"license": {"id": "Apache-2.0"}}],
         "purl": "pkg:generic/mbedtls@%s" % mbedtls_version()},
        {"type": "library", "name": "Luau", "version": provenance_version("luau"),
         "licenses": [{"license": {"id": "MIT"}}],
         "purl": "pkg:generic/luau@%s" % provenance_version("luau")},
        {"type": "library", "name": "Dobby", "version": provenance_version("Dobby"),
         "licenses": [{"license": {"id": "Apache-2.0"}}],
         "purl": "pkg:generic/dobby@%s" % provenance_version("Dobby")},
        {"type": "library", "name": "lz4", "version": provenance_version("lz4"),
         "licenses": [{"license": {"id": "BSD-2-Clause"}}],
         "purl": "pkg:generic/lz4@%s" % provenance_version("lz4")},
        {"type": "library", "name": "ImGui", "version": provenance_version("ImGui"),
         "licenses": [{"license": {"id": "MIT"}}],
         "purl": "pkg:generic/imgui@%s" % provenance_version("ImGui")},
        {"type": "library", "name": "ed25519", "version": provenance_version("ed25519"),
         "licenses": [{"license": {"id": "Zlib"}}],
         "purl": "pkg:generic/ed25519@%s" % provenance_version("ed25519")},
    ]

    doc = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "version": 1,
        "metadata": {
            "component": {
                "type": "library",
                "name": "BerryAPI",
                "version": berry_version,
                "licenses": [{"license": {"id": "MIT"}}],
            }
        },
        "components": components,
    }
    json.dump(doc, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
