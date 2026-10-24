#!/usr/bin/env python3
"""BerryAPI signature-pack signer.

Signs an OTA signature pack so a device can verify it with an Ed25519 public
key it was provisioned with (the same trust split the script manifest uses:
the publisher holds the seed, the device holds only the public key).

A pack's signature covers a small canonical payload that binds the pack's name,
the build fingerprint it was written for, a monotonic anti-rollback `sequence`
and an optional `expires` (Unix seconds). The library verifies that signature
BEFORE it parses a single signature row or layout, so a tampered, unsigned,
expired or replayed pack never reaches the entry parser.

    # one-time: reuse (or make) a signing key
    python3 tools/berry-sigpack-sign.py keygen --out berry-pack.key

    # sign a pack in place (adds/updates `sequence`, `expires` and `signature`)
    python3 tools/berry-sigpack-sign.py sign --key berry-pack.key \\
        --file packs/1.21.sigpack.json --sequence 7 --expires 1893456000

    # check a signed pack against a public key
    python3 tools/berry-sigpack-sign.py verify --pub <hex> \\
        --file packs/1.21.sigpack.json

The canonical payload is defined by SignaturePack::CanonicalPayload in
include/berry/sigpack.h and must stay byte-identical to it:

    berry-sigpack-v1\\n
    format=1\\n
    name=<name>\\n
    build=<normalized build>\\n
    sequence=<n>\\n
    expires=<n>\\n

Ed25519 is reused from tools/berry-sign.py (pure Python, RFC 8032), so this tool
has no dependencies. tests/test_sigpack.cpp pins an interop vector and
tools/check-sigpack-signer.py keeps the two in step.
"""

import argparse
import importlib.util
import json
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SIGNER = os.path.join(ROOT, "tools", "berry-sign.py")

FORMAT_VERSION = 1


def load_ed25519():
    """Loads the pure-Python Ed25519 primitives from tools/berry-sign.py."""
    spec = importlib.util.spec_from_file_location("berry_sign", SIGNER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def normalize_build(build):
    """Mirrors SignaturePack::NormalizeBuild: strip one leading 0x, lowercase."""
    s = build
    if len(s) > 2 and s[0] == "0" and s[1] in ("x", "X"):
        s = s[2:]
    return s.lower()


def canonical_payload(pack):
    """Mirrors SignaturePack::CanonicalPayload byte for byte."""
    lines = [
        "berry-sigpack-v1",
        "format=%d" % FORMAT_VERSION,
        "name=" + pack.get("name", ""),
        "build=" + normalize_build(pack.get("build", "")),
        "sequence=%d" % int(pack.get("sequence", 0)),
        "expires=%d" % int(pack.get("expires", 0)),
    ]
    return ("\n".join(lines) + "\n").encode("utf-8")


def _load_seed(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read().strip()
    seed_hex = text.split()[0].split(":")[0]
    seed = bytes.fromhex(seed_hex)
    if len(seed) != 32:
        raise SystemExit("signing key file must hold a 32-byte hex seed")
    return seed


def _read_json(path):
    with open(path, "rb") as f:
        return json.loads(f.read().decode("utf-8"))


def _write_json(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, sort_keys=True)
        f.write("\n")


def cmd_keygen(args):
    ed = load_ed25519()
    seed = os.urandom(32)
    pub, sec = ed.keypair_from_seed(seed)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(seed.hex() + "\n")
        os.chmod(args.out, 0o600)
        print("wrote signing key to %s (keep it secret)" % args.out)
    print("seed:       " + seed.hex())
    print("public_key: " + pub.hex())
    print("secret_key: " + sec.hex())
    print()
    print("Provision the device with the public key only.")
    return 0


def cmd_sign(args):
    ed = load_ed25519()
    seed = _load_seed(args.key)
    pub, sec = ed.keypair_from_seed(seed)

    pack = _read_json(args.file)
    if "name" not in pack or "build" not in pack:
        raise SystemExit("pack must carry 'name' and 'build' before signing")
    pack["format"] = FORMAT_VERSION
    pack["sequence"] = int(args.sequence)
    pack["expires"] = int(args.expires)

    payload = canonical_payload(pack)
    signature = ed.sign(payload, sec)
    pack["signature"] = {"algo": "ed25519", "value": signature.hex()}

    if args.check:
        if not ed.verify(signature, payload, pub):
            raise SystemExit("self-check failed: signature does not verify")
    _write_json(args.file, pack)
    print("signed %s" % args.file)
    print("  name:      %s" % pack["name"])
    print("  build:     %s" % pack["build"])
    print("  sequence:  %d" % pack["sequence"])
    print("  expires:   %d" % pack["expires"])
    print("  signature: %s" % pack["signature"]["value"])
    print("  public_key: %s" % pub.hex())
    return 0


def cmd_verify(args):
    ed = load_ed25519()
    pack = _read_json(args.file)
    sig_block = pack.get("signature")
    if not isinstance(sig_block, dict) or sig_block.get("algo") != "ed25519":
        print("FAIL pack has no ed25519 signature block")
        return 1
    try:
        sig = bytes.fromhex(sig_block.get("value", ""))
        pub = bytes.fromhex(args.pub)
    except ValueError:
        print("FAIL signature or public key is not hex")
        return 1
    if not ed.verify(sig, canonical_payload(pack), pub):
        print("FAIL signature does not verify")
        return 1
    print("OK %s verifies against the given public key" % args.file)
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    kg = sub.add_parser("keygen", help="generate a signing key")
    kg.add_argument("--out", help="write the 32-byte seed here (chmod 600)")
    kg.set_defaults(func=cmd_keygen)

    sg = sub.add_parser("sign", help="sign a pack file in place")
    sg.add_argument("--key", required=True, help="signing key file (seed)")
    sg.add_argument("--file", required=True, help="pack JSON file")
    sg.add_argument("--sequence", required=True, type=int,
                    help="monotonic anti-rollback sequence")
    sg.add_argument("--expires", type=int, default=0,
                    help="optional Unix-seconds expiry (0 = none)")
    sg.add_argument("--no-check", dest="check", action="store_false",
                    help="skip the self-check after signing")
    sg.set_defaults(func=cmd_sign, check=True)

    vf = sub.add_parser("verify", help="verify a signed pack")
    vf.add_argument("--pub", required=True, help="public key (64 hex chars)")
    vf.add_argument("--file", required=True, help="pack JSON file")
    vf.set_defaults(func=cmd_verify)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
