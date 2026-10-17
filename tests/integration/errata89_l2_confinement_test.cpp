#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>

#include "support/test_env.hpp"
#include "ymh/execution/confinement.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/process.hpp"

namespace {

using namespace ymh;
namespace fs = std::filesystem;

TEST(Errata89L2Confinement, ResolvedPathsOutsideRootAreDenied) {
    if (kernel_confinement().probe() != ConfinementStatus::Enforced) {
        GTEST_SKIP() << "Landlock is unavailable on this host";
    }

    test::TempWorkspace workspace("errata89_l2");
    const fs::path      root = fs::canonical(workspace.path());
    const fs::path      base = root / ".spawn";
    fs::create_directories(base / "home");
    fs::create_directories(base / "tmp");

    ConfinementPolicy policy;
    policy.root    = root;
    policy.home    = base / "home";
    policy.tmpdir  = base / "tmp";
    policy.ro_paths = {fs::path{"/"}};

    const fs::path outside_dir = test::make_temp_dir("errata89_l2_outside");
    const fs::path outside     = outside_dir / "victim.txt";
    { std::ofstream(outside, std::ios::binary) << "outside-data"; }
    const fs::path inside = root / "victim.txt";
    { std::ofstream(inside, std::ios::binary) << "inside-data"; }

    OutputRing    ring(8192);
    RingOutputSink sink(ring);

    ProcessRequest request;
    request.executable = "/bin/bash";
    request.argv       = {"/bin/bash", "-c",
                          "rm -f '" + outside.string() + "'; echo unlink_outside_rc=$?; "
                          "printf x > '" + outside.string() + "'; echo write_outside_rc=$?; "
                          "rm -f '" + inside.string() + "'; echo unlink_inside_rc=$?"};
    request.cwd        = root;
    request.sink       = &sink;
    request.confinement = &policy;

    LocalProcessService service;
    const ProcessResult result = service.run(request, {}).get();
    const std::string   output = sink.materialize(8192);

    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(output.find("unlink_outside_rc=0"), std::string::npos) << output;
    EXPECT_EQ(output.find("write_outside_rc=0"), std::string::npos) << output;
    EXPECT_NE(output.find("unlink_inside_rc=0"), std::string::npos) << output;
    EXPECT_TRUE(fs::exists(outside));
    EXPECT_FALSE(fs::exists(inside));

    std::error_code ec;
    fs::remove_all(outside_dir, ec);
}

TEST(Errata89L2Confinement, PolicyWithoutConfinementDoesNotRestrict) {
    test::TempWorkspace workspace("errata89_l2_off");
    const fs::path      root = fs::canonical(workspace.path());
    const fs::path      inside = root / "victim.txt";
    { std::ofstream(inside, std::ios::binary) << "inside-data"; }

    ProcessRequest request;
    request.executable = "/bin/bash";
    request.argv       = {"/bin/bash", "-c", "rm -f '" + inside.string() + "'"};
    request.cwd        = root;
    request.confinement = nullptr;

    LocalProcessService service;
    const ProcessResult result = service.run(request, {}).get();
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_FALSE(fs::exists(inside));
}

} // namespace
