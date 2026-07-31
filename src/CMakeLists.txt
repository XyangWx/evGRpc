add_subdirectory(auth)
add_subdirectory(config)
add_subdirectory(db)
add_subdirectory(log)
add_subdirectory(util)
add_subdirectory(services)   # adds evgrpc_services target; not yet linked
                              # into evgrpc_server — that wiring is Task 15.

add_executable(evgrpc_server main.cc)
target_link_libraries(evgrpc_server PRIVATE
  Threads::Threads
  evgrpc_auth
  evgrpc_config
  evgrpc_db
  evgrpc_log
  evgrpc_util
)
target_include_directories(evgrpc_server PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(evgrpc_server evgrpc_proto_gen)