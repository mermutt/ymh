// Multi-root persistence in `<workspace>/.ymh/permissions.jsonc`: additional
// approved roots live next to the durable grants, survive a restart, and never
// corrupt each other. The runtime/sandbox wiring itself is covered by the
// integration suites; these are the store-level guarantees.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "ymh/policy/permission_policy.hpp"

namespace {

using namespace ymh;

class TempDir {
public:
    TempDir() {
        static std::uint64_t        counter = 0;
        const std::int64_t          stamp =
            std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("ymh-roots-store-" + std::to_string(stamp) + "-" +
                 std::to_string(++counter));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    TempDir(const TempDir&)            = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

PolicyRule path_grant(std::string id, std::string root = {}) {
    PolicyRule rule;
    rule.tool         = "read_file";
    rule.id           = std::move(id);
    rule.effect       = PolicyVerdict::Allow;
    rule.layer        = PolicyRule::Layer::LocalGrant;
    rule.literal      = true;
    rule.target_class = TargetClass::InRoot;
    rule.root         = std::move(root);
    return rule;
}

void write_document(const std::filesystem::path& file, const std::string& body) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream output{file, std::ios::binary | std::ios::trunc};
    ASSERT_TRUE(output);
    output << body;
    output.close();
    ASSERT_TRUE(output);
}

TEST(WorkspaceRootsStore, AppendRootAndGrantPreserveEachOther) {
    TempDir                     temp;
    const std::filesystem::path file = temp.path() / ".ymh" / "permissions.jsonc";
    const auto                  store = open_file_grant_store(file, std::nullopt);
    ASSERT_NE(store, nullptr);

    ASSERT_TRUE(store->append_root("/abs/one"));
    std::vector<std::filesystem::path> roots = store->load_roots();
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0], std::filesystem::path{"/abs/one"});
    EXPECT_TRUE(store->load().empty());

    ASSERT_TRUE(store->append(path_grant("grant:read_file")));
    const std::vector<PolicyRule> grants = store->load();
    ASSERT_EQ(grants.size(), 1u);
    EXPECT_EQ(grants[0].tool, "read_file");
    // A grant append must not drop the persisted roots.
    roots = store->load_roots();
    ASSERT_EQ(roots.size(), 1u);
    EXPECT_EQ(roots[0], std::filesystem::path{"/abs/one"});

    ASSERT_TRUE(store->append_root("/abs/two"));
    // A root append must not drop the persisted grants.
    EXPECT_EQ(store->load().size(), 1u);
    roots = store->load_roots();
    ASSERT_EQ(roots.size(), 2u);
    EXPECT_EQ(roots[0], std::filesystem::path{"/abs/one"});
    EXPECT_EQ(roots[1], std::filesystem::path{"/abs/two"});

    // Lexically-identical duplicates are not written twice.
    ASSERT_TRUE(store->append_root("/abs/./two"));
    EXPECT_EQ(store->load_roots().size(), 2u);
}

TEST(WorkspaceRootsStore, GrantRootAnchorRoundTrips) {
    TempDir                     temp;
    const std::filesystem::path file = temp.path() / ".ymh" / "permissions.jsonc";
    const auto                  store = open_file_grant_store(file, std::nullopt);

    ASSERT_TRUE(store->append(path_grant("grant:anchored", "/ws1")));
    const std::vector<PolicyRule> grants = store->load();
    ASSERT_EQ(grants.size(), 1u);
    EXPECT_EQ(grants[0].root, "/ws1");

    // Legacy grant without an anchor loads with an empty root.
    ASSERT_TRUE(store->append(path_grant("grant:legacy")));
    const std::vector<PolicyRule> reloaded = store->load();
    ASSERT_EQ(reloaded.size(), 2u);
    bool saw_legacy = false;
    for (const PolicyRule& rule : reloaded) {
        if (rule.id == "grant:legacy") {
            saw_legacy = true;
            EXPECT_TRUE(rule.root.empty());
        }
    }
    EXPECT_TRUE(saw_legacy);
}

TEST(WorkspaceRootsStore, WorkspaceIdMismatchIgnoresEverything) {
    TempDir                     temp;
    const std::filesystem::path file = temp.path() / ".ymh" / "permissions.jsonc";
    {
        const auto writer = open_file_grant_store(file, std::string{"ws-one"});
        ASSERT_TRUE(writer->append_root("/abs/one"));
        ASSERT_TRUE(writer->append(path_grant("grant:read_file")));
    }
    const auto reader = open_file_grant_store(file, std::string{"ws-two"});
    EXPECT_TRUE(reader->load().empty());
    EXPECT_TRUE(reader->load_roots().empty());
}

TEST(WorkspaceRootsStore, MalformedRootsFailSafe) {
    TempDir temp;

    const std::filesystem::path not_array =
        temp.path() / "not-array" / ".ymh" / "permissions.jsonc";
    write_document(not_array,
                   R"({"schema":"ymh.permissions/1","grants":[],"roots":"/abs/one"})");
    {
        const auto store = open_file_grant_store(not_array, std::nullopt);
        EXPECT_TRUE(store->load_roots().empty());
        EXPECT_TRUE(store->load().empty());
    }

    const std::filesystem::path non_string =
        temp.path() / "non-string" / ".ymh" / "permissions.jsonc";
    write_document(non_string,
                   R"({"schema":"ymh.permissions/1","grants":[],"roots":["/abs/one",5]})");
    {
        const auto store = open_file_grant_store(non_string, std::nullopt);
        EXPECT_TRUE(store->load_roots().empty());
    }

    // A non-string grant `root` makes the grant document invalid (fail-safe).
    const std::filesystem::path bad_anchor =
        temp.path() / "bad-anchor" / ".ymh" / "permissions.jsonc";
    write_document(bad_anchor,
                   R"({"schema":"ymh.permissions/1","grants":[{"tool":"read_file",)"
                   R"("match":"literal","effect":"allow","root":5}]})");
    {
        const auto store = open_file_grant_store(bad_anchor, std::nullopt);
        EXPECT_TRUE(store->load().empty());
    }
}

TEST(WorkspaceRootsStore, MissingFileLoadsEmptyRoots) {
    TempDir                     temp;
    const std::filesystem::path file = temp.path() / "never-created" / "permissions.jsonc";
    const auto                  store = open_file_grant_store(file, std::nullopt);
    EXPECT_TRUE(store->load_roots().empty());
    EXPECT_TRUE(store->load().empty());
}

} // namespace
