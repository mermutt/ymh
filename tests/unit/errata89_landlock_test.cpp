#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <unistd.h>

#if defined(__linux__) && __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#include <sys/syscall.h>
#define YMH_TEST_LANDLOCK 1
#endif

#include "ymh/execution/confinement.hpp"

namespace {

using namespace ymh;

TEST(Errata89Landlock, ClassifyAbiRequiresTruncateAndIoctlDev) {
    EXPECT_EQ(classify_landlock_abi(-ENOSYS), ConfinementStatus::Unavailable);
    EXPECT_EQ(classify_landlock_abi(-EPERM), ConfinementStatus::Unavailable);
    EXPECT_EQ(classify_landlock_abi(-1), ConfinementStatus::Unavailable);
    for (long long abi = 0; abi < 5; ++abi) {
        EXPECT_EQ(classify_landlock_abi(abi), ConfinementStatus::Unavailable) << abi;
    }
    EXPECT_EQ(classify_landlock_abi(5), ConfinementStatus::Enforced);
    EXPECT_EQ(classify_landlock_abi(10), ConfinementStatus::Enforced);
}

TEST(Errata89Landlock, HandledRightsIncludeTruncateAndIoctlDev) {
    const std::uint64_t rights = confinement_handled_rights();
    EXPECT_NE(rights & kLandlockFsTruncate, 0u);
    EXPECT_NE(rights & kLandlockFsIoctlDev, 0u);
    EXPECT_NE(rights & kLandlockFsRefer, 0u);
}

TEST(Errata89Landlock, ProbeNeverReportsNotApplicable) {
    EXPECT_NE(kernel_confinement().probe(), ConfinementStatus::NotApplicable);
}

TEST(Errata89Landlock, ProbeMatchesTheHostAbiContract) {
#if defined(YMH_TEST_LANDLOCK) && defined(SYS_landlock_create_ruleset)
    const long abi = static_cast<long>(
        ::syscall(SYS_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION));
    if (abi < 0) {
        EXPECT_EQ(kernel_confinement().probe(), ConfinementStatus::Unavailable);
        return;
    }
    EXPECT_EQ(kernel_confinement().probe(), classify_landlock_abi(abi));
    if (abi >= 5) {
        EXPECT_EQ(kernel_confinement().probe(), ConfinementStatus::Enforced);
    } else {
        EXPECT_EQ(kernel_confinement().probe(), ConfinementStatus::Unavailable);
    }
#else
    EXPECT_EQ(kernel_confinement().probe(), ConfinementStatus::Unavailable);
#endif
}

} // namespace
