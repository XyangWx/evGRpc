add_subdirectory(auth)
add_subdirectory(config)
add_subdirectory(db)
add_subdirectory(log)
add_subdirectory(util)
add_subdirectory(services)
add_subdirectory(proto)     # wraps generated *.pb.cc + *.grpc.pb.cc

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

# Per-target coverage (see top-level CMakeLists.txt for why global fails).
# Run AFTER all add_subdirectory/add_executable/add_library calls so every
# project target (including evgrpc_server) is in scope.
# Type guard filters out UTILITY (e.g. evgrpc_proto_gen — add_custom_target)
# and INTERFACE_LIBRARY (header-only) targets that can't take compile/link
# options.
if(EVGRPC_COVERAGE)
  foreach(target evgrpc_auth evgrpc_config evgrpc_db evgrpc_log evgrpc_util
                    evgrpc_services evgrpc_proto evgrpc_server)
    if(TARGET ${target})
      get_target_property(_type ${target} TYPE)
      if(NOT _type STREQUAL "UTILITY" AND
         NOT _type STREQUAL "INTERFACE_LIBRARY")
        target_compile_options(${target} PRIVATE --coverage -O0 -g)
        target_link_options(${target} PRIVATE --coverage)
      endif()
    endif()
  endforeach()
endif()