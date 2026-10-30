#!/usr/bin/env python3
"""Keeps tools/berry-sigpack-sign.py and the C++ pack verifier in step.

tests/test_sigpack.cpp pins one signed pack that tools/berry-sigpack-sign.py
produced, so the two canonical payloads are proven byte-identical at test time.
That pin only stays meaningful while it matches what the signer actually emits,
so this script recomputes the vector and fails when the constants in the test
have drifted from the signer's output.

Run by `make test`.
"""

import importlib.util
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIGNER = os.path.join(ROOT, "tools", "berry-sigpack-sign.py")
TEST = os.path.join(ROOT, "tests", "test_sigpack.cpp")

# The same RFC 8032 §7.1 seed the manifest and loader tests use.
SEED_HEX = "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60"
PACK = {"name": "interop-pack", "build": "0xBUILD-INTEROP",
        "sequence": 3, "expires": 1893456000}


def load_signer():
    spec = importlib.util.spec_from_file_location("berry_sigpack_sign", SIGNER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def expected():
    signer = load_signer()
    ed = signer.load_ed25519()
    seed = bytes.fromhex(SEED_HEX)
    pub, sec = ed.keypair_from_seed(seed)
    payload = signer.canonical_payload(PACK)
    signature = ed.sign(payload, sec).hex()
    return pub.hex(), signature


def assert_in_test(name, value, text):
    # The test wraps long hex strings across adjacent C++ literals, so compare
    # against the concatenated text with all whitespace and quotes removed.
    flat = re.sub(r'[\s"]', "", text)
    if value not in flat:
        print("check-sigpack-signer: %s has drifted from "
              "tools/berry-sigpack-sign.py" % name)
        print("  signer says: %s" % value)
        print("  update tests/test_sigpack.cpp")
        return False
    return True


def main():
    pub, signature = expected()

    with open(TEST, "r", encoding="utf-8") as f:
        text = f.read()

    ok = True
    ok &= assert_in_test("public key", pub, text)
    ok &= assert_in_test("signature", signature, text)

    # The signature must also verify, so a constant that happens to appear in
    # the file for another reason cannot pass the check above.
    signer = load_signer()
    ed = signer.load_ed25519()
    if not ed.verify(bytes.fromhex(signature), signer.canonical_payload(PACK),
                     bytes.fromhex(pub)):
        print("check-sigpack-signer: the signer's own output does not verify")
        ok = False

    if ok:
        print("== sigpack signer interop vector matches ==")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
