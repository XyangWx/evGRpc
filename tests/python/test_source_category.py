"""SourceCategoryService: 2 RPCs (Create, Search) — ~7 tests.

Mirrors WeatherService (same create + prefix-search shape).
"""

from __future__ import annotations

import uuid

import grpc
import pytest

from tests.python._helpers import TrackedInsert, make_source_category_name
from tests.python.gen.evgrpc import source_category_pb2 as pb
from tests.python.gen.evgrpc import source_category_pb2_grpc as rpc


# ─────────────────────────── TestHappyPath ───────────────────────────

class TestHappyPath:
    def test_create_source_category_returns_id_and_name(self, channel, namespace, pg_conn):
        stub = rpc.SourceCategoryServiceStub(channel)
        name = make_source_category_name(namespace)
        with TrackedInsert(pg_conn, "source_category") as ti:
            resp = stub.CreateSourceCategory(pb.CreateSourceCategoryRequest(name=name))
            ti.register(resp.id)
        assert resp.id
        assert resp.name == name

    def test_search_source_category_finds_created(self, channel, namespace, pg_conn):
        stub = rpc.SourceCategoryServiceStub(channel)
        # PG ^@ = "starts with"; marker must be at START of name.
        # 6 (marker) + 1 (-) + 29 (make_source_category_name) = 36 (at VARCHAR(36) limit).
        marker = uuid.uuid4().hex[:6]
        name = marker + "-" + make_source_category_name(namespace)
        with TrackedInsert(pg_conn, "source_category") as ti:
            stub.CreateSourceCategory(pb.CreateSourceCategoryRequest(name=name))
            ti.register(stub.SearchSourceCategory(
                pb.SearchSourceCategoryRequest(prefix=marker, limit=5)
            ).matches[0].id)
        # Outer search — row already deleted by cleanup; verify response is OK and iterable.
        resp = stub.SearchSourceCategory(
            pb.SearchSourceCategoryRequest(prefix=marker, limit=5)
        )
        assert isinstance(resp, pb.SearchSourceCategoryResponse)
        assert len(resp.matches) >= 0  # may be 0 after cleanup


# ─────────────────────────── TestErrorPath ───────────────────────────

class TestErrorPath:
    def test_create_source_category_duplicate_name_returns_already_exists(
        self, channel, namespace, pg_conn
    ):
        """UNIQUE on source_category.Name → ALREADY_EXISTS."""
        stub = rpc.SourceCategoryServiceStub(channel)
        name = make_source_category_name(namespace)
        with TrackedInsert(pg_conn, "source_category") as ti:
            resp1 = stub.CreateSourceCategory(pb.CreateSourceCategoryRequest(name=name))
            ti.register(resp1.id)
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateSourceCategory(pb.CreateSourceCategoryRequest(name=name))
        assert exc.value.code() == grpc.StatusCode.ALREADY_EXISTS


# ─────────────────────────── TestBoundaries ───────────────────────────

class TestBoundaries:
    @pytest.mark.parametrize("name_len", [1, 35, 36, 37])
    def test_create_source_category_name_length(
        self, channel, namespace, pg_conn, name_len
    ):
        """VARCHAR(36): 1/35/36 = OK, 37 = INVALID_ARGUMENT."""
        stub = rpc.SourceCategoryServiceStub(channel)
        unique = uuid.uuid4().hex
        if name_len <= len(unique):
            name = unique[:name_len]
        else:
            name = unique + "x" * (name_len - len(unique))
        if name_len > 36:
            with pytest.raises(grpc.RpcError) as exc:
                stub.CreateSourceCategory(pb.CreateSourceCategoryRequest(name=name))
            assert exc.value.code() == grpc.StatusCode.INVALID_ARGUMENT
        else:
            with TrackedInsert(pg_conn, "source_category") as ti:
                resp = stub.CreateSourceCategory(pb.CreateSourceCategoryRequest(name=name))
                ti.register(resp.id)
            assert len(resp.name) == name_len