#!/usr/bin/env python3
"""Minimal OIDC discovery mock for testing evgrpc_server config loading.

Serves:
  GET /.well-known/openid-configuration -> {"issuer": "...", "jwks_uri": "..."}
  GET /jwks.json                        -> {"keys": [{RSA JWK}]}

Generates a fresh RSA-2048 key pair on startup. NOT for production use.

Usage:
  ./scripts/mock_oidc.py [port]    # default port 8080
"""

import base64
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives.asymmetric import rsa


def b64url(data: bytes) -> str:
    """Base64url without padding (JWK spec)."""
    return base64.urlsafe_b64encode(data).rstrip(b"=").decode("ascii")


def jwk_from_public(public_key) -> dict:
    numbers = public_key.public_numbers()
    return {
        "kty": "RSA",
        "kid": "mock-oidc-key-1",
        "use": "sig",
        "alg": "RS256",
        "n": b64url(numbers.n.to_bytes((numbers.n.bit_length() + 7) // 8, "big")),
        "e": b64url(numbers.e.to_bytes((numbers.e.bit_length() + 7) // 8, "big")),
    }


class Handler(BaseHTTPRequestHandler):
    # Silence the default access-log spam — we only care about real errors.
    def log_message(self, fmt, *args):
        if " 5" in fmt or " 4" in fmt:  # 4xx/5xx only
            sys.stderr.write("[mock-oidc] " + (fmt % args) + "\n")

    def _json(self, body: dict, status: int = 200):
        data = json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):  # noqa: N802 — http.server protocol
        if self.path == "/.well-known/openid-configuration":
            self._json({
                "issuer": ISSUER,
                "jwks_uri": JWKS_URI,
            })
        elif self.path == "/jwks.json":
            self._json({"keys": [JWK]})
        else:
            self._json({"error": f"unknown path: {self.path}"}, status=404)


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080

    # Generate fresh RSA-2048 key per run.
    private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    public_key = private_key.public_key()

    # Make them module-level so Handler can access them.
    global ISSUER, JWKS_URI, JWK
    ISSUER = f"http://localhost:{port}"
    JWKS_URI = f"http://localhost:{port}/jwks.json"
    JWK = jwk_from_public(public_key)

    # Print the public key PEM to stdout — useful for JwtValidator
    # debugging / manual JWT crafting during local dev.
    pem = public_key.public_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PublicFormat.SubjectPublicKeyInfo,
    )
    print(f"[mock-oidc] issuer = {ISSUER}")
    print(f"[mock-oidc] jwks_uri = {JWKS_URI}")
    print("[mock-oidc] public key (PEM):")
    print(pem.decode())

    server = HTTPServer(("0.0.0.0", port), Handler)
    print(f"[mock-oidc] listening on 0.0.0.0:{port}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("[mock-oidc] shutting down")
        server.server_close()


if __name__ == "__main__":
    main()