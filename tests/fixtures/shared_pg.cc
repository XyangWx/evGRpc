#include "fixtures/shared_pg.h"

#include <fstream>
#include <sstream>
#include <pqxx/pqxx>

namespace evgrpc::test {

namespace {
std::shared_ptr<PgContainer> g_pg;
}

std::shared_ptr<PgContainer> SharedPgEnvironment::pg() { return g_pg; }

void SharedPgEnvironment::TruncateAll() {
  auto c = std::make_shared<pqxx::connection>(g_pg->Conninfo());
  pqxx::work tx(*c);
  tx.exec("TRUNCATE vehicle, charging, consumption, "
          "source_category, weather CASCADE");
  tx.commit();
}

void SharedPgEnvironment::SetUp() {
  g_pg = std::make_shared<PgContainer>();
  // Apply schema (idempotent if dev DB already has it).
  // Path comes in via -DEVGRPC_TEST_SQL_PATH at compile time
  // (absolute path injected by CMake's `target_compile_definitions`)
  // so we don't depend on CWD — tests run from `cmake-build-debug/`.
  const auto sql_text = []() {
    std::ifstream f(EVGRPC_TEST_SQL_PATH);
    std::stringstream ss; ss << f.rdbuf();
    return ss.str();
  }();
  auto c = std::make_shared<pqxx::connection>(g_pg->Conninfo());
  pqxx::work tx(*c);
  tx.exec(sql_text);
  tx.commit();
}

void SharedPgEnvironment::TearDown() {
  g_pg.reset();
}

}  // namespace evgrpc::test
