#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "ymh/transport/protocol.hpp"

namespace {

using ymh::protocol::classify_hello_identity;
using ymh::protocol::HelloParams;
using ymh::protocol::HelloResult;

HelloResult server(std::string build, std::string digest, std::size_t turns = 0) {
    HelloResult hello;
    hello.server_build  = std::move(build);
    hello.config_digest = std::move(digest);
    hello.active_turns  = turns;
    return hello;
}

TEST(HelloIdentity, PreIdentityServerIsOlder) {
    const auto mismatch = classify_hello_identity(server("", ""), "v261120-10aff020", "abcd");
    EXPECT_TRUE(mismatch.build);
    EXPECT_TRUE(mismatch.server_older);
}

TEST(HelloIdentity, SameBuildDifferentConfigNeedsRestart) {
    const auto mismatch =
        classify_hello_identity(server("v261120-10aff020", "aaaaaaaa"), "v261120-10aff020", "bbbbbbbb");
    EXPECT_FALSE(mismatch.build);
    EXPECT_TRUE(mismatch.config);
    EXPECT_TRUE(mismatch.server_older);
}

TEST(HelloIdentity, OlderClientDoesNotDowngradeANewerDaemon) {
    const auto mismatch =
        classify_hello_identity(server("v261121-abcdef12", "same"), "v261120-10aff020", "same");
    EXPECT_TRUE(mismatch.build);
    EXPECT_FALSE(mismatch.server_older);
}

TEST(HelloIdentity, NewerClientReplacesAnOlderDaemon) {
    const auto mismatch =
        classify_hello_identity(server("v261119-abcdef12", "same"), "v261120-10aff020", "same");
    EXPECT_TRUE(mismatch.build);
    EXPECT_TRUE(mismatch.server_older);
}

TEST(HelloIdentity, SameDateDifferentHashTreatsTheInvokedClientAsFresh) {
    const auto mismatch =
        classify_hello_identity(server("v261120-aaaaaaaa", "same"), "v261120-bbbbbbbb", "same");
    EXPECT_TRUE(mismatch.build);
    EXPECT_TRUE(mismatch.server_older);
}

TEST(HelloIdentity, UnknownDigestNeverForcesARestartByItself) {
    const auto mismatch =
        classify_hello_identity(server("v261120-10aff020", ""), "v261120-10aff020", "abcd");
    EXPECT_FALSE(mismatch.build);
    EXPECT_FALSE(mismatch.config);
    EXPECT_FALSE(mismatch.server_older);
}

TEST(HelloIdentity, IdenticalIdentityIsClean) {
    const auto mismatch =
        classify_hello_identity(server("v261120-10aff020", "abcd"), "v261120-10aff020", "abcd");
    EXPECT_FALSE(mismatch.build);
    EXPECT_FALSE(mismatch.config);
    EXPECT_FALSE(mismatch.server_older);
}

TEST(HelloIdentity, WireRoundTripsAndOldPayloadsStillParse) {
    HelloParams params;
    params.client_build = "v261120-10aff020";
    params.config_digest = "0123456789abcdef";
    nlohmann::json encoded;
    to_json(encoded, params);
    HelloParams decoded;
    from_json(encoded, decoded);
    EXPECT_EQ(decoded.client_build, params.client_build);
    EXPECT_EQ(decoded.config_digest, params.config_digest);

    nlohmann::json old_params = encoded;
    old_params.erase("client_build");
    old_params.erase("config_digest");
    HelloParams legacy_params;
    from_json(old_params, legacy_params);
    EXPECT_TRUE(legacy_params.client_build.empty());
    EXPECT_TRUE(legacy_params.config_digest.empty());

    HelloResult result;
    result.server_build = "v261120-10aff020";
    result.config_digest = "0123456789abcdef";
    result.active_turns = 2;
    nlohmann::json encoded_result;
    to_json(encoded_result, result);
    HelloResult decoded_result;
    from_json(encoded_result, decoded_result);
    EXPECT_EQ(decoded_result.server_build, result.server_build);
    EXPECT_EQ(decoded_result.config_digest, result.config_digest);
    EXPECT_EQ(decoded_result.active_turns, 2U);

    nlohmann::json old_result = encoded_result;
    old_result.erase("server_build");
    old_result.erase("config_digest");
    old_result.erase("active_turns");
    HelloResult legacy_result;
    from_json(old_result, legacy_result);
    EXPECT_TRUE(legacy_result.server_build.empty());
    EXPECT_TRUE(legacy_result.config_digest.empty());
    EXPECT_EQ(legacy_result.active_turns, 0U);
}

} // namespace
