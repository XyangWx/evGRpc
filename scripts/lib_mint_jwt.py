#!/usr/bin/env python3
"""Mint an RS256 JWT from a PEM private key file.

Usage:
    python3 lib_mint_jwt.py <pem-path> <iss> <aud> <sub> <ttl-seconds>

Reads <pem-path>, signs an RS256 JWT with the given iss/aud/sub claims
and expiry = now + ttl-seconds, prints the JWT to stdout. kid is read
from $KID env var (smoke.sh sets it from the JWK it generated).

The matching JWK must be served at the same kid by the JWKS endpoint the
server validates against — this is what OAUTH_JWKS_URL points at.

Why a separate file: keeps the bash smoke script free of inline Python
and makes the JWT format easy to inspect + tweak.
"""

import os
import sys
import time

import jwt  # PyJWT


def main() -> int:
    if len(sys.argv) != 6:
        sys.stderr.write(
            "usage: lib_mint_jwt.py <pem-path> <iss> <aud> <sub> <ttl-seconds>\n"
        )
        return 2

    pem_path, iss, aud, sub, ttl_s = sys.argv[1:6]
    kid = os.environ.get("KID", "smoke-key-1")

    with open(pem_path, "rb") as f:
        priv_pem = f.read()

    now = int(time.time())
    ttl = int(ttl_s)

    claims = {
        "iss": iss,
        "aud": aud,
        "sub": sub,
        "iat": now,
        "exp": now + ttl,
    }

    token = jwt.encode(
        claims,
        priv_pem,
        algorithm="RS256",
        headers={"kid": kid},
    )
    sys.stdout.write(token)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())