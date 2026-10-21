#!/usr/bin/env python3
"""BerryAPI manifest signer.

Produces and checks the Ed25519 signatures the loader requires. This is the
publisher-side half of the trust split: it holds the secret key, the device
holds only the public key, so a compromised client can verify a script but
cannot mint one.

    # one-time: make a signing key
    python3 tools/berry-sign.py keygen --out berry-signing.key

    # sign a script directory (rewrites its manifest.json in place)
    python3 tools/berry-sign.py sign --key berry-signing.key --dir scripts/cps

    # check a script directory against a public key
    python3 tools/berry-sign.py verify --pub <hex> --dir scripts/cps

The canonical payload is defined by ScriptManifest::CanonicalPayload in
include/berry/manifest.h and must stay byte-identical to it:

    berry-manifest-v1\n
    name=<name>\n
    version=<version>\n
    entry=<entry>\n
    author=<author>\n
    capabilities=<sorted, comma-joined canonical capability list>\n
    source_sha256=<hex sha256 of the entry file>\n

Ed25519 is implemented here in pure Python (RFC 8032 reference construction)
so the tool has no dependencies and can run on a bare build machine. It is
cross-checked against the vendored C implementation by tests/test_loader.cpp.
"""

import argparse
import hashlib
import json
import os
import sys

# ---------------------------------------------------------------------------
# Ed25519 (RFC 8032), pure Python. Only sign/verify/keypair are needed.
# ---------------------------------------------------------------------------

_P = 2 ** 255 - 19
_L = 2 ** 252 + 27742317777372353535851937790883648493
_D = (-121665 * pow(121666, _P - 2, _P)) % _P
_I = pow(2, (_P - 1) // 4, _P)


def _sha512(data):
    return hashlib.sha512(data).digest()


def _inv(x):
    return pow(x, _P - 2, _P)


def _x_recover(y):
    xx = (y * y - 1) * _inv(_D * y * y + 1)
    x = pow(xx, (_P + 3) // 8, _P)
    if (x * x - xx) % _P != 0:
        x = (x * _I) % _P
    if x % 2 != 0:
        x = _P - x
    return x


_BY = (4 * _inv(5)) % _P
_BX = _x_recover(_BY)
_B = (_BX % _P, _BY % _P, 1, (_BX * _BY) % _P)


def _edwards_add(p, q):
    x1, y1, z1, t1 = p
    x2, y2, z2, t2 = q
    a = ((y1 - x1) * (y2 - x2)) % _P
    b = ((y1 + x1) * (y2 + x2)) % _P
    c = (2 * t1 * t2 * _D) % _P
    dd = (2 * z1 * z2) % _P
    e = b - a
    f = dd - c
    g = dd + c
    h = b + a
    return ((e * f) % _P, (g * h) % _P, (f * g) % _P, (e * h) % _P)


def _scalar_mul(p, e):
    if e == 0:
        return (0, 1, 1, 0)
    q = _scalar_mul(p, e // 2)
    q = _edwards_add(q, q)
    if e & 1:
        q = _edwards_add(q, p)
    return q


def _encode_point(p):
    x, y, z, _t = p
    zi = _inv(z)
    x = (x * zi) % _P
    y = (y * zi) % _P
    bits = [(y >> i) & 1 for i in range(255)] + [x & 1]
    return bytes(sum(bits[i * 8 + j] << j for j in range(8)) for i in range(32))


def _decode_point(s):
    if len(s) != 32:
        return None
    y = int.from_bytes(s, "little") & ((1 << 255) - 1)
    if y >= _P:
        return None
    x = _x_recover(y)
    if (x & 1) != (s[31] >> 7):
        x = _P - x
    # Twisted Edwards: -x^2 + y^2 = 1 + d*x^2*y^2.
    if (-x * x + y * y - 1 - _D * x * x * y * y) % _P != 0:
        return None
    return (x, y, 1, (x * y) % _P)


def _clamp(h):
    a = bytearray(h)
    a[0] &= 248
    a[31] &= 127
    a[31] |= 64
    return int.from_bytes(bytes(a), "little")


def keypair_from_seed(seed):
    """seed (32 bytes) -> (public_key 32 bytes, secret_key 64 bytes)."""
    if len(seed) != 32:
        raise ValueError("seed must be 32 bytes")
    h = _sha512(seed)
    a = _clamp(h[:32])
    pub = _encode_point(_scalar_mul(_B, a))
    return pub, seed + pub


def sign(message, secret_key):
    if len(secret_key) != 64:
        raise ValueError("secret key must be 64 bytes")
    seed, pub = secret_key[:32], secret_key[32:]
    h = _sha512(seed)
    a = _clamp(h[:32])
    prefix = h[32:]
    r = int.from_bytes(_sha512(prefix + message), "little") % _L
    big_r = _encode_point(_scalar_mul(_B, r))
    k = int.from_bytes(_sha512(big_r + pub + message), "little") % _L
    s = (r + k * a) % _L
    return big_r + s.to_bytes(32, "little")


def verify(signature, message, public_key):
    if len(signature) != 64 or len(public_key) != 32:
        return False
    big_r, s = signature[:32], int.from_bytes(signature[32:], "little")
    if s >= _L:
        return False
    a = _decode_point(public_key)
    r = _decode_point(big_r)
    if a is None or r is None:
        return False
    k = int.from_bytes(_sha512(big_r + public_key + message), "little") % _L
    return _encode_point(_scalar_mul(_B, s)) == _encode_point(
        _edwards_add(r, _scalar_mul(a, k))
    )


# ---------------------------------------------------------------------------
# Manifest payload (must match ScriptManifest::CanonicalPayload).
# ---------------------------------------------------------------------------

# Mirrors the kind names in CapKindName (core/script/manifest.cpp). A capability
# is canonicalized as "kind" when unscoped and "kind:scope" otherwise, with the
# scope kept exactly as written -- no normalization, matching
# Capability::Canonical, because the payload must be byte-identical on both
# sides or the signature will not verify.
CAP_KINDS = frozenset([
    "network", "render", "fileio", "world", "combat", "input", "hud", "perf",
    "events", "audio", "cosmetic",
])


def canonical_capability(text):
    """Canonicalizes one capability string the way Capability::Canonical does."""
    kind, sep, scope = text.partition(":")
    if kind not in CAP_KINDS:
        raise ValueError("unknown capability kind: %s" % text)
    if not sep:
        return kind
    if not scope:
        raise ValueError("capability has an empty scope: %s" % text)
    return kind + ":" + scope


def canonical_payload(manifest):
    caps = manifest.get("capabilities", [])
    if not isinstance(caps, list):
        raise ValueError("capabilities must be a list")
    canonical = sorted({canonical_capability(c) for c in caps})
    lines = [
        "berry-manifest-v1",
        "name=" + manifest.get("name", ""),
        "version=" + manifest.get("version", ""),
        "entry=" + manifest.get("entry", ""),
        "author=" + manifest.get("author", ""),
        "capabilities=" + ",".join(canonical),
        "source_sha256=" + manifest.get("source_sha256", ""),
    ]
    return ("\n".join(lines) + "\n").encode("utf-8")


def payload_hash(manifest):
    return hashlib.sha256(canonical_payload(manifest)).hexdigest()


# ---------------------------------------------------------------------------
# Commands
# ---------------------------------------------------------------------------


def _read_json(path):
    with open(path, "rb") as f:
        return json.loads(f.read().decode("utf-8"))


def _write_json(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=2, sort_keys=True)
        f.write("\n")


def _load_seed(path):
    with open(path, "r", encoding="utf-8") as f:
        text = f.read().strip()
    # Accept either a bare seed or a "seed:pub" pair file.
    seed_hex = text.split()[0].split(":")[0]
    seed = bytes.fromhex(seed_hex)
    if len(seed) != 32:
        raise SystemExit("signing key file must hold a 32-byte hex seed")
    return seed


def cmd_keygen(args):
    seed = os.urandom(32)
    pub, sec = keypair_from_seed(seed)
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
    seed = _load_seed(args.key)
    pub, sec = keypair_from_seed(seed)
    manifest_path = os.path.join(args.dir, "manifest.json")
    manifest = _read_json(manifest_path)

    entry_path = os.path.join(args.dir, manifest["entry"])
    with open(entry_path, "rb") as f:
        source = f.read()
    manifest["source_sha256"] = hashlib.sha256(source).hexdigest()

    digest = payload_hash(manifest)
    manifest["payload_hash"] = digest
    manifest["signature_algo"] = "ed25519"
    manifest["signature"] = sign(bytes.fromhex(digest), sec).hex()

    if args.check:
        if not verify(bytes.fromhex(manifest["signature"]),
                      bytes.fromhex(digest), pub):
            raise SystemExit("self-check failed: signature does not verify")
    _write_json(manifest_path, manifest)
    print("signed %s" % manifest_path)
    print("  source_sha256: %s" % manifest["source_sha256"])
    print("  payload_hash:  %s" % manifest["payload_hash"])
    print("  signature:     %s" % manifest["signature"])
    print("  public_key:    %s" % pub.hex())
    return 0


def cmd_verify(args):
    manifest = _read_json(os.path.join(args.dir, "manifest.json"))
    entry_path = os.path.join(args.dir, manifest["entry"])
    with open(entry_path, "rb") as f:
        source = f.read()

    actual = hashlib.sha256(source).hexdigest()
    if actual != manifest.get("source_sha256"):
        print("FAIL source_sha256: manifest says %s, file is %s"
              % (manifest.get("source_sha256"), actual))
        return 1

    digest = payload_hash(manifest)
    if manifest.get("payload_hash") and manifest["payload_hash"] != digest:
        print("FAIL payload_hash: manifest says %s, recomputed %s"
              % (manifest["payload_hash"], digest))
        return 1
    if manifest.get("signature_algo") != "ed25519":
        print("FAIL signature_algo is %r" % manifest.get("signature_algo"))
        return 1
    try:
        sig = bytes.fromhex(manifest.get("signature", ""))
        pub = bytes.fromhex(args.pub)
    except ValueError:
        print("FAIL signature or public key is not hex")
        return 1
    if not verify(sig, bytes.fromhex(digest), pub):
        print("FAIL signature does not verify")
        return 1
    print("OK %s verifies against the given public key" % args.dir)
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    kg = sub.add_parser("keygen", help="generate a signing key")
    kg.add_argument("--out", help="write the 32-byte seed here (chmod 600)")
    kg.set_defaults(func=cmd_keygen)

    sg = sub.add_parser("sign", help="sign a script directory in place")
    sg.add_argument("--key", required=True, help="signing key file (seed)")
    sg.add_argument("--dir", required=True, help="script directory")
    sg.add_argument("--no-check", dest="check", action="store_false",
                    help="skip the self-check after signing")
    sg.set_defaults(func=cmd_sign, check=True)

    vf = sub.add_parser("verify", help="verify a script directory")
    vf.add_argument("--pub", required=True, help="public key (64 hex chars)")
    vf.add_argument("--dir", required=True, help="script directory")
    vf.set_defaults(func=cmd_verify)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
