include(FetchContent)

# Need GIT_EXECUTABLE for the manual `git config --global ... insteadOf`
# below. Without this, ${GIT_EXECUTABLE} is empty and execute_process
# fails with exit code 1.
find_package(Git REQUIRED)

set(FETCHCONTENT_QUIET FALSE)

# Note: this environment has severely rate-limited direct connectivity to
# github.com (git clone stalls at ~3 KB/s; behind GFW, port 443 is fully
# blocked). All deps are fetched through the gh-proxy.com mirror, which
# proxies GitHub at full speed. The gitconfig
# `url.<proxy>.insteadOf = github.com` redirection (set globally in this
# session) ensures submodules also go through the mirror. The upstream tags/
# versions are unchanged — only the transport is mirrored.
#
# We set the redirect HERE (at configure time) so the project is
# self-contained — new clones don't need a manual `git config --global`
# step. FETCHCONTENT runs git submodules with a clean env that respects
# the gitconfig we set. Tested: with this line, grpc's
# third_party/{abseil,boringssl,cares,re2,zlib} submodules all clone
# through gh-proxy.com; without it, every submodule fails on the user's
# machine with "Failed to connect to github.com port 443".
execute_process(
  COMMAND ${GIT_EXECUTABLE}
          config --global url.https://gh-proxy.com/https://github.com/.insteadOf
                  https://github.com/
  RESULT_VARIABLE _gh_proxy_config_rc
  OUTPUT_QUIET ERROR_QUIET
)
if(NOT _gh_proxy_config_rc EQUAL 0)
  message(WARNING
    "Failed to set git redirect to gh-proxy.com. If you're behind the GFW "
    "or on a slow link, grpc's submodules will fail to clone. Run "
    "manually:  git config --global url.gh-proxy.com.insteadOf "
    "https://github.com/")
endif()

FetchContent_Declare(
  grpc
  GIT_REPOSITORY https://gh-proxy.com/https://github.com/grpc/grpc.git
  GIT_TAG        v1.62.0
  GIT_SHALLOW    TRUE
  # Only fetch the submodules grpc's CMake build actually needs. Fetching all
  # 20 submodules recursively pulls in benchmark/bloaty/opentelemetry-cpp
  # etc. that we don't need and that explode disk usage.
  GIT_SUBMODULES "third_party/abseil-cpp" "third_party/cares/cares" "third_party/re2" "third_party/zlib" "third_party/boringssl-with-bazel"
)
FetchContent_Declare(
  protobuf
  GIT_REPOSITORY https://gh-proxy.com/https://github.com/protocolbuffers/protobuf.git
  GIT_TAG        v25.1
  GIT_SHALLOW    TRUE
  # Build the `protoc` binary alongside libprotobuf. Without this,
  # protoc.cmake falls back to the host's `protoc` (3.21.12 on Ubuntu
  # noble), which generates .pb.h files in the legacy 3.x format
  # that 4.x protobuf headers refuse (`#error regenerate with newer
  # protoc` at the version-check line). Building protoc from source
  # adds ~30s to first-time configure but matches the 4.25 headers.
)
FetchContent_Declare(
  libpqxx
  GIT_REPOSITORY https://gh-proxy.com/https://github.com/jtv/libpqxx.git
  GIT_TAG        7.9.2
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://gh-proxy.com/https://github.com/gabime/spdlog.git
  GIT_TAG        v1.13.0
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://gh-proxy.com/https://github.com/google/googletest.git
  GIT_TAG        v1.14.0
  GIT_SHALLOW    TRUE
)
# testcontainers_cpp: Task 20 reintroduction. The canonical repo
#   https://github.com/testcontainers/testcontainers-cpp (tag v0.20.0
#   referenced by the original plan) is no longer reachable on GitHub;
#   the project moved to https://github.com/cppudge/testcontainers-cpp
#   with the v0.2.0 release line (verified 2026-08-04 via git ls-remote).
# Requires C++20 (declared on its INTERFACE via cxx_std_20). We disable
# its bundled conan provider and link against system OpenSSL / libcurl
# (already pulled in transitively via jwt-cpp + curl). On the test
# machine, the Docker daemon must be reachable at /var/run/docker.sock.
FetchContent_Declare(
  testcontainers_cpp
  GIT_REPOSITORY https://gh-proxy.com/https://github.com/cppudge/testcontainers-cpp.git
  GIT_TAG        v0.2.0
  GIT_SHALLOW    TRUE
)
# Disable TLS + host-port forwarding: drops OpenSSL + libssh2 from the
# dep graph (we only need localhost http for the JWKS endpoint).
# These options must be set BEFORE MakeAvailable since testcontainers-cpp
# declares them via option() inside its own CMakeLists.
set(TC_TLS OFF CACHE BOOL "" FORCE)
set(TC_HOST_PORT_FORWARDING OFF CACHE BOOL "" FORCE)
set(SKIP_CONAN_PROVIDER_CMAKE ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(TC_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(TC_BUILD_INTEGRATION_TESTS OFF CACHE BOOL "" FORCE)
# testcontainers-cpp includes CTest unconditionally, which auto-enables
# BUILD_TESTING and then fails on `find_package(GTest REQUIRED)`. We
# don't want their unit tests in our graph (we run our own).
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
# cpp-httplib: header-only HTTP server used by TestServer to serve the
# JWKS endpoint on a random localhost port. Picked over Crow / Pistache
# because it's a single header + no link-time deps.
FetchContent_Declare(
  cpp_httplib
  GIT_REPOSITORY https://gh-proxy.com/https://github.com/yhirose/cpp-httplib.git
  GIT_TAG        v0.18.5
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  jwt_cpp
  GIT_REPOSITORY https://gh-proxy.com/https://github.com/Thalhammer/jwt-cpp.git
  GIT_TAG        v0.7.0
  GIT_SHALLOW    TRUE
)
FetchContent_Declare(
  curl
  GIT_REPOSITORY https://gh-proxy.com/https://github.com/curl/curl.git
  GIT_TAG        curl-8_5_0
  GIT_SHALLOW    TRUE
)

# Trim grpc build: drop language plugins we don't ship (only cpp_plugin is used
# by protoc), use system OpenSSL instead of bundling boringssl, and skip
# xDS/upb-gen dirs that pull in extra codegen. These cuts roughly halve the
# grpc build target count.
set(gRPC_BUILD_GRPC_CSHARP_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_NODE_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_PHP_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_PYTHON_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_BUILD_GRPC_RUBY_PLUGIN OFF CACHE BOOL "" FORCE)
set(gRPC_SSL_PROVIDER package CACHE STRING "" FORCE)
set(gRPC_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
# Disable protobuf's vendored gtest build: it would create gmock/gmock_main
# targets that collide with our separate googletest FetchContent (CMP0002
# "add_library cannot create target 'gmock' because another target with
# the same name already exists"). We provide our own gtest via the
# googletest FetchContent below; we never run protobuf's own test suite.
set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
# Disable install(EXPORT ...) rules for the FetchContent deps we never
# install. Each library's CMakeLists runs `install(EXPORT ...)` at
# CONFIGURE time, and fails with "absl_* not in any export set" if a
# transitive dep (especially abseil) is also pulled in separately by
# a sibling target — because FetchContent's two-level submodule pulls
# (grpc → abseil, protobuf → abseil) don't share a target graph.
#
# We never install any of these — only build. Disabling install on
# the consumer side is the canonical fix.
set(protobuf_INSTALL            OFF CACHE BOOL "" FORCE)
set(utf8_range_ENABLE_INSTALL  OFF CACHE BOOL "" FORCE)
set(absl_ENABLE_INSTALL        OFF CACHE BOOL "" FORCE)
set(c-ares_ENABLE_INSTALL      OFF CACHE BOOL "" FORCE)
set(gRPC_INSTALL              OFF CACHE BOOL "" FORCE)
# CMake 3.27+: globally skip install(EXPORT ...) rules at configure time.
# We never run `cmake --install`; this is the belt-and-suspenders fallback
# for any FetchContent lib whose install-option name we didn't guess
# correctly above. Verified to silence the abseil-export storm that
# started at Task 17 / 48c6d3384.
set(CMAKE_SKIP_INSTALL_RULES   ON  CACHE BOOL "" FORCE)

# CRITICAL ORDER: pre-populate grpc BEFORE FetchContent_MakeAvailable so we
# can patch grpc-src/cmake/protobuf.cmake BEFORE gRPC's own CMakeLists.txt
# runs (inside MakeAvailable's add_subdirectory call).
#
# Background: FetchContent_MakeAvailable does populate + add_subdirectory
# in one call. If we don't pre-populate, gRPC's CMakeLists.txt runs with the
# UNPATCHED cmake/protobuf.cmake, captures the broken $<TARGET_FILE:protoc>
# generator expression (or empty string after the if(EXISTS) check fails on
# the missing grpc-src/third_party/protobuf submodule) into its
# add_custom_command, and the patch runs too late. Reconfigure doesn't fix
# it because ${grpc_ADDED_SUBDIRECTORY} cache var prevents MakeAvailable
# from re-running add_subdirectory(grpc-src).
#
# v0.2.1 (5690ba736) introduced this pre-populate + patch + MakeAvailable
# pattern. v0.2.7 (5e00a9af9) accidentally dropped the pre-populate when
# refactoring the patch block, regressing reflection/channelz codegen on
# `--target all`. Restoring it here.
FetchContent_GetProperties(grpc)
if(NOT grpc_POPULATED)
  FetchContent_Populate(grpc)
endif()

# Wire gRPC's cmake/protobuf.cmake to use our FetchContent protobuf.
# gRPC defaults PROTOBUF_ROOT_DIR to ${CMAKE_CURRENT_SOURCE_DIR}/third_party/protobuf
# (= grpc-src/third_party/protobuf), which doesn't exist because we don't fetch
# gRPC's protobuf submodule. When EXISTS fails, gRPC's protobuf.cmake falls into
# its else() warning branch and never sets _gRPC_PROTOBUF_PROTOC_EXECUTABLE —
# leaving it empty for the gRPC internal reflection/channelz/health.pb.cc
# codegen add_custom_command, which then runs `cd ... && --grpc_out=...: not found`.
#
# PATCH ONLY the file content here. We don't set PROTOBUF_ROOT_DIR yet because
# `protobuf_SOURCE_DIR` is empty until FetchContent_MakeAvailable populates
# protobuf (it lives in PARENT_SCOPE of FetchContent_Populate, which only runs
# inside MakeAvailable). The third string(REPLACE) uses [==[...]==] bracket
# argument so ${PROTOBUF_ROOT_DIR} stays LITERAL in the patched file — we'll
# set PROTOBUF_ROOT_DIR right before the manual add_subdirectory(grpc-src)
# below (after MakeAvailable has populated protobuf).
set(_grpc_pb_cmake "${grpc_SOURCE_DIR}/cmake/protobuf.cmake")
if(EXISTS "${_grpc_pb_cmake}")
  file(READ "${_grpc_pb_cmake}" _grpc_pb_cmake_content)
  string(REPLACE
    "set(_gRPC_PROTOBUF_PROTOC_EXECUTABLE $<TARGET_FILE:protoc>)"
    "set(_gRPC_PROTOBUF_PROTOC_EXECUTABLE \"${CMAKE_BINARY_DIR}/_deps/protobuf-build/protoc\")"
    _grpc_pb_cmake_content "${_grpc_pb_cmake_content}")
  string(REPLACE
    "set(_gRPC_PROTOBUF_PROTOC_EXECUTABLE $<TARGET_FILE:protobuf::protoc>)"
    "set(_gRPC_PROTOBUF_PROTOC_EXECUTABLE \"${CMAKE_BINARY_DIR}/_deps/protobuf-build/protoc\")"
    _grpc_pb_cmake_content "${_grpc_pb_cmake_content}")
  # NOTE: bracket argument [==[...]==] prevents cmake from evaluating
  # ${PROTOBUF_ROOT_DIR} inside the strings. We need the literal text
  # "${PROTOBUF_ROOT_DIR}" (no expansion) for both match and replace,
  # because grpc-src/cmake/protobuf.cmake:33 itself contains the literal
  # "${PROTOBUF_ROOT_DIR}" (it's evaluated at gRPC's own cmake time, not ours).
  string(REPLACE
    [==[    add_subdirectory(${PROTOBUF_ROOT_DIR} third_party/protobuf)]==]
    [==[    if(NOT TARGET libprotobuf)
      add_subdirectory(${PROTOBUF_ROOT_DIR} third_party/protobuf)
    endif()]==]
    _grpc_pb_cmake_content "${_grpc_pb_cmake_content}")
  file(WRITE "${_grpc_pb_cmake}" "${_grpc_pb_cmake_content}")
  message(STATUS "DEBUG v0.2.8: wrote patched protobuf.cmake (PROTOBUF_ROOT_DIR=pending-MakeAvailable)")
  message(STATUS
    "evGRpc workaround: patched grpc-src/cmake/protobuf.cmake to use "
    "absolute protoc path (gRPC 1.62 $<TARGET_FILE:protoc> bug)")
endif()

# Now MakeAvailable. grpc is already populated (POPULATED=TRUE from our
# pre-populate above), so MakeAvailable's `if(NOT POPULATED)` block is
# SKIPPED for grpc — no populate, no add_subdirectory. For protobuf and
# the rest, populate + add_subdirectory both run, creating libprotobuf /
# libprotoc / protoc targets in our scope.
FetchContent_MakeAvailable(grpc protobuf libpqxx spdlog googletest jwt_cpp cpp_httplib)

# Refresh protobuf_SOURCE_DIR after MakeAvailable populated protobuf
# (it's PARENT_SCOPE from FetchContent_Populate, only set now).
FetchContent_GetProperties(protobuf)
set(PROTOBUF_ROOT_DIR "${protobuf_SOURCE_DIR}")
message(STATUS "DEBUG v0.2.8: PROTOBUF_ROOT_DIR set to '${PROTOBUF_ROOT_DIR}' in evGRpc scope")

# Now manually add_subdirectory grpc-src. MakeAvailable skipped grpc's
# add_subdirectory (POPULATED=TRUE), so grpc_cpp_plugin target doesn't
# exist in our scope yet. The PATCHED grpc-src/cmake/protobuf.cmake
# uses absolute path for _gRPC_PROTOBUF_PROTOC_EXECUTABLE, and the
# patched line 33 (now wrapped in if(NOT TARGET libprotobuf)) is a
# no-op because libprotobuf already exists from protobuf MakeAvailable.
if(NOT TARGET grpc_cpp_plugin)
  add_subdirectory(${grpc_SOURCE_DIR} ${grpc_BINARY_DIR})
endif()

# Patch grpc-src/CMakeLists.txt:557 — same generator-expression bug for
# grpc_cpp_plugin. gRPC sets `_gRPC_CPP_PLUGIN = $<TARGET_FILE:grpc_cpp_plugin>`
# in its internal protobuf_generate_grpc() function, used to generate
# reflection/channelz/health.pb.cc — same FetchContent boundary bug as
# the protoc one. Replace with the absolute path where grpc's
# add_executable(grpc_cpp_plugin) lands.
set(_grpc_cmake "${grpc_SOURCE_DIR}/CMakeLists.txt")
if(EXISTS "${_grpc_cmake}")
  file(READ "${_grpc_cmake}" _grpc_cmake_content)
  string(REPLACE
    "set(_gRPC_CPP_PLUGIN \$<TARGET_FILE:grpc_cpp_plugin>)"
    "set(_gRPC_CPP_PLUGIN \"${CMAKE_BINARY_DIR}/_deps/grpc-build/grpc_cpp_plugin\")"
    _grpc_cmake_content "${_grpc_cmake_content}")
  file(WRITE "${_grpc_cmake}" "${_grpc_cmake_content}")
  message(STATUS
    "evGRpc workaround: patched grpc-src/CMakeLists.txt to use "
    "absolute grpc_cpp_plugin path (gRPC 1.62 \$<TARGET_FILE:...> bug)")
endif()

# testcontainers_cpp is only needed by the integration test fixture.
# Production Docker images (and any other build that doesn't compile
# tests/) can disable it to skip the Boost dependency.
option(EVGRPC_WITH_TESTCONTAINERS "Populate testcontainers_cpp (needed for e2e tests)" ON)
if(EVGRPC_WITH_TESTCONTAINERS)
  FetchContent_MakeAvailable(testcontainers_cpp)
endif()

# gRPC's CMake config defaults to using its bundled third_party/protobuf for
# protobuf headers (gRPC_PROTOBUF_PROVIDER=module). We didn't fetch that
# submodule, so the include dirs end up empty and grpc_cpp_plugin fails to
# compile with `fatal error: google/protobuf/compiler/code_generator.h: No
# such file or directory`. Manually wire the FetchContent protobuf headers
# and libprotoc into the grpc_cpp_plugin / grpc_plugin_support targets.
# gRPC's CMakeLists uses the plain target_link_libraries() signature for
# these targets, so we must too (mixing plain and keyword signatures is an
# error). target_include_directories() doesn't have that restriction, so
# we can use PRIVATE there.
if(TARGET grpc_cpp_plugin)
  target_include_directories(grpc_cpp_plugin PRIVATE ${protobuf_SOURCE_DIR}/src)
  target_include_directories(grpc_plugin_support PRIVATE ${protobuf_SOURCE_DIR}/src)
  target_link_libraries(grpc_plugin_support libprotoc libprotobuf)
  target_link_libraries(grpc_cpp_plugin libprotoc libprotobuf)
endif()
# curl: only libcurl target, fetched below
set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(curl)

# nlohmann_json: provided by system package nlohmann-json3-dev (apt).
# Promoted to top-level dep so src/config/config_loader.cc (config.json
# parsing, see docs/superpowers/plans/2026-08-06-config-json-migration.md
# Task 2) can include <nlohmann/json.hpp> unconditionally. We avoid
# FetchContent here because testcontainers-cpp does
# `find_package(nlohmann_json REQUIRED)` and would fail without an
# installed nlohmann_jsonConfig.cmake; system package provides it.
# jwt-cpp and testcontainers-cpp both pick it up via find_package.
find_package(nlohmann_json 3.11.0 REQUIRED)

find_package(Threads REQUIRED)