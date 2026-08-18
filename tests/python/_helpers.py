"""Shared helpers for the evGRpc pytest suite."""

from __future__ import annotations

import uuid

import psycopg


class TrackedInsert:
    """Per-function cleanup (L1).

    CRITICAL usage note: any read/Update RPC that depends on a row created
    earlier in the same block MUST run INSIDE the `with` block. Otherwise
    `__exit__` deletes the row before the read RPC runs, and the read
    returns NOT_FOUND.

    Canonical pattern (create + read inside block):
        with TrackedInsert(pg_conn, "vehicle") as ti:
            created = stub.CreateVehicle(req)
            ti.register(created.id)
            got = stub.GetVehicle(pb.GetVehicleRequest(id=created.id))  # inside!
        assert got.id == created.id

    For delete tests, use `ti.unregister(id)` after the Delete RPC:
        with TrackedInsert(pg_conn, "vehicle") as ti:
            created = stub.CreateVehicle(req)
            ti.register(created.id)
            stub.DeleteVehicle(pb.DeleteVehicleRequest(id=created.id))
            ti.unregister(created.id)  # skip L1 cleanup (already deleted)
        # then assert GetVehicle returns NOT_FOUND

    `__exit__` runs: DELETE FROM <table> WHERE id = ANY(<ids>).
    """

    def __init__(self, pg_conn, table: str):
        self._conn = pg_conn
        self._table = table
        self._ids: list[str] = []

    def register(self, id: str) -> None:
        self._ids.append(id)

    def unregister(self, id: str) -> None:
        """Remove a previously-registered id from cleanup tracking.

        Use after deleting the row via the RPC, to avoid a redundant DELETE
        in __exit__ (which would either be a no-op or a unique-fk violation).
        """
        try:
            self._ids.remove(id)
        except ValueError:
            pass

    def __enter__(self) -> "TrackedInsert":
        return self

    def __exit__(self, exc_type, exc, tb) -> None:
        if not self._ids:
            return
        with self._conn.cursor() as cur:
            cur.execute(
                f"DELETE FROM {self._table} WHERE id = ANY(%s)",
                (self._ids,),
            )
        self._conn.commit()


def _ns_prefix(ns: str) -> str:
    """All identifier helpers emit values starting with 'test-'."""
    return f"test-{ns}"


def make_license_plate(ns: str) -> str:
    """Returns 'test-<8hex><2hex>' = 15 chars exactly (VARCHAR(15) limit)."""
    return f"{_ns_prefix(ns)}{uuid.uuid4().hex[:2]}"


def make_weather_name(ns: str) -> str:
    """Returns 'test-<8hex><16hex>' = 29 chars (VARCHAR(36) limit)."""
    return f"{_ns_prefix(ns)}{uuid.uuid4().hex[:16]}"


def make_source_category_name(ns: str) -> str:
    """Same form as make_weather_name."""
    return make_weather_name(ns)


def make_charging_location(ns: str) -> str:
    """Returns 'test-loc-<8hex><8hex>' = 25 chars (VARCHAR(100) limit)."""
    return f"test-loc-{ns}{uuid.uuid4().hex[:8]}"


def make_consumption_remark(ns: str) -> str:
    """Returns 'test-rem-<8hex><8hex>' = 25 chars (TEXT column)."""
    return f"test-rem-{ns}{uuid.uuid4().hex[:8]}"


def make_uuid() -> str:
    """Random UUID for probe-style tests (NOT_FOUND, etc.)."""
    return str(uuid.uuid4())


def sweep_all_test_rows(pg_conn) -> dict[str, int]:
    """L2 cleanup (used by cleanup_namespace fixture).

    Children-first to respect FK NO ACTION constraint.
    Returns dict mapping table -> rows-deleted (for logging).
    """
    counts: dict[str, int] = {}
    with pg_conn.cursor() as cur:
        for table, like_clauses in [
            ("consumption", ["remark"]),
            ("charging", ["location", "remark"]),
            # NOTE: SQL column is `licenseplate` (no underscore).
            # `LicensePlate` (unquoted in sql/001_initial.sql) folds
            # to `licenseplate` in PostgreSQL. The proto field is
            # snake_case `license_plate` but that's irrelevant for
            # raw SQL.
            ("vehicle", ["licenseplate"]),
            ("weather", ["name"]),
            ("source_category", ["name"]),
        ]:
            where = " OR ".join(f"{c} LIKE 'test-%'" for c in like_clauses)
            cur.execute(f"DELETE FROM {table} WHERE {where}")
            counts[table] = cur.rowcount
    pg_conn.commit()
    return counts