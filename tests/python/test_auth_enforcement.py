"""Auth enforcement: 3 tests covering missing / malformed / forged tokens.

These tests use a bare insecure_channel (no Bearer interceptor) so we can
inject our own metadata (or none at all).
"""

from __future__ import annotations

import time
import uuid

import grpc
import jwt
import pytest
from cryptography.hazmat.primitives.asymmetric import rsa
from cryptography.hazmat.primitives import serialization

from tests.python.gen.evgrpc import weather_pb2 as pb
from tests.python.gen.evgrpc import weather_pb2_grpc as rpc


def _bare_channel():
    chan = grpc.insecure_channel("localhost:80")
    grpc.channel_ready_future(chan).result(timeout=5)
    return chan


def test_no_token_returns_unauthenticated():
    """No Authorization header → UNAUTHENTICATED."""
    chan = _bare_channel()
    stub = rpc.WeatherServiceStub(chan)
    with pytest.raises(grpc.RpcError) as exc:
        stub.SearchWeather(pb.SearchWeatherRequest())
    assert exc.value.code() == grpc.StatusCode.UNAUTHENTICATED


def test_malformed_token_returns_unauthenticated():
    """Malformed Bearer token → UNAUTHENTICATED."""
    chan = _bare_channel()
    stub = rpc.WeatherServiceStub(chan)
    with pytest.raises(grpc.RpcError) as exc:
        stub.SearchWeather(
            pb.SearchWeatherRequest(),
            metadata=(("authorization", "Bearer not.a.real.jwt"),),
        )
    assert exc.value.code() == grpc.StatusCode.UNAUTHENTICATED


def test_forged_token_returns_unauthenticated():
    """Sign a structurally-valid JWT with throwaway RSA key + real iss/aud.

    evgrpc's JWT validator checks signature against the IdP's JWKS, which
    does not contain our throwaway key → UNAUTHENTICATED.
    """
    private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    private_pem = private_key.private_bytes(
        encoding=serialization.Encoding.PEM,
        format=serialization.PrivateFormat.PKCS8,
        encryption_algorithm=serialization.NoEncryption(),
    )

    now = int(time.time())
    claims = {
        "iss": "https://auth-test.mksword.com/",
        "aud": "https://www.mksword.com/grpc/ev",
        "sub": "forged-test",
        "iat": now,
        "exp": now + 3600,
    }
    forged_token = jwt.encode(
        claims, private_pem, algorithm="RS256",
        headers={"kid": "forged-key"},
    )

    chan = _bare_channel()
    stub = rpc.WeatherServiceStub(chan)
    with pytest.raises(grpc.RpcError) as exc:
        stub.SearchWeather(
            pb.SearchWeatherRequest(),
            metadata=(("authorization", f"Bearer {forged_token}"),),
        )
    assert exc.value.code() == grpc.StatusCode.UNAUTHENTICATED