// Spec 89 (rev 3) sec 13: the differential canary oracle. A falsifier only,
// NOT a prover (89-D1): for each destructive shell-syntax string the classifier
// rates `None`, run it in a throwaway tree whose writable area contains both
// the workspace root and an outside-root canary, then fail if any outside-root
// sentinel changed. Landlock confines the child to that throwaway tree, so the
// real host is never at risk; without a usable sandbox the test skips.
//
// Interpreter payloads (SG-F22, e.g. `python3 -c ...`) are deliberately not in
// this corpus: the spec records them as the unclaimed L1 residual backed by L2.

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/execution/confinement.hpp"
#include "ymh/execution/process.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using namespace ymh;
namespace fs = std::filesystem;

class CanaryHarness {
public:
    explicit CanaryHarness(const std::string& prefix) : base_(prefix) {
        root_   = base_.path() / "root";
        canary_ = base_.path() / "canary";
        home_   = base_.path() / "home";
        tmp_    = base_.path() / "tmp";
        fs::create_directories(root_ / "build");
        fs::create_directories(root_ / "dist");
        fs::create_directories(canary_);
        fs::create_directories(home_);
        fs::create_directories(tmp_);
        reset();
    }

    void reset() const {
        write(root_ / "build" / "a.o", "object");
        write(root_ / "build" / "secret", "secret");
        write(canary_ / "victim.txt", "outside");
        write(home_ / "home_sentinel.txt", "home");
        write(tmp_ / "tmp_sentinel.txt", "tmp");
    }

    [[nodiscard]] bool outsideIntact() const {
        return fs::exists(canary_ / "victim.txt") && fs::exists(home_ / "home_sentinel.txt") &&
               fs::exists(tmp_ / "tmp_sentinel.txt");
    }

    [[nodiscard]] ProcessResult run(const std::string& command) const {
        ConfinementPolicy policy;
        policy.root    = base_.path();
        policy.home    = home_;
        policy.tmpdir  = tmp_;
        policy.ro_paths = {fs::path{"/"}};

        ProcessRequest request;
        request.executable  = "/bin/bash";
        request.argv        = {"/bin/bash", "--noprofile", "--norc", "-c", command};
        request.cwd         = root_;
        request.env_mode    = ProcessEnvMode::Minimal;
        request.environment = {{"HOME", home_.string()}, {"TMPDIR", tmp_.string()}};
        request.confinement = &policy;

        LocalProcessService service;
        return service.run(request, {}).get();
    }

    [[nodiscard]] const fs::path& root() const noexcept { return root_; }
    [[nodiscard]] const fs::path& canary() const noexcept { return canary_; }

private:
    static void write(const fs::path& path, const std::string& content) {
        std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    }

    test::TempWorkspace base_;
    fs::path            root_;
    fs::path            canary_;
    fs::path            home_;
    fs::path            tmp_;
};

const std::vector<std::string>& destructive_corpus() {
    static const std::vector<std::string> corpus = {
        "rm -fr build ~",
        "rm -rf /",
        "rm -rf $HOME",
        "rm -rf ../x",
        "rm -rf ..",
        "rm -rf *",
        "rm -rf build",
        "rm -f build/*.o",
        "rmdir build",
        "shred build/secret",
        "truncate -s 0 build",
        "chmod -R 000 /etc",
        "chown -R u build",
        "mv build ../outside",
        "cp -r src ../outside",
        "find .. -delete",
        "sudo rm -rf ~",
        "env X=1 rm -rf ~",
        "bash -c 'rm -rf ~'",
        "echo $(rm -rf ~)",
        "> /dev/sda",
        "dd if=/dev/zero of=/dev/sda",
    };
    return corpus;
}

// Sanity: the canary harness really can observe an outside-root effect. This
// bypasses the classifier on purpose; it proves the oracle is not vacuous.
TEST(Errata89Canary, SandboxDetectsAnOutsideRootEffect) {
    if (kernel_confinement().probe() != ConfinementStatus::Enforced) {
        GTEST_SKIP() << "no Landlock sandbox; the canary cannot be made safe here";
    }
    CanaryHarness harness("errata89_canary_sanity");
    const ProcessResult result = harness.run("rm -rf '" + harness.canary().string() + "'");
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(fs::exists(harness.canary() / "victim.txt"));
}

TEST(Errata89Canary, ClassifierNoneStringsHaveNoOutsideRootEffect) {
    if (kernel_confinement().probe() != ConfinementStatus::Enforced) {
        GTEST_SKIP() << "no Landlock sandbox; the canary cannot be made safe here";
    }
    CanaryHarness harness("errata89_canary");

    std::size_t executed = 0;
    std::size_t caught   = 0;
    for (const std::string& command : destructive_corpus()) {
        const ShellCommandRisk risk =
            classify_shell_command(command, harness.root(), harness.root(), true);
        if (risk != ShellCommandRisk::None) {
            ++caught;
            continue;
        }
        SCOPED_TRACE(command);
        harness.reset();
        (void)harness.run(command);
        EXPECT_TRUE(harness.outsideIntact())
            << "L1 rated a destructive command None but it changed an outside-root path";
        ++executed;
    }

    EXPECT_GT(executed, 0u) << "the corpus must exercise at least one L1-None command";
    EXPECT_GT(caught, 0u) << "the corpus must contain destructive commands L1 catches";
}

} // namespace
