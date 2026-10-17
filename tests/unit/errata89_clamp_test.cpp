#include <gtest/gtest.h>

#include <filesystem>

#include "ymh/cli/wiring.hpp"

namespace {

using namespace ymh;
namespace fs = std::filesystem;

TEST(Errata89Clamp, RefusesRootEqualToHome) {
    EXPECT_TRUE(clamp_or_refuse_root(fs::path{"/home/tester"}, fs::path{"/home/tester"})
                    .has_value());
}

TEST(Errata89Clamp, RefusesRootAncestorOfHome) {
    EXPECT_TRUE(clamp_or_refuse_root(fs::path{"/home"}, fs::path{"/home/tester"}).has_value());
    EXPECT_TRUE(clamp_or_refuse_root(fs::path{"/"}, fs::path{"/home/tester"}).has_value());
}

TEST(Errata89Clamp, RefusesSensitiveSystemPaths) {
    for (const char* path : {"/", "/home", "/etc", "/usr", "/bin", "/var", "/root"}) {
        EXPECT_TRUE(clamp_or_refuse_root(fs::path{path}, fs::path{"/home/tester"}).has_value())
            << path;
    }
}

TEST(Errata89Clamp, AcceptsWorkspaceNestedUnderHome) {
    EXPECT_FALSE(clamp_or_refuse_root(fs::path{"/home/tester/project"},
                                      fs::path{"/home/tester"})
                     .has_value());
    EXPECT_FALSE(clamp_or_refuse_root(fs::path{"/home/tester/project/sub"},
                                      fs::path{"/home/tester"})
                     .has_value());
}

TEST(Errata89Clamp, AcceptsUnrelatedRootAndEmptyHome) {
    EXPECT_FALSE(
        clamp_or_refuse_root(fs::path{"/tmp/ymh-workspace"}, fs::path{"/home/tester"})
            .has_value());
    EXPECT_FALSE(
        clamp_or_refuse_root(fs::path{"/tmp/ymh-workspace"}, fs::path{}).has_value());
}

} // namespace
