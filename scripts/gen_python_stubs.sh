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
    proto/evgrpc/*.proto
# Generated *_pb2_grpc.py uses `from evgrpc import <svc>_pb2 as ...` for each
# of the 6 services (vehicle, weather, source_category, charging, consumption,
# display). Rewrite to package-relative form so the `tests.python.gen.evgrpc`
# package imports resolve correctly.
# (protoc's exact form depends on grpcio-tools version; older versions emit
# bare `import <svc>_pb2 as ...`. Both forms are handled by this sed.)
sed -i -E 's/^(from evgrpc |)import ([a-z_]+)_pb2 as/from . import \2_pb2 as/' \
    tests/python/gen/evgrpc/*_pb2_grpc.py
# Generated *_pb2.py cross-imports sibling messages (e.g. vehicle.proto
# imports evgrpc/common.proto; display.proto imports both common and
# charging). protoc emits `from evgrpc import common_pb2 as ...`; rewrite
# to package-relative form.
sed -i -E 's/^from evgrpc import ([a-z_]+)_pb2/from . import \1_pb2/' \
    tests/python/gen/evgrpc/*_pb2.py
# Generate __init__.py for the gen package and the evgrpc subpackage.
touch tests/python/gen/__init__.py
touch tests/python/gen/evgrpc/__init__.py