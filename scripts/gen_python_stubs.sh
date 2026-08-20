#!/usr/bin/env bash
# Regenerate Python gRPC stubs from proto/evgrpc/*.proto.
# Re-run only when a .proto file changes (stubs are committed).
# GNU sed -i (no backup arg) is fine on the dev VM (Linux).
set -euo pipefail
cd "$(dirname "$0")/.."
python -m grpc_tools.protoc \
    -I proto \
    --python_out=tests/python/gen \
    --grpc_python_out=tests/python/gen \
    proto/evgrpc/*.proto \
    proto/google/type/date.proto
# Generated *_pb2_grpc.py uses `from evgrpc import <svc>_pb2 as ...` for each
# of the 6 services (vehicle, weather, source_category, charging, consumption,
# display). Rewrite to package-relative form so the `tests.python.gen.evgrpc`
# package imports resolve correctly.
# (protoc's exact form depends on grpcio-tools version; older versions emit
# bare `import <svc>_pb2 as ...`. Both forms are handled by this sed.)
#
# NB: `from google.type import date_pb2` is rewritten to the absolute
# dotted path under the project's test namespace (`tests.python.gen.*`)
# so the vendored stub resolves via `pythonpath = .` in pytest.ini and
# we don't shadow the real `google.protobuf` package installed in the env.
sed -i -E 's/^(from evgrpc |)import ([a-z_]+)_pb2 as/from . import \2_pb2 as/' \
    tests/python/gen/evgrpc/*_pb2_grpc.py
# Generated *_pb2.py cross-imports sibling messages (e.g. vehicle.proto
# imports evgrpc/common.proto; display.proto imports both common and
# charging). protoc emits `from evgrpc import common_pb2 as ...`; rewrite
# to package-relative form.
sed -i -E 's/^from evgrpc import ([a-z_]+)_pb2/from . import \1_pb2/' \
    tests/python/gen/evgrpc/*_pb2.py
# Rewrite the absolute `from google.type import date_pb2` so it resolves
# to the vendored stub under the project's test namespace instead of
# shadowing the installed `google.protobuf` package.
sed -i -E 's/^from google\.type import date_pb2(\s+as\s+google_dot_type_dot_date__pb2)?$/from tests.python.gen.google.type import date_pb2 as google_dot_type_dot_date__pb2/' \
    tests/python/gen/evgrpc/*_pb2.py
# Generate __init__.py for the gen package, the evgrpc subpackage, and
# the vendored google.type stub tree (vehicle.proto imports
# google/type/date.proto).
touch tests/python/gen/__init__.py
touch tests/python/gen/evgrpc/__init__.py
touch tests/python/gen/google/__init__.py
touch tests/python/gen/google/type/__init__.py