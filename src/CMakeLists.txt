add_subdirectory(auth)
add_subdirectory(config)
add_subdirectory(db)
add_subdirectory(log)
add_subdirectory(util)
add_subdirectory(services)
add_subdirectory(proto)     # wraps generated *.pb.cc + *.grpc.pb.cc

# Per-target coverage (after add_subdirectory calls so project targets exist;
# before add_executable so the new evgrpc_server target also gets coverage).
# See top-level CMakeLists.txt for why this can't be a global add_compile_options.
if(EVGRPC_COVERAGE)
  foreach(target evgrpc_auth evgrpc_config evgrpc_db evgrpc_log evgrpc_util
                    evgrpc_services evgrpc_proto evgrpc_proto_gen)
    if(TARGET ${target})
      target_compile_options(${target} PRIVATE --coverage -O0 -g)
      target_link_options(${target} PRIVATE --coverage)
    endif()
  endforeach()
endif()

add_executable(evgrpc_server main.cc)
target_link_libraries(evgrpc_server PRIVATE
  Threads::Threads
  evgrpc_auth
  evgrpc_config
  evgrpc_db
  evgrpc_log
  evgrpc_util
  evgrpc_services
)
target_include_directories(evgrpc_server PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(evgrpc_server evgrpc_proto_gen)