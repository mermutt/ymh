// Spec 89 (rev 3) sec 13: the 20-item regression corpus. It is a table-driven
// census over the sec 4.6 destructive-verb catalogue and the SG-F1..SG-F27
// failure modes -- a regression net, NOT a defense claim (89-D1: L1 is
// advisory; the boundary is L2, 89-D14). Each row pins the L1 outcome with L2
// on and (where the coupling changes it) with L2 off.

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/policy/shell_guard.hpp"

namespace {

using namespace ymh;

struct CensusRow {
    std::string_view command;
    ShellCommandRisk l2_on;
    ShellCommandRisk l2_off;
};

// 20 rows. `l2_off` differs from `l2_on` exactly where the coupling (89-D14)
// promotes an in-root-bounded `None` to `Confirm` (fail-to-ASK).
const std::vector<CensusRow>& census() {
    static const std::vector<CensusRow> rows = {
        // -- outside-root / device / catastrophe: Forbid regardless of L2 ----
        {"rm -fr build ~", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},   // SG-F1
        {"rm -rf /", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},          // SG-F2
        {"rm -rf $HOME", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},      // SG-F3
        {"rm -rf ../x", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},       // SG-F4
        {"dd if=/dev/zero of=/dev/sda", ShellCommandRisk::Forbid,
         ShellCommandRisk::Forbid},                                                // SG-F6
        {"mkfs.ext4 /dev/sda", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},// SG-F7
        {":(){ :|:& };:", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},     // SG-F8
        {"chmod -R 000 /etc", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid}, // SG-F9
        {"sudo rm -rf ~", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},     // SG-F10
        {"env X=1 rm -rf ~", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},  // SG-F10
        {"bash -c 'rm -rf ~'", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},// SG-F12
        {"echo $(rm -rf ~)", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},  // SG-F13
        {"mv x /outside", ShellCommandRisk::Forbid, ShellCommandRisk::Forbid},     // SG-F17
        // -- in-root unbounded / ambiguous: Confirm regardless of L2 ---------
        {"rm -rf *", ShellCommandRisk::Confirm, ShellCommandRisk::Confirm},        // SG-F5
        {"dd if=/dev/zero of=out.img", ShellCommandRisk::Confirm,
         ShellCommandRisk::Confirm},                                              // dd in-root
        // -- in-root bounded literal: None with L2 on, Confirm with L2 off --
        {"rm -rf build", ShellCommandRisk::None, ShellCommandRisk::Confirm},       // SG-F16
        {"rm -f build/*.o", ShellCommandRisk::None, ShellCommandRisk::Confirm},    // narrow glob
        {"rmdir build", ShellCommandRisk::None, ShellCommandRisk::Confirm},
        {"truncate -s 0 build", ShellCommandRisk::None, ShellCommandRisk::Confirm},
        {"chown -R u build", ShellCommandRisk::None, ShellCommandRisk::Confirm},
    };
    return rows;
}

TEST(Errata89Corpus, TwentyItemCensusWithL2On) {
    test::TempWorkspace workspace("errata89_corpus_on");
    for (const CensusRow& row : census()) {
        SCOPED_TRACE(std::string{row.command});
        EXPECT_EQ(classify_shell_command(row.command, workspace.path(), workspace.path(), true),
                  row.l2_on);
    }
}

TEST(Errata89Corpus, TwentyItemCensusWithL2Off) {
    test::TempWorkspace workspace("errata89_corpus_off");
    for (const CensusRow& row : census()) {
        SCOPED_TRACE(std::string{row.command});
        EXPECT_EQ(classify_shell_command(row.command, workspace.path(), workspace.path(), false),
                  row.l2_off);
    }
}

// Positive controls: non-destructive work stays quiet with and without L2.
TEST(Errata89Corpus, NonDestructiveCommandsAreNone) {
    test::TempWorkspace workspace("errata89_corpus_quiet");
    for (const std::string_view command : {"mkdir build", "echo hi", "ls -la", "git status"}) {
        SCOPED_TRACE(std::string{command});
        EXPECT_EQ(classify_shell_command(command, workspace.path(), workspace.path(), true),
                  ShellCommandRisk::None);
        EXPECT_EQ(classify_shell_command(command, workspace.path(), workspace.path(), false),
                  ShellCommandRisk::None);
    }
}

} // namespace
