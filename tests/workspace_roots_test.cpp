// Multi-root workspace support: root validation, the shared registry, and the
// execution layer (resolve/fs/glob) honoring additional roots.
//
// The scratch trees live under $HOME (not /tmp) because
// `validate_workspace_root` deliberately refuses directories inside the
// canonical /tmp scratch area; if $HOME is unavailable the validation tests
// skip.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "test_support.hpp"

#include "ymh/execution/environment.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/filesystem.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/execution/workspace_roots.hpp"

namespace {

using namespace ymh;
using ymh_test::fresh_uuid;

std::filesystem::path canonical_or_lexical(const std::filesystem::path& path) {
    std::error_code       ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
    return ec ? path.lexically_normal() : canonical;
}

std::filesystem::path scratch_base() {
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return canonical_or_lexical(std::filesystem::path{home});
    }
    return std::filesystem::temp_directory_path();
}

// A removable directory tree outside the canonical /tmp scratch area (best
// effort: the validation tests skip when the base itself is inside /tmp).
struct ScratchTree {
    std::filesystem::path root;

    explicit ScratchTree(std::string_view label) {
        root = scratch_base() / ("ymh-workspace-roots-" + std::string{label} + "-" +
                                 std::to_string(::getpid()) + "-" + fresh_uuid());
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
        std::filesystem::create_directories(root, ec);
    }

    ~ScratchTree() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    ScratchTree(const ScratchTree&)            = delete;
    ScratchTree& operator=(const ScratchTree&) = delete;

    [[nodiscard]] std::filesystem::path dir(std::string_view name) const {
        const std::filesystem::path path = root / std::string{name};
        std::filesystem::create_directories(path);
        return path;
    }

    [[nodiscard]] bool inside_canonical_temp() const {
        const std::filesystem::path& temp = canonical_tmp_root();
        if (temp.empty()) {
            return false;
        }
        const std::filesystem::path canonical = canonical_or_lexical(root);
        return canonical == temp || path_is_under_tmp_root(temp, canonical);
    }
};

// Temporarily overrides $HOME so the "refusing the home directory" rule can be
// exercised without touching the real one.
class ScopedHome {
public:
    explicit ScopedHome(const std::filesystem::path& value) {
        if (const char* previous = std::getenv("HOME"); previous != nullptr) {
            previous_ = previous;
        }
        ::setenv("HOME", value.c_str(), 1);
    }

    ~ScopedHome() {
        if (previous_.has_value()) {
            ::setenv("HOME", previous_->c_str(), 1);
        } else {
            ::unsetenv("HOME");
        }
    }

    ScopedHome(const ScopedHome&)            = delete;
    ScopedHome& operator=(const ScopedHome&) = delete;

private:
    std::optional<std::string> previous_;
};

void expect_path_escape(const ExecutionEnvironment& environment,
                        const std::filesystem::path& path) {
    try {
        (void)environment.resolve(path.string());
        FAIL() << "resolve accepted " << path;
    } catch (const ToolError& error) {
        EXPECT_EQ(error.code(), ToolErrorCode::PathEscape) << path;
    }
}

struct TwoRoots {
    ScratchTree                     tree{"environment"};
    std::filesystem::path           primary = tree.dir("primary");
    std::filesystem::path           extra   = tree.dir("extra");
    std::shared_ptr<WorkspaceRoots> roots   = std::make_shared<WorkspaceRoots>(primary);

    TwoRoots() {
        const auto added = roots->add(extra);
        if (!added.has_value()) {
            throw std::runtime_error("cannot register the extra root: " + added.error());
        }
    }

    [[nodiscard]] LocalEnvironment environment(
        SandboxMode mode = SandboxMode::WorkspaceWrite) const {
        return LocalEnvironment{primary, mode, ToolConfig{}, nullptr, {}, roots};
    }
};

} // namespace

TEST(WorkspaceRoots, ValidateAcceptsASiblingDirectory) {
    ScratchTree tree{"validate-accept"};
    if (tree.inside_canonical_temp()) {
        GTEST_SKIP() << "scratch root is inside the canonical /tmp area";
    }
    const std::filesystem::path primary = tree.dir("primary");
    const std::filesystem::path sibling = tree.dir("sibling");

    const auto validated = validate_workspace_root(sibling, primary, {});
    ASSERT_TRUE(validated.has_value()) << validated.error();
    EXPECT_EQ(*validated, std::filesystem::canonical(sibling));
}

TEST(WorkspaceRoots, ValidateRejectsUnsafeDirectories) {
    ScratchTree tree{"validate-reject"};
    if (tree.inside_canonical_temp()) {
        GTEST_SKIP() << "scratch root is inside the canonical /tmp area";
    }
    const std::filesystem::path primary = tree.dir("primary");
    const std::filesystem::path extra   = tree.dir("extra");

    EXPECT_FALSE(validate_workspace_root("/", primary, {}).has_value());
    EXPECT_FALSE(validate_workspace_root(primary / "missing", primary, {}).has_value());
    // The workspace root itself and anything inside it.
    EXPECT_FALSE(validate_workspace_root(primary, primary, {}).has_value());
    EXPECT_FALSE(validate_workspace_root(tree.dir("primary/inner"), primary, {}).has_value());
    // An ancestor of the primary root would widen the workspace.
    EXPECT_FALSE(validate_workspace_root(tree.root, primary, {}).has_value());
    // A duplicate of an already approved root.
    EXPECT_FALSE(validate_workspace_root(extra, primary, {extra}).has_value());

    if (!canonical_tmp_root().empty()) {
        const std::filesystem::path temp_dir =
            canonical_tmp_root() / ("ymh-workspace-roots-tmp-" + fresh_uuid());
        std::error_code ec;
        std::filesystem::create_directories(temp_dir, ec);
        EXPECT_FALSE(validate_workspace_root(temp_dir, primary, {}).has_value());
        std::filesystem::remove_all(temp_dir, ec);
    }
}

TEST(WorkspaceRoots, ValidateRejectsTheHomeDirectory) {
    ScratchTree tree{"validate-home"};
    if (tree.inside_canonical_temp()) {
        GTEST_SKIP() << "scratch root is inside the canonical /tmp area";
    }
    const std::filesystem::path primary   = tree.dir("primary");
    const std::filesystem::path fake_home = tree.dir("fake-home");

    const ScopedHome home{fake_home};
    const auto       validated = validate_workspace_root(fake_home, primary, {});
    ASSERT_FALSE(validated.has_value());
    EXPECT_NE(validated.error().find("home"), std::string::npos) << validated.error();
}

TEST(WorkspaceRoots, AddReturnsTheCanonicalPathAndQueriesAreComponentWise) {
    ScratchTree tree{"registry"};
    const std::filesystem::path primary = tree.dir("primary");
    const std::filesystem::path extra   = tree.dir("extra");

    WorkspaceRoots roots{primary};
    EXPECT_EQ(roots.all().size(), 1u);
    EXPECT_EQ(roots.primary(), std::filesystem::canonical(primary));

    const auto added = roots.add(extra);
    ASSERT_TRUE(added.has_value()) << added.error();
    EXPECT_EQ(*added, std::filesystem::canonical(extra));

    const std::vector<std::filesystem::path> all = roots.all();
    ASSERT_EQ(all.size(), 2u);
    EXPECT_EQ(all.front(), std::filesystem::canonical(primary));
    EXPECT_EQ(all.back(), std::filesystem::canonical(extra));

    EXPECT_TRUE(roots.contains(extra / "src" / "main.cpp"));
    EXPECT_FALSE(roots.contains(tree.root / "outside.txt"));
    const auto relative = roots.relative(extra / "src" / "main.cpp");
    ASSERT_TRUE(relative.has_value());
    EXPECT_EQ(*relative, std::filesystem::path{"src/main.cpp"});
    EXPECT_FALSE(roots.relative(tree.root / "outside.txt").has_value());

    // Duplicate / widening registrations are refused.
    EXPECT_FALSE(roots.add(extra).has_value());
    EXPECT_FALSE(roots.add(primary).has_value());
    EXPECT_FALSE(roots.add(tree.root).has_value());

    // Component-wise, never a string-prefix: /a/bc is not under /a/b.
    const std::filesystem::path narrow = tree.root / "a" / "b";
    std::error_code            ec;
    std::filesystem::create_directories(narrow, ec);
    EXPECT_TRUE(path_is_under_root(primary, primary));
    EXPECT_TRUE(path_is_under_root(narrow, narrow / "file.txt"));
    EXPECT_FALSE(path_is_under_root(narrow, tree.root / "a" / "bc"));
    const WorkspaceRoots narrow_roots{narrow};
    EXPECT_FALSE(narrow_roots.contains(tree.root / "a" / "bc" / "file.txt"));
    EXPECT_FALSE(narrow_roots.relative(tree.root / "a" / "bc" / "file.txt").has_value());
}

TEST(MultiRootEnvironment, ResolveAcceptsPathsUnderAnExtraRoot) {
    const TwoRoots fixture;
    LocalEnvironment environment = fixture.environment();

    EXPECT_EQ(environment.roots().size(), 2u);
    EXPECT_EQ(environment.roots().front(), std::filesystem::canonical(fixture.primary));
    EXPECT_EQ(environment.roots().back(), std::filesystem::canonical(fixture.extra));
    EXPECT_TRUE(environment.within_roots(fixture.extra / "src" / "main.cpp"));

    const std::filesystem::path relative =
        environment.relative_to_root(fixture.extra / "src" / "main.cpp").value_or(
            std::filesystem::path{});
    EXPECT_EQ(relative, std::filesystem::path{"src/main.cpp"});

    const std::filesystem::path target = fixture.extra / "src" / "main.cpp";
    EXPECT_EQ(environment.resolve(target.string()),
              std::filesystem::weakly_canonical(target));
}

TEST(MultiRootEnvironment, ResolveRejectsEscapesAndSharedPrefixSiblings) {
    const TwoRoots fixture;
    LocalEnvironment environment = fixture.environment();

    expect_path_escape(environment, fixture.extra / ".." / "outside.txt");

    const std::filesystem::path sibling = fixture.tree.dir("extra-sibling");
    expect_path_escape(environment, sibling / "escape.txt");
    expect_path_escape(environment, fixture.primary / ".." / "outside.txt");
}

TEST(MultiRootEnvironment, SymlinkCrossingRootsIsRejected) {
    const TwoRoots        fixture;
    LocalEnvironment environment = fixture.environment();

    const std::filesystem::path link = fixture.extra / "into-primary";
    std::error_code            ec;
    std::filesystem::create_directory_symlink(fixture.primary, link, ec);
    ASSERT_FALSE(ec) << ec.message();

    expect_path_escape(environment, link / "file.txt");

    // A symlink from the primary root into the extra root is equally an escape
    // of the root the lexical path names.
    const std::filesystem::path other = fixture.primary / "into-extra";
    std::filesystem::create_directory_symlink(fixture.extra, other, ec);
    ASSERT_FALSE(ec) << ec.message();
    expect_path_escape(environment, other / "file.txt");
}

TEST(MultiRootEnvironment, TempScratchAreaBehaviorIsUnchanged) {
    if (canonical_tmp_root().empty()) {
        GTEST_SKIP() << "/tmp is unavailable";
    }
    const TwoRoots fixture;

    const std::filesystem::path scratch =
        canonical_tmp_root() / ("ymh-workspace-roots-scratch-" + fresh_uuid()) / "note.txt";

    LocalEnvironment writable = fixture.environment(SandboxMode::WorkspaceWrite);
    EXPECT_NO_THROW((void)writable.resolve(scratch.string()));
    // Still not a workspace root, so the file service must not treat it as one
    // unless the WorkspaceWrite scratch area applies.
    EXPECT_FALSE(writable.within_roots(scratch));

    LocalEnvironment readonly = fixture.environment(SandboxMode::ReadOnly);
    EXPECT_THROW((void)readonly.resolve(scratch.string()), ToolError);
}

TEST(MultiRootFilesystem, ReadsWritesGlobsAndProtectsGitUnderAnExtraRoot) {
    const TwoRoots        fixture;
    LocalEnvironment environment = fixture.environment();

    const std::filesystem::path file = environment.resolve(
        (fixture.extra / "src" / "note.txt").string());
    environment.fs().write(file, Data{"hello extra root\n"}).get();
    ASSERT_TRUE(std::filesystem::is_regular_file(file));

    const FileData read = environment.fs().read(file).get();
    EXPECT_EQ(read.bytes, "hello extra root\n");

    // The glob pattern is matched root-relative: the extra root is "src", not
    // the workspace root.
    GlobQuery query;
    query.base    = environment.resolve((fixture.extra / "src").string());
    query.pattern = "src/*.txt";
    const std::vector<std::filesystem::path> files = environment.fs().glob(query).get();
    EXPECT_NE(std::find(files.begin(), files.end(), file), files.end());

    // `.git` under the extra root is never traversed.
    const std::filesystem::path git_dir = fixture.extra / ".git";
    std::filesystem::create_directories(git_dir);
    {
        std::ofstream config{git_dir / "config"};
        config << "secret\n";
    }
    GlobQuery everything;
    everything.base    = fixture.extra;
    everything.pattern = "**/*";
    const std::vector<std::filesystem::path> all = environment.fs().glob(everything).get();
    EXPECT_NE(std::find(all.begin(), all.end(), file), all.end());
    for (const std::filesystem::path& entry : all) {
        bool traverses_git = false;
        for (const std::filesystem::path& component :
             entry.lexically_relative(fixture.extra)) {
            if (component == ".git") {
                traverses_git = true;
            }
        }
        EXPECT_FALSE(traverses_git) << "traversed a protected entry: " << entry;
    }

    GlobQuery protected_query;
    protected_query.base    = git_dir;
    protected_query.pattern = "*";
    EXPECT_THROW((void)environment.fs().glob(protected_query).get(), ToolError);
}
