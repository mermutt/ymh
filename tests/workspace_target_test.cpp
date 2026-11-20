#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "test_support.hpp"

#include "ymh/registry/registry.hpp"
#include "ymh/registry/workspace_cli.hpp"

namespace {

using ymh::RegistryConfig;
using ymh::WorkspaceRecord;
using ymh::WorkspaceRegistry;
using ymh::WorkspaceTargetResolution;
using ymh_test::TempWorkspace;

struct RegistryFixture {
    TempWorkspace                          workspace;
    std::unique_ptr<WorkspaceRegistry>     registry;
    WorkspaceRecord                        alpha;
    WorkspaceRecord                        beta;

    RegistryFixture() {
        std::filesystem::create_directories(workspace.root / "alpha");
        std::filesystem::create_directories(workspace.root / "beta");
        std::filesystem::create_directories(workspace.root / "scan");

        RegistryConfig config;
        config.db_path         = workspace.root / "state" / "registry.db";
        config.lock_path       = workspace.root / "state" / "registry.lock";
        config.workspace_roots = {workspace.root / "scan"};
        registry = WorkspaceRegistry::open(config);
        alpha = registry->registerWorkspace(std::filesystem::canonical(workspace.root / "alpha"),
                                            "alpha");
        beta = registry->registerWorkspace(std::filesystem::canonical(workspace.root / "beta"),
                                           "beta");
    }
};

TEST(WorkspaceTarget, ResolvesExactIdPathAndTitle) {
    RegistryFixture fixture;
    for (const std::string& target : {fixture.alpha.id.value,
                                      fixture.alpha.canonicalPath.string(),
                                      std::string{"alpha"}}) {
        const WorkspaceTargetResolution resolution =
            ymh::resolve_workspace_target(*fixture.registry, target);
        ASSERT_TRUE(resolution.resolved.has_value()) << target;
        EXPECT_EQ(resolution.resolved->id.value, fixture.alpha.id.value);
        EXPECT_TRUE(resolution.ambiguous.empty());
    }
}

TEST(WorkspaceTarget, ResolvesAUniqueIdPrefix) {
    RegistryFixture fixture;
    const WorkspaceTargetResolution resolution =
        ymh::resolve_workspace_target(*fixture.registry, fixture.beta.id.value.substr(0, 8));
    ASSERT_TRUE(resolution.resolved.has_value());
    EXPECT_EQ(resolution.resolved->id.value, fixture.beta.id.value);
}

TEST(WorkspaceTarget, ExactTitleBeatsAnAmbiguousPrefix) {
    RegistryFixture fixture;
    std::filesystem::create_directories(fixture.workspace.root / "alpha2");
    (void)fixture.registry->registerWorkspace(
        std::filesystem::canonical(fixture.workspace.root / "alpha2"), "alphabeta");
    const WorkspaceTargetResolution resolution =
        ymh::resolve_workspace_target(*fixture.registry, "alpha");
    ASSERT_TRUE(resolution.resolved.has_value());
    EXPECT_EQ(resolution.resolved->id.value, fixture.alpha.id.value);
}

TEST(WorkspaceTarget, AmbiguousPrefixReportsCandidates) {
    RegistryFixture fixture;
    std::filesystem::create_directories(fixture.workspace.root / "alpha2");
    (void)fixture.registry->registerWorkspace(
        std::filesystem::canonical(fixture.workspace.root / "alpha2"), "alphabeta");
    const WorkspaceTargetResolution resolution =
        ymh::resolve_workspace_target(*fixture.registry, "alph");
    EXPECT_FALSE(resolution.resolved.has_value());
    EXPECT_EQ(resolution.ambiguous.size(), 2U);
}

TEST(WorkspaceTarget, UnknownTargetIsNotResolved) {
    RegistryFixture fixture;
    const WorkspaceTargetResolution resolution =
        ymh::resolve_workspace_target(*fixture.registry, "definitely-not-a-workspace");
    EXPECT_FALSE(resolution.resolved.has_value());
    EXPECT_TRUE(resolution.ambiguous.empty());
}

} // namespace
