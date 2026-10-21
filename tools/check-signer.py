#!/usr/bin/env python3
"""Keeps tools/berry-sign.py and the C++ verifier in step.

tests/test_loader.cpp pins one manifest that tools/berry-sign.py produced, so
the two canonical payloads are proven byte-identical at test time. That pin
only stays meaningful while it matches what the signer actually emits, so this
script recomputes the vector and fails when the constants in the test have
drifted from the signer's output.

Run by `make idl-check` (and so by `make test`).
"""

import hashlib
import importlib.util
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIGNER = os.path.join(ROOT, "tools", "berry-sign.py")
TEST = os.path.join(ROOT, "tests", "test_loader.cpp")

# The same RFC 8032 §7.1 seed the manifest and loader tests use.
SEED_HEX = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
SOURCE = "func on_load\n  push 1\n  ret 0\nend\n"
MANIFEST = {
    "name": "interop",
    "version": "1.0.0",
    "entry": "main.luau",
    "author": "Lynxifyy",
    "capabilities": ["render", "hud", "input", "network:api.example.com"],
}


def load_signer():
    spec = importlib.util.spec_from_file_location("berry_sign", SIGNER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expected():
    signer = load_signer()
    seed = bytes.fromhex(SEED_HEX)
    pub, sec = signer.keypair_from_seed(seed)

    manifest = dict(MANIFEST)
    manifest["source_sha256"] = hashlib.sha256(SOURCE.encode("utf-8")).hexdigest()
    digest = signer.payload_hash(manifest)
    signature = signer.sign(bytes.fromhex(digest), sec).hex()
    return pub.hex(), manifest["source_sha256"], digest, signature


def assert_in_test(name, value, text):
    # The test wraps long hex strings across adjacent C++ literals, so compare
    # against the concatenated text with all whitespace and quotes removed.
    flat = re.sub(r'[\s"]', "", text)
    if value not in flat:
        print("check-signer: %s has drifted from tools/berry-sign.py" % name)
        print("  signer says: %s" % value)
        print("  re-run tools/berry-sign.py sign and update tests/test_loader.cpp")
        return False
    return True


def main():
    pub, source_hash, digest, signature = expected()

    with open(TEST, "r", encoding="utf-8") as f:
        text = f.read()

    ok = True
    ok &= assert_in_test("public key", pub, text)
    ok &= assert_in_test("source_sha256", source_hash, text)
    ok &= assert_in_test("payload_hash", digest, text)
    ok &= assert_in_test("signature", signature, text)

    # The signature must also verify, so a constant that happens to appear in
    # the file for another reason cannot pass the check above.
    signer = load_signer()
    if not signer.verify(bytes.fromhex(signature), bytes.fromhex(digest),
                         bytes.fromhex(pub)):
        print("check-signer: the signer's own output does not verify")
        ok = False

    if ok:
        print("== signer interop vector matches ==")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
