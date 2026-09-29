#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <sys/stat.h>

#include "support/test_env.hpp"
#include "ymh/skills/workspace_trust.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

TEST(WorkspaceTrust, RoundTripIsGlobalAndCanonical) {
    TempWorkspace directory("workspace_trust");
    const std::filesystem::path store_path = directory.path() / "trusted_workspaces.json";
    WorkspaceTrustStore        trust(store_path);

    EXPECT_FALSE(trust.is_trusted("/tmp/ymh-ws-a"));
    EXPECT_TRUE(trust.trust("/tmp/ymh-ws-a"));
    EXPECT_TRUE(trust.is_trusted("/tmp/ymh-ws-a"));
    EXPECT_TRUE(trust.is_trusted("/tmp/ymh-ws-a/"));
    EXPECT_FALSE(trust.is_trusted("/tmp/ymh-ws-b"));

    EXPECT_TRUE(trust.untrust("/tmp/ymh-ws-a"));
    EXPECT_FALSE(trust.is_trusted("/tmp/ymh-ws-a"));
}

TEST(WorkspaceTrust, StoreFileIsOwnerOnly) {
    TempWorkspace directory("workspace_trust_mode");
    const std::filesystem::path store_path = directory.path() / "trusted_workspaces.json";
    WorkspaceTrustStore        trust(store_path);
    ASSERT_TRUE(trust.trust("/tmp/ymh-ws-c"));

    struct stat info {};
    ASSERT_EQ(::stat(store_path.c_str(), &info), 0);
    EXPECT_EQ(info.st_mode & 077, 0);
}

TEST(WorkspaceTrust, CorruptStoreIsTreatedAsEmpty) {
    TempWorkspace directory("workspace_trust_corrupt");
    const std::filesystem::path store_path = directory.path() / "trusted_workspaces.json";
    std::ofstream(store_path) << "{not json";
    WorkspaceTrustStore trust(store_path);
    EXPECT_FALSE(trust.is_trusted("/tmp/ymh-ws-d"));
}

TEST(WorkspaceTrust, DefaultTrustCoversLaunchDirAndConfigRootAndFailsClosed) {
    TempWorkspace directory("workspace_trust_default");
    const std::filesystem::path store_path = directory.path() / "trusted_workspaces.json";
    WorkspaceTrustStore        store(store_path);
    const std::filesystem::path launch    = directory.path() / "launch";
    const std::filesystem::path cfg_root  = directory.path() / "config";
    const std::filesystem::path elsewhere = directory.path() / "elsewhere";

    EXPECT_TRUE(workspace_tier_trusted(launch, launch, cfg_root, store));
    EXPECT_TRUE(workspace_tier_trusted(cfg_root, launch, cfg_root, store));
    EXPECT_FALSE(workspace_tier_trusted(elsewhere, launch, cfg_root, store));

    ASSERT_TRUE(store.trust(elsewhere));
    EXPECT_TRUE(workspace_tier_trusted(elsewhere, launch, cfg_root, store));
}

} // namespace
