# config: env-var Config (legacy) + config.json SchemaConfig (new, Task 2)
# nlohmann_json is a top-level dep (Task 1) for config_loader.cc.
add_library(evgrpc_config
  config.cc
  config_loader.cc
)
target_link_libraries(evgrpc_config PUBLIC spdlog::spdlog nlohmann_json::nlohmann_json)
target_include_directories(evgrpc_config PUBLIC ${CMAKE_SOURCE_DIR}/src)
