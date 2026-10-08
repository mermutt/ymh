#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include "support/test_env.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/session/checkpoints.hpp"

namespace {

using namespace ymh;

const SessionId kSession{"s-int"};

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

std::filesystem::path blob_path(const std::filesystem::path& root,
                                const std::string&           bytes) {
    const std::string id = checkpoint_blob_id(bytes);
    return root / ".ymh" / "checkpoints" / "blobs" / id.substr(0, 2) / id;
}

}

TEST(CheckpointsIntegration, CP_I1_CaptureThenRestoreCode) {
    ymh::test::TempWorkspace workspace("cp_i1");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("a.txt", "original");
    store.capture(kSession, TurnId{1}, {std::filesystem::path{"a.txt"}}, env);
    workspace.write("a.txt", "agent edited");
    workspace.write("untracked.txt", "other");

    const CheckpointRestoreReport report = store.restore(env, kSession, TurnId{1});
    EXPECT_EQ(report.restored, 1);
    EXPECT_EQ(read_text(workspace.path() / "a.txt"), "original");
    EXPECT_TRUE(std::filesystem::exists(workspace.path() / "untracked.txt"));
}

TEST(CheckpointsIntegration, CP_I3_ShellEditNotRestored) {
    ymh::test::TempWorkspace workspace("cp_i3");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("tracked.txt", "pre");
    store.capture(kSession, TurnId{1}, {std::filesystem::path{"tracked.txt"}}, env);
    workspace.write("tracked.txt", "post");
    workspace.write("shell.txt", "created by shell");

    const CheckpointRestoreReport report = store.restore(env, kSession, TurnId{1});
    EXPECT_EQ(report.restored, 1);
    EXPECT_EQ(read_text(workspace.path() / "tracked.txt"), "pre");
    EXPECT_EQ(read_text(workspace.path() / "shell.txt"), "created by shell");
}

TEST(CheckpointsIntegration, CP_I4_RemoveSessionGc) {
    ymh::test::TempWorkspace workspace("cp_i4");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("a.txt", "alpha");
    workspace.write("b.txt", "beta");
    store.capture(kSession, TurnId{1},
                  {std::filesystem::path{"a.txt"}, std::filesystem::path{"b.txt"}}, env);
    EXPECT_EQ(count_files(env.root() / ".ymh" / "checkpoints" / "blobs"), 2u);

    store.removeSession(kSession);
    EXPECT_EQ(store.revertible_count(kSession, TurnId{1}), 0);
    EXPECT_EQ(count_files(env.root() / ".ymh" / "checkpoints" / "blobs"), 0u);

    workspace.write("c.txt", "gamma");
    store.capture(kSession, TurnId{2}, {std::filesystem::path{"c.txt"}}, env);
    store.sweep({}, std::chrono::system_clock::now());
    EXPECT_EQ(store.revertible_count(kSession, TurnId{2}), 0);
    EXPECT_EQ(count_files(env.root() / ".ymh" / "checkpoints" / "blobs"), 0u);
}

TEST(CheckpointsIntegration, CP_I5_RetentionAcrossResume) {
    ymh::test::TempWorkspace workspace("cp_i5");
    LocalEnvironment         env(workspace.path());
    {
        CheckpointStore store(env.root());
        for (int turn = 1; turn <= 101; ++turn) {
            workspace.write("f.txt", std::to_string(turn));
            store.capture(kSession, TurnId{static_cast<TurnId>(turn)},
                          {std::filesystem::path{"f.txt"}}, env);
        }
    }

    CheckpointStore resumed(env.root());
    EXPECT_EQ(resumed.revertible_count(kSession, TurnId{1}), 0);
    EXPECT_EQ(resumed.revertible_count(kSession, TurnId{2}), 1);
}

TEST(CheckpointsIntegration, CP_I6_RestorePartialFailure) {
    ymh::test::TempWorkspace workspace("cp_i6");
    LocalEnvironment         env(workspace.path());
    CheckpointStore          store(env.root());

    workspace.write("keep.txt", "keep");
    workspace.write("lost.txt", "lost");
    store.capture(kSession, TurnId{1},
                  {std::filesystem::path{"keep.txt"}, std::filesystem::path{"lost.txt"}}, env);
    workspace.write("keep.txt", "changed");
    workspace.write("lost.txt", "changed");
    std::filesystem::remove(blob_path(env.root(), "lost"));

    const CheckpointRestoreReport report = store.restore(env, kSession, TurnId{1});
    EXPECT_EQ(report.restored, 1);
    EXPECT_EQ(report.failed, 1);
    EXPECT_EQ(read_text(workspace.path() / "keep.txt"), "keep");
}
