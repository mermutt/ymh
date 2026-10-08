#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/session/checkpoints.hpp"

namespace {

using namespace ymh;

const SessionId kSession{"s-cp"};

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(input)),
                       std::istreambuf_iterator<char>());
}

std::size_t count_files(const std::filesystem::path& dir) {
    std::error_code ec;
    std::size_t     total = 0;
    if (!std::filesystem::is_directory(dir, ec)) {
        return 0;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.is_regular_file()) {
            ++total;
        }
    }
    return total;
}

std::filesystem::path store_dir(const std::filesystem::path& root) {
    return root / ".ymh" / "checkpoints";
}

std::filesystem::path blob_path(const std::filesystem::path& root,
                                const std::string&           bytes) {
    const std::string id = checkpoint_blob_id(bytes);
    return store_dir(root) / "blobs" / id.substr(0, 2) / id;
}

}

TEST(Checkpoints, CP_U1_CapturePreImageKeyedByTurn) {
    ymh::test::TempWorkspace workspace("cp_u1");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("a.txt", "one");
    store.capture(kSession, TurnId{3}, {std::filesystem::path{"a.txt"}}, env);
    workspace.write("a.txt", "two");
    store.capture(kSession, TurnId{3}, {std::filesystem::path{"a.txt"}}, env);
    workspace.write("a.txt", "three");

    const CheckpointRestoreReport report = store.restore(env, kSession, TurnId{3});
    EXPECT_EQ(report.restored, 1);
    EXPECT_EQ(read_text(workspace.path() / "a.txt"), "one");
}

TEST(Checkpoints, CP_U2_AddedFileRestoresByRemoval) {
    ymh::test::TempWorkspace workspace("cp_u2");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    store.capture(kSession, TurnId{1}, {std::filesystem::path{"new.txt"}}, env);
    workspace.write("new.txt", "created");

    const CheckpointRestoreReport report = store.restore(env, kSession, TurnId{1});
    EXPECT_EQ(report.restored, 1);
    EXPECT_FALSE(std::filesystem::exists(workspace.path() / "new.txt"));
}

TEST(Checkpoints, CP_U3_BlobDedup) {
    ymh::test::TempWorkspace workspace("cp_u3");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("a.txt", "x");
    store.capture(kSession, TurnId{1}, {std::filesystem::path{"a.txt"}}, env);
    workspace.write("a.txt", "y");
    store.capture(kSession, TurnId{2}, {std::filesystem::path{"a.txt"}}, env);
    workspace.write("b.txt", "x");
    store.capture(kSession, TurnId{2}, {std::filesystem::path{"b.txt"}}, env);

    EXPECT_EQ(count_files(store_dir(env.root()) / "blobs"), 2u);
}

TEST(Checkpoints, CP_U4_AbsentAndSymlinkKinds) {
    ymh::test::TempWorkspace workspace("cp_u4");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("target.txt", "payload");
    workspace.write("plain.txt", "plain");
    std::filesystem::create_symlink("target.txt", workspace.path() / "link.txt");
    std::filesystem::create_hard_link(workspace.path() / "plain.txt",
                                      workspace.path() / "hard.txt");

    const std::optional<std::filesystem::path> link_component =
        first_symlink_component(env.root() / "link.txt");
    ASSERT_TRUE(link_component.has_value());
    EXPECT_EQ(*link_component, env.root() / "link.txt");
    EXPECT_TRUE(first_symlink_component(env.root() / "hard.txt").has_value());
    EXPECT_FALSE(first_symlink_component(env.root() / "target.txt").has_value());
    EXPECT_FALSE(first_symlink_component(env.root() / "missing.txt").has_value());

    store.capture(kSession, TurnId{1}, {std::filesystem::path{"link.txt"},
                                        std::filesystem::path{"hard.txt"}}, env);
    const CheckpointRestoreReport report = store.restore(env, kSession, TurnId{1});
    EXPECT_EQ(report.skipped, 2);
    EXPECT_EQ(report.restored, 0);
    EXPECT_EQ(read_text(workspace.path() / "target.txt"), "payload");
}

TEST(Checkpoints, CP_U5_RestoreSetEarliestAtOrAfter) {
    ymh::test::TempWorkspace workspace("cp_u5");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("p.txt", "old");
    store.capture(kSession, TurnId{1}, {std::filesystem::path{"p.txt"}}, env);
    workspace.write("q.txt", "q1");
    store.capture(kSession, TurnId{1}, {std::filesystem::path{"q.txt"}}, env);
    workspace.write("p.txt", "mid");
    store.capture(kSession, TurnId{4}, {std::filesystem::path{"p.txt"}}, env);
    workspace.write("q.txt", "q2");

    const CheckpointRestoreReport report = store.restore(env, kSession, TurnId{3});
    EXPECT_EQ(report.restored, 1);
    EXPECT_EQ(read_text(workspace.path() / "p.txt"), "mid");
    EXPECT_EQ(read_text(workspace.path() / "q.txt"), "q2");
}

TEST(Checkpoints, CP_U6_CountAndAgeEviction) {
    ymh::test::TempWorkspace workspace("cp_u6_count");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    for (int turn = 1; turn <= 101; ++turn) {
        workspace.write("f.txt", std::to_string(turn));
        store.capture(kSession, TurnId{static_cast<TurnId>(turn)},
                      {std::filesystem::path{"f.txt"}}, env);
    }
    EXPECT_EQ(store.revertible_count(kSession, TurnId{1}), 0);
    EXPECT_EQ(store.revertible_count(kSession, TurnId{2}), 1);

    std::set<SessionId> live{kSession};
    store.sweep(live, std::chrono::system_clock::now());
    EXPECT_EQ(count_files(store_dir(env.root()) / "blobs"), 100u);

    ymh::test::TempWorkspace aged("cp_u6_age");
    LocalEnvironment         aged_env(aged.path());
    CheckpointStore          aged_store(aged_env.root());
    aged.write("a.txt", "x");
    aged_store.capture(kSession, TurnId{1}, {std::filesystem::path{"a.txt"}}, aged_env);

    std::set<SessionId> aged_live{kSession};
    aged_store.sweep(aged_live,
                     std::chrono::system_clock::now() + std::chrono::hours{24 * 31});
    EXPECT_EQ(aged_store.revertible_count(kSession, TurnId{1}), 0);
    EXPECT_EQ(count_files(store_dir(aged_env.root()) / "blobs"), 0u);
}

TEST(Checkpoints, CP_U7_IndexCrashSafety) {
    ymh::test::TempWorkspace workspace("cp_u7");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("a.txt", "x");
    store.capture(kSession, TurnId{1}, {std::filesystem::path{"a.txt"}}, env);

    const std::filesystem::path dir = store_dir(env.root());
    std::ofstream(dir / "index.json.tmp.999") << "stale";
    std::filesystem::create_directories(dir / "blobs" / "zz");
    std::ofstream(dir / "blobs" / "zz" / "orphan") << "orphan";

    std::set<SessionId> live{kSession};
    store.sweep(live, std::chrono::system_clock::now());

    EXPECT_FALSE(std::filesystem::exists(dir / "index.json.tmp.999"));
    EXPECT_FALSE(std::filesystem::exists(dir / "blobs" / "zz" / "orphan"));
    EXPECT_EQ(store.revertible_count(kSession, TurnId{1}), 1);
}

TEST(Checkpoints, CP_U8_CorruptIndexQuarantined) {
    ymh::test::TempWorkspace workspace("cp_u8");
    LocalEnvironment         env(workspace.path());
    std::filesystem::create_directories(store_dir(env.root()));
    std::ofstream(store_dir(env.root()) / "index.json") << "{ not json";

    CheckpointStore store(env.root());
    EXPECT_EQ(store.revertible_count(kSession, TurnId{1}), 0);

    store.sweep({}, std::chrono::system_clock::now());
    bool quarantined = false;
    for (const auto& entry : std::filesystem::directory_iterator(store_dir(env.root()))) {
        if (entry.path().filename().string().rfind("index.json.corrupt.", 0) == 0) {
            quarantined = true;
        }
    }
    EXPECT_TRUE(quarantined);

    workspace.write("a.txt", "x");
    store.capture(kSession, TurnId{1}, {std::filesystem::path{"a.txt"}}, env);
    EXPECT_EQ(store.revertible_count(kSession, TurnId{1}), 1);
}

TEST(Checkpoints, CP_U9_RestoreMissingBlobPartial) {
    ymh::test::TempWorkspace workspace("cp_u9");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("a.txt", "alpha");
    workspace.write("b.txt", "beta");
    store.capture(kSession, TurnId{1},
                  {std::filesystem::path{"a.txt"}, std::filesystem::path{"b.txt"}}, env);
    workspace.write("a.txt", "A");
    workspace.write("b.txt", "B");

    std::filesystem::remove(blob_path(env.root(), "alpha"));
    const CheckpointRestoreReport first = store.restore(env, kSession, TurnId{1});
    EXPECT_EQ(first.restored, 1);
    EXPECT_EQ(first.failed, 1);
    EXPECT_EQ(first.changed, 1);
    EXPECT_EQ(read_text(workspace.path() / "b.txt"), "beta");

    std::filesystem::remove(blob_path(env.root(), "beta"));
    workspace.write("a.txt", "A2");
    workspace.write("b.txt", "B2");
    const CheckpointRestoreReport second = store.restore(env, kSession, TurnId{1});
    EXPECT_EQ(second.restored, 0);
    EXPECT_EQ(second.failed, 2);
    EXPECT_EQ(second.detail,
              "No files were restored: 2 files failed (backup missing, or the file "
              "could not be updated)");
}

TEST(Checkpoints, CP_U10_RevertibleCount) {
    ymh::test::TempWorkspace workspace("cp_u10");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("a.txt", "a1");
    store.capture(kSession, TurnId{1}, {std::filesystem::path{"a.txt"}}, env);
    workspace.write("b.txt", "b1");
    store.capture(kSession, TurnId{2}, {std::filesystem::path{"b.txt"}}, env);
    workspace.write("a.txt", "a2");
    store.capture(kSession, TurnId{3}, {std::filesystem::path{"a.txt"}}, env);

    EXPECT_EQ(store.revertible_count(kSession, TurnId{1}), 2);
    EXPECT_EQ(store.revertible_count(kSession, TurnId{2}), 2);
    EXPECT_EQ(store.revertible_count(kSession, TurnId{3}), 1);
    EXPECT_EQ(store.revertible_count(kSession, TurnId{4}), 0);
}

TEST(Checkpoints, CP_U18_ExpiredTargetRefused) {
    ymh::test::TempWorkspace workspace("cp_u18");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    for (int turn = 1; turn <= 101; ++turn) {
        workspace.write("f.txt", std::to_string(turn));
        store.capture(kSession, TurnId{static_cast<TurnId>(turn)},
                      {std::filesystem::path{"f.txt"}}, env);
    }
    workspace.write("f.txt", "current");

    EXPECT_EQ(store.revertible_count(kSession, TurnId{1}), 0);
    const CheckpointRestoreReport report = store.restore(env, kSession, TurnId{1});
    EXPECT_TRUE(report.expired);
    EXPECT_EQ(report.restored, 0);
    EXPECT_EQ(report.detail, "checkpoint expired; code restore unavailable");
    EXPECT_EQ(read_text(workspace.path() / "f.txt"), "current");
}

TEST(Checkpoints, CP_U19_WorkspaceRootVsStoreDir) {
    ymh::test::TempWorkspace workspace("cp_u19");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("sub/a.txt", "x");
    store.capture(kSession, TurnId{1}, {std::filesystem::path{"sub/a.txt"}}, env);

    const std::filesystem::path index = store_dir(env.root()) / "index.json";
    ASSERT_TRUE(std::filesystem::exists(index));
    std::ifstream input(index);
    const nlohmann::json document = nlohmann::json::parse(input);
    EXPECT_EQ(document.at("sessions").at("s-cp").at("checkpoints").at(0)
                  .at("files").at(0).at("path").get<std::string>(),
              "sub/a.txt");
    EXPECT_EQ(workspace_relative(env.root(), env.root() / "z.txt"), "z.txt");
}

TEST(Checkpoints, CP_U20_IndexIoErrorThrows) {
    ymh::test::TempWorkspace workspace("cp_u20_io");
    LocalEnvironment         env(workspace.path());
    std::filesystem::create_directories(store_dir(env.root()) / "index.json");

    CheckpointStore store(env.root());
    EXPECT_EQ(store.revertible_count(kSession, TurnId{1}), 0);
    EXPECT_THROW(store.restore(env, kSession, TurnId{1}), CheckpointUnavailableError);

    ymh::test::TempWorkspace corrupt("cp_u20_corrupt");
    LocalEnvironment         corrupt_env(corrupt.path());
    std::filesystem::create_directories(store_dir(corrupt_env.root()));
    std::ofstream(store_dir(corrupt_env.root()) / "index.json") << "{ this is not json";
    CheckpointStore corrupt_store(corrupt_env.root());
    EXPECT_EQ(corrupt_store.revertible_count(kSession, TurnId{1}), 0);

    CheckpointRestoreReport report;
    EXPECT_NO_THROW({ report = corrupt_store.restore(corrupt_env, kSession, TurnId{1}); });
    EXPECT_FALSE(report.expired);
    EXPECT_EQ(report.detail, "nothing to restore");
}

TEST(Checkpoints, CP_U21_ConstLazyLoad) {
    ymh::test::TempWorkspace workspace("cp_u21");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("a.txt", "x");
    store.capture(kSession, TurnId{1}, {std::filesystem::path{"a.txt"}}, env);

    const CheckpointStore& view = store;
    EXPECT_EQ(view.revertible_count(kSession, TurnId{1}), 1);
    EXPECT_EQ(view.revertible_count(kSession, TurnId{1}), 1);
}

TEST(Checkpoints, CP_U23_SweepNeverThrows) {
    ymh::test::TempWorkspace workspace("cp_u23");
    LocalEnvironment         env(workspace.path());
    std::filesystem::create_directories(store_dir(env.root()) / "index.json");

    CheckpointStore store(env.root());
    EXPECT_NO_THROW(store.sweep({}, std::chrono::system_clock::now()));
    EXPECT_NO_THROW(store.sweep({}, std::chrono::system_clock::now()));
}
