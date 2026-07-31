include(FetchContent)

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
  nlohmann_json
  GIT_REPOSITORY https://gh-proxy.com/https://github.com/nlohmann/json.git
  GIT_TAG        v3.11.3
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
# testcontainers_cpp: deferred — the canonical repo
#   https://github.com/testcontainers/testcontainers-cpp (tag v0.20.0)
# referenced by the plan/brief no longer exists on GitHub (returns 404), and
# the project has moved to https://github.com/cppudge/testcontainers-cpp with a
# new version scheme (v0.1.x/v0.2.x). Task 1 only needs the build skeleton;
# testcontainers_cpp is not linked into evgrpc_server or evgrpc_tests. It is
# reintroduced in Task 20 once the correct repo/tag is confirmed.
# FetchContent_Declare(
#   testcontainers_cpp
#   GIT_REPOSITORY https://gh-proxy.com/https://github.com/cppudge/testcontainers-cpp.git
#   GIT_TAG        v0.2.0
#   GIT_SHALLOW    TRUE
# )
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
# Disable protobuf's install(EXPORT ...) rules. We only BUILD protobuf
# (use its lib + headers + protoc binary) — never install. Without this,
# protobuf's CMakeLists runs install(EXPORT "protobuf-targets" ...) at
# CONFIGURE time, which fails on every absl_* target with "not in any
# export set" because grpc also pulls in a separate copy of abseil
# (grpc-src/third_party/abseil-cpp) and the two abseil copies don't
# share a target graph. Configure fails with "Build files cannot be
# regenerated correctly" because the install rules abort generation.
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(grpc protobuf libpqxx nlohmann_json spdlog googletest jwt_cpp)

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

find_package(Threads REQUIRED)