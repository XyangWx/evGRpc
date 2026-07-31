add_library(evgrpc_log log.cc)
target_link_libraries(evgrpc_log PUBLIC spdlog::spdlog)
target_include_directories(evgrpc_log PUBLIC ${CMAKE_SOURCE_DIR}/src)