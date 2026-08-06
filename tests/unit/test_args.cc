#include <gtest/gtest.h>
#include <string>
#include <vector>
#include "util/args.h"

namespace {

// Owns the underlying std::string storage so the returned char* pointers
// stay valid for the lifetime of the OwnedArgv object. The brief's
// `ToArgv` returned a vector of c_str() pointers into a temporary
// std::vector<std::string> — UB once the temporary was destroyed.
struct OwnedArgv {
  std::vector<std::string> storage;
  std::vector<char*> argv;
};

OwnedArgv ToArgv(const std::vector<std::string>& args) {
  OwnedArgv out;
  out.storage = args;
  for (auto& s : out.storage) out.argv.push_back(s.data());
  return out;
}

}  // namespace

TEST(ArgsTest, Defaults) {
  auto a = ToArgv({"evgrpc"});
  auto r = evgrpc::ParseArgs(static_cast<int>(a.argv.size()), a.argv.data());
  EXPECT_EQ(r.config_path, "./config.json");
  EXPECT_FALSE(r.help_requested);
}

TEST(ArgsTest, LongConfigFlag) {
  auto a = ToArgv({"evgrpc", "--config", "/etc/evgrpc/config.json"});
  auto r = evgrpc::ParseArgs(static_cast<int>(a.argv.size()), a.argv.data());
  EXPECT_EQ(r.config_path, "/etc/evgrpc/config.json");
  EXPECT_FALSE(r.help_requested);
}

TEST(ArgsTest, ShortConfigFlag) {
  auto a = ToArgv({"evgrpc", "-c", "/tmp/x.json"});
  auto r = evgrpc::ParseArgs(static_cast<int>(a.argv.size()), a.argv.data());
  EXPECT_EQ(r.config_path, "/tmp/x.json");
}

TEST(ArgsTest, HelpLong) {
  auto a = ToArgv({"evgrpc", "--help"});
  auto r = evgrpc::ParseArgs(static_cast<int>(a.argv.size()), a.argv.data());
  EXPECT_TRUE(r.help_requested);
}

TEST(ArgsTest, HelpShort) {
  auto a = ToArgv({"evgrpc", "-h"});
  auto r = evgrpc::ParseArgs(static_cast<int>(a.argv.size()), a.argv.data());
  EXPECT_TRUE(r.help_requested);
}

TEST(ArgsTest, UnknownFlag) {
  auto a = ToArgv({"evgrpc", "--foo"});
  EXPECT_THROW(evgrpc::ParseArgs(static_cast<int>(a.argv.size()), a.argv.data()),
               std::runtime_error);
}

TEST(ArgsTest, MissingConfigValue) {
  auto a = ToArgv({"evgrpc", "--config"});  // no value
  EXPECT_THROW(evgrpc::ParseArgs(static_cast<int>(a.argv.size()), a.argv.data()),
               std::runtime_error);
}
