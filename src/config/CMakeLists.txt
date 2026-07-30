add_library(evgrpc_config config.cc)
target_link_libraries(evgrpc_config PUBLIC spdlog::spdlog)
target_include_directories(evgrpc_config PUBLIC ${CMAKE_SOURCE_DIR}/src)
