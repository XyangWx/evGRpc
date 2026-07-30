add_subdirectory(config)

add_executable(evgrpc_server main.cc)
target_link_libraries(evgrpc_server PRIVATE Threads::Threads evgrpc_config)
target_include_directories(evgrpc_server PRIVATE ${CMAKE_BINARY_DIR}/generated)
add_dependencies(evgrpc_server evgrpc_proto_gen)