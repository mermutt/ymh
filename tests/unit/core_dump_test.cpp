#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <sys/resource.h>

#include "support/short_temp.hpp"
#include "ymh/host/core_dump.hpp"

namespace {

using namespace ymh;

constexpr rlim_t kKB = 1024;
constexpr rlim_t kMB = 1024 * 1024;

rlim_t mb(std::uint64_t value) {
    return static_cast<rlim_t>(value * kMB);
}

TEST(CoreDumpPlan, TruthTableT1) {
    {
        const CoreDumpPlan plan = plan_core_dump_raise(0, 0);
        EXPECT_FALSE(plan.apply);
        EXPECT_EQ(plan.outcome, CoreDumpOutcome::HardLimitZero);
    }
    for (const rlim_t hard : {mb(32), mb(64), static_cast<rlim_t>(RLIM_INFINITY)}) {
        const CoreDumpPlan plan = plan_core_dump_raise(0, hard);
        EXPECT_TRUE(plan.apply);
        EXPECT_EQ(plan.soft, hard);
        EXPECT_EQ(plan.hard, hard);
        EXPECT_EQ(plan.outcome, CoreDumpOutcome::RaiseToHard);
    }
    {
        const CoreDumpPlan plan = plan_core_dump_raise(mb(32), mb(32));
        EXPECT_FALSE(plan.apply);
        EXPECT_EQ(plan.outcome, CoreDumpOutcome::AlreadyAtCeiling);
    }
    {
        const CoreDumpPlan plan = plan_core_dump_raise(mb(64), mb(32));
        EXPECT_FALSE(plan.apply);
        EXPECT_EQ(plan.soft, mb(64));
        EXPECT_EQ(plan.outcome, CoreDumpOutcome::AlreadyAtCeiling);
    }
    {
        const CoreDumpPlan plan = plan_core_dump_raise(mb(1), mb(32));
        EXPECT_TRUE(plan.apply);
        EXPECT_EQ(plan.soft, mb(32));
        EXPECT_EQ(plan.outcome, CoreDumpOutcome::RaiseToHard);
    }
    {
        const CoreDumpPlan plan =
            plan_core_dump_raise(static_cast<rlim_t>(RLIM_INFINITY),
                                 static_cast<rlim_t>(RLIM_INFINITY));
        EXPECT_FALSE(plan.apply);
        EXPECT_EQ(plan.outcome, CoreDumpOutcome::AlreadyAtCeiling);
    }
}

TEST(CoreDumpPlan, RejectedPatchRegressionsT2) {
    const CoreDumpPlan plan = plan_core_dump_raise(0, mb(32));
    EXPECT_EQ(plan.soft, mb(32));
    EXPECT_NE(plan.soft, mb(64));
    EXPECT_TRUE(plan.apply);
}

TEST(CoreDumpPlan, IsPureT3) {
    struct rlimit before {};
    ASSERT_EQ(::getrlimit(RLIMIT_CORE, &before), 0);
    const CoreDumpPlan first = plan_core_dump_raise(mb(1), mb(32));
    const CoreDumpPlan second = plan_core_dump_raise(mb(1), mb(32));
    EXPECT_EQ(first.apply, second.apply);
    EXPECT_EQ(first.soft, second.soft);
    EXPECT_EQ(first.hard, second.hard);
    EXPECT_EQ(first.outcome, second.outcome);
    struct rlimit after {};
    ASSERT_EQ(::getrlimit(RLIMIT_CORE, &after), 0);
    EXPECT_EQ(before.rlim_cur, after.rlim_cur);
    EXPECT_EQ(before.rlim_max, after.rlim_max);
}

TEST(CoreDumpPattern, EmptyAndUnreadableAreUnknownT12) {
    ymh::test::ShortTempRoot root("ymh-e92-pattern");
    const std::filesystem::path empty = root.path() / "empty";
    std::ofstream(empty, std::ios::binary).close();
    EXPECT_EQ(read_core_pattern(empty), "unknown");
    EXPECT_EQ(read_core_pattern(root.path() / "missing"), "unknown");
}

TEST(CoreDumpPattern, PipePatternPreservedT12) {
    ymh::test::ShortTempRoot root("ymh-e92-pipe");
    const std::filesystem::path file = root.path() / "pattern";
    std::ofstream(file, std::ios::binary) << "|/handler %P\n";
    EXPECT_EQ(read_core_pattern(file), "|/handler %P");
}

TEST(CoreDumpSanitize, NonPrintableBecomesQuestionMarkT12) {
    EXPECT_EQ(sanitize_diagnostic_field(std::string{"a\nb\tc\x1b[31m"}, 64), "a?b?c?[31m");
    EXPECT_EQ(sanitize_diagnostic_field("caf\xC3\xA9", 64), "caf??");
}

TEST(CoreDumpSanitize, CapsAndEllipsisT12) {
    const std::string long_value(200, 'x');
    const std::string sanitized = sanitize_diagnostic_field(long_value, 140);
    EXPECT_EQ(sanitized.size(), 143u);
    EXPECT_EQ(sanitized.substr(140), "...");
    const std::string pattern = sanitize_diagnostic_field(long_value, 100);
    EXPECT_EQ(pattern.size(), 103u);
    EXPECT_EQ(pattern.substr(100), "...");
}

TEST(CoreDumpSanitize, OnlyPrintableAsciiT12) {
    const std::string value = sanitize_diagnostic_field(
        std::string{"ab\x01\x02\x7f\xC3\xA9" "cd"}, 64);
    for (const char raw : value) {
        const auto byte = static_cast<unsigned char>(raw);
        EXPECT_GE(byte, 0x20);
        EXPECT_LE(byte, 0x7e);
    }
}

TEST(CoreDumpInternalError, FixedLiteralT14) {
    const std::string text{kCoreDumpInternalErrorLine};
    EXPECT_EQ(text,
              "ymh --host: core dumps: internal error; no limit change "
              "privacy=core-may-contain-prompts-tool-output-secrets\n");
    EXPECT_EQ(text.back(), '\n');
    EXPECT_NE(text.find("privacy="), std::string::npos);
    EXPECT_EQ(text.find("dir="), std::string::npos);
    EXPECT_EQ(text.find("dir_status="), std::string::npos);
    EXPECT_EQ(text.find("pattern="), std::string::npos);
    EXPECT_EQ(text.find("soft="), std::string::npos);
    EXPECT_EQ(text.find("hard="), std::string::npos);
    EXPECT_LE(text.size(), 512u);
}

TEST(CoreDumpPolicy, DisabledEmitsNothingAndCreatesNoDirectory) {
    ymh::test::ShortTempRoot root("ymh-e92-disabled");
    EXPECT_EQ(apply_core_dump_policy(false, root.path(), nullptr),
              CoreDumpOutcome::Disabled);
    EXPECT_FALSE(std::filesystem::exists(root.path() / ".ymh" / "coredumps"));
}

} // namespace
