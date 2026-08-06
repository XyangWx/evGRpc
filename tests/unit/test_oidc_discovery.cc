#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <string>
#include <thread>
#include <httplib.h>
#include "auth/oidc_discovery.h"

namespace {

// RAII httplib::Server bound to an ephemeral port. The server runs in
// a background thread; Stop() joins it on destruction.
class TestHttpServer {
public:
    TestHttpServer() {
        port_ = srv_.bind_to_any_port("127.0.0.1");
        thr_ = std::thread([this]() { srv_.listen_after_bind(); });
        // Wait for server to be ready (httplib signals via is_running()).
        for (int i = 0; i < 100 && !srv_.is_running(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    ~TestHttpServer() {
        srv_.stop();
        if (thr_.joinable()) thr_.join();
    }
    int port() const { return port_; }
    httplib::Server& server() { return srv_; }
private:
    httplib::Server srv_;
    int port_;
    std::thread thr_;
};

std::string IssuerUrl(int port) {
    return "http://127.0.0.1:" + std::to_string(port);
}

}  // namespace

TEST(OidcDiscoveryTest, HappyPath) {
    TestHttpServer s;
    s.server().Get("/.well-known/openid-configuration",
                   [](const httplib::Request&, httplib::Response& res) {
                       res.set_content(
                           R"({"issuer":"https://x","jwks_uri":"https://x/jwks"})",
                           "application/json");
                   });

    auto jwks = evgrpc::auth::DiscoverJwksUri(IssuerUrl(s.port()));
    EXPECT_EQ(jwks, "https://x/jwks");
}

TEST(OidcDiscoveryTest, TrailingSlashIssuer) {
    TestHttpServer s;
    s.server().Get("/.well-known/openid-configuration",
                   [](const httplib::Request&, httplib::Response& res) {
                       res.set_content(
                           R"({"jwks_uri":"https://y/jwks"})",
                           "application/json");
                   });
    // Note trailing slash — must still hit the same endpoint.
    auto jwks = evgrpc::auth::DiscoverJwksUri(IssuerUrl(s.port()) + "/");
    EXPECT_EQ(jwks, "https://y/jwks");
}

TEST(OidcDiscoveryTest, IssuerPath) {
    TestHttpServer s;
    // Server only matches /tenant/.well-known/openid-configuration
    s.server().Get("/tenant/.well-known/openid-configuration",
                   [](const httplib::Request&, httplib::Response& res) {
                       res.set_content(
                           R"({"jwks_uri":"https://z/jwks"})",
                           "application/json");
                   });
    auto jwks = evgrpc::auth::DiscoverJwksUri(
        IssuerUrl(s.port()) + "/tenant");
    EXPECT_EQ(jwks, "https://z/jwks");
}

TEST(OidcDiscoveryTest, Rejects404) {
    TestHttpServer s;
    s.server().Get("/.well-known/openid-configuration",
                   [](const httplib::Request&, httplib::Response& res) {
                       res.status = 404;
                   });
    EXPECT_THROW(evgrpc::auth::DiscoverJwksUri(IssuerUrl(s.port())),
                 std::runtime_error);
}

TEST(OidcDiscoveryTest, RejectsMalformedJson) {
    TestHttpServer s;
    s.server().Get("/.well-known/openid-configuration",
                   [](const httplib::Request&, httplib::Response& res) {
                       res.set_content("not json", "application/json");
                   });
    EXPECT_THROW(evgrpc::auth::DiscoverJwksUri(IssuerUrl(s.port())),
                 std::runtime_error);
}

TEST(OidcDiscoveryTest, RejectsMissingJwksUri) {
    TestHttpServer s;
    s.server().Get("/.well-known/openid-configuration",
                   [](const httplib::Request&, httplib::Response& res) {
                       res.set_content(R"({"issuer":"https://x"})",
                                       "application/json");
                   });
    EXPECT_THROW(evgrpc::auth::DiscoverJwksUri(IssuerUrl(s.port())),
                 std::runtime_error);
}

TEST(OidcDiscoveryTest, RejectsNonHttpJwksUri) {
    TestHttpServer s;
    s.server().Get("/.well-known/openid-configuration",
                   [](const httplib::Request&, httplib::Response& res) {
                       res.set_content(
                           R"({"jwks_uri":"ftp://x/jwks"})",
                           "application/json");
                   });
    EXPECT_THROW(evgrpc::auth::DiscoverJwksUri(IssuerUrl(s.port())),
                 std::runtime_error);
}

TEST(OidcDiscoveryTest, RejectsHttpIssuerUrl) {
    EXPECT_THROW(evgrpc::auth::DiscoverJwksUri("ftp://example.com"),
                 std::runtime_error);
}
