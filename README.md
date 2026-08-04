# evGRpc

EV electricity cost tracking gRPC service. C++20, gRPC v1.62, PostgreSQL 14+,
JWT (RS256) auth via OAuth 2.0 resource server semantics.

## Build (host)

```sh
cmake -G Ninja -S . -B build
cmake --build build --target evgrpc_server
```

System packages required (Debian/Ubuntu):

- `build-essential`, `cmake` (≥3.22), `ninja-build`, `git`, `pkg-config`
- `libpqxx-dev`, `libpq-dev`, `libssl-dev`, `libcurl4-openssl-dev`
- `libabsl-dev`, `libre2-dev`, `libc-ares-dev`, `zlib1g-dev`
- `libprotobuf-dev`, `protobuf-compiler`
- `nlohmann-json3-dev`, `uuid-dev`

Fetched via FetchContent (network access required to github.com or a
gh-proxy mirror; see `cmake/deps.cmake`):

- gRPC v1.62.0 (with boringssl, abseil, cares, re2, zlib submodules)
- protobuf v25.1 (for the `protoc` binary)
- libpqxx 7.9.2, spdlog v1.13.0, googletest v1.14.0
- jwt-cpp (with its own nlohmann/json FetchContent fallback if the system
  pkg isn't installed)
- cpp_httplib, testcontainers-cpp v0.2.0

## Test (host)

```sh
cmake --build build --target evgrpc_tests evgrpc_e2e_tests
./build/tests/evgrpc_tests

EVGRPC_TEST_DB_PASSWORD=… ./build/tests/integration/evgrpc_e2e_tests
```

- 38 unit tests cover config, JWT validation, JWKS caching, all 6 service
  layers, DB pool, ID generation, logging.
- 1 e2e test (`E2ESmoke.CreateThenListVehicle`) brings up an in-process
  gRPC server + JWKS IdP + shared local PG and walks CreateVehicle →
  ListVehicles.
- The e2e test truncates `vehicle ... CASCADE` at the start, so it's
  re-runnable without manual cleanup.

## Run (host)

```sh
DATABASE_URL='postgresql://user:pw@host:5432/db' \
OAUTH_ISSUER_URL=https://issuer.example.com \
OAUTH_AUDIENCE=evgrpc-api \
OAUTH_JWKS_URL=https://issuer.example.com/.well-known/jwks.json \
GRPC_PORT=50051 \
./build/src/evgrpc_server
```

All four `DATABASE_URL` + three `OAUTH_*` env vars are required.
`GRPC_PORT` defaults to 50051.

## Docker

```sh
docker build -t evgrpc:dev .
```

Behind a restrictive network (e.g. mainland China):

```sh
docker build \
  --build-arg APT_MIRROR=http://mirrors.tuna.tsinghua.edu.cn/ubuntu/ \
  --build-arg GIT_INSTEADOF=https://gh-proxy.com/https://github.com/ \
  -t evgrpc:dev .
```

Build args:

- `APT_MIRROR` — apt source line rewrite. Empty by default (hermetic).
  Pass `http://…` for a working mirror. HTTPS mirrors don't work because
  the base image doesn't ship with a ca-bundle that trusts third-party
  mirrors; HTTP is fine for apt since packages carry no secret payload.
- `GIT_INSTEADOF` — git URL rewrite. Empty by default. Pass
  `https://gh-proxy.com/https://github.com/` to route github.com traffic
  through a proxy.

Run:

```sh
docker run --rm --network host \
  -e DATABASE_URL='postgresql://user:pw@host.docker.internal:5432/db' \
  -e OAUTH_ISSUER_URL=https://issuer.example.com \
  -e OAUTH_AUDIENCE=evgrpc-api \
  -e OAUTH_JWKS_URL=https://issuer.example.com/.well-known/jwks.json \
  -e GRPC_PORT=50051 \
  evgrpc:dev
```

Notes:

- `--network host` lets the container reach services on the host
  network namespace (e.g. a locally-running PostgreSQL on `127.0.0.1`).
  On macOS/Windows Docker Desktop, use `host.docker.internal` in
  `DATABASE_URL` instead.
- **URL-encode `@` in the password** as `%40`. libpqxx refuses a raw
  `@` because it would be parsed as the user/host separator.

## Smoke

```sh
DATABASE_URL='postgresql://evgrpc_admin:NewUser%40123@127.0.0.1:5432/evgrpc' \
scripts/smoke.sh
```

Starts the `evgrpc:dev` container, generates an ephemeral RSA keypair,
serves the JWKS from a local `python3 -m http.server`, mints an RS256
JWT, runs `VehicleService.CreateVehicle` → `ListVehicles` via `grpcurl`,
then tears everything down. Exits 0 on success, 1 on any failure.

Re-runs are idempotent-ish (CreateVehicle doesn't enforce a unique
license plate, so each run adds another `SMOKE-<pid>` row; ListVehicles
returns all rows; both calls succeed).

Requires: `docker`, `python3` with `PyJWT` + `cryptography`, `openssl`,
and `grpcurl` (the script auto-falls-back to `~/.local/bin/grpcurl` or
whatever `GRPCURL=$PWD/grpcurl` points at).

## Env vars

| var                    | required | default | notes                                                       |
|------------------------|----------|---------|-------------------------------------------------------------|
| `DATABASE_URL`         | yes      | —       | `postgresql://user:pw@host:port/db`. URL-encode `@` in pw as `%40`. |
| `OAUTH_ISSUER_URL`     | yes      | —       | Expected value of the JWT `iss` claim.                      |
| `OAUTH_AUDIENCE`       | yes      | —       | Expected value of the JWT `aud` claim.                      |
| `OAUTH_JWKS_URL`       | yes      | —       | URL to the JWKS document (RS256 public keys).               |
| `OAUTH_JWKS_CACHE_TTL` | no       | 3600    | JWKS cache TTL in seconds.                                  |
| `GRPC_PORT`            | no       | 50051   | TCP port the server binds on `0.0.0.0`.                     |

## Spec

See `docs/superpowers/specs/2026-07-30-evgrpc-design.md` for the full
design (services, DB schema, auth model, logging conventions).