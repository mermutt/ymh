// 86-U1..U11/U13/U14: hermetic coverage for the paste-placeholder chip. The
// supervisor cases replay the exact FTXUI event run a terminal produces for a
// bracketed paste (open marker, body, close marker) through the real
// `handle_event` via the `SupervisorHarness` seam (cf.
// tests/unit/errata66_ui_test.cpp); the pure-model cases drive `InputModel`
// directly.

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <ftxui/component/event.hpp>
#include <nlohmann/json.hpp>

#include "ymh/transport/protocol.hpp"
#include "ymh/ui/supervisor_harness.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const char* kPasteOpen  = "\x1b[200~";
const char* kPasteClose = "\x1b[201~";

const std::string kSixLines = "l1\nl2\nl3\nl4\nl5\nl6";
const std::string kChip1    = "[Pasted text #1 +6 lines]";
const std::string kChip2    = "[Pasted text #2 +4 lines]";
const std::string kFourLines = "a\nb\nc\nd";

AttachIdentity test_identity() {
    return AttachIdentity{protocol::ClientInstanceId{"errata86-ui"},
                          protocol::ClientRole::Supervisor};
}

WorkspaceModel live_workspace(const WorkspaceId& id, const std::string& title) {
    WorkspaceModel workspace;
    workspace.id           = id;
    workspace.title        = title;
    workspace.cwd          = "/" + id.value;
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live         = true;
    return workspace;
}

std::unique_ptr<SupervisorHarness> make_harness() {
    SupervisorRunOptions options;
    options.identity = test_identity();
    return make_supervisor_harness(std::move(options));
}

void paste(SupervisorHarness& harness, const std::string& body) {
    harness.dispatch_event(ftxui::Event::Special(kPasteOpen));
    for (const char character : body) {
        if (character == '\n') {
            harness.dispatch_event(ftxui::Event::Return);
        } else {
            harness.dispatch_event(ftxui::Event::Character(std::string(1, character)));
        }
    }
    harness.dispatch_event(ftxui::Event::Special(kPasteClose));
}

struct Fixture {
    Fixture() : harness(make_harness()) {
        harness->seed_active_workspace(live_workspace(workspace, "alpha"));
        harness->activate_session(workspace, session);
    }

    SessionUiState* state() { return harness->mutable_model().session(session); }
    InputModel&     input() { return state()->input; }

    WorkspaceId                        workspace{"ws-a"};
    SessionId                          session{"s1"};
    std::unique_ptr<SupervisorHarness> harness;
};

// 86-U1.
TEST(Errata86Paste, LongPasteBecomesOneChipAndDoesNotSubmit) {
    Fixture fixture;
    paste(*fixture.harness, kSixLines);
    InputModel& input = fixture.input();
    EXPECT_EQ(input.draft, kChip1);
    ASSERT_EQ(input.paste_chips.size(), 1u);
    const PasteChip& chip = input.paste_chips.at(1);
    EXPECT_EQ(chip.index, 1u);
    EXPECT_EQ(chip.line_count, 6u);
    EXPECT_EQ(chip.text, kSixLines);
    EXPECT_EQ(chip.token, kChip1);
    EXPECT_EQ(fixture.harness->submitted_count("agent.prompt"), 0u);
}

// 86-U2 (66-D4 preserved).
TEST(Errata86Paste, ShortPasteInsertsVerbatim) {
    Fixture fixture;
    paste(*fixture.harness, "line1\nline2");
    EXPECT_EQ(fixture.input().draft, "line1\nline2");
    EXPECT_TRUE(fixture.input().paste_chips.empty());
}

// 86-U3.
TEST(Errata86Paste, BackspaceRemovesWholeChipAndSkipsCaret) {
    Fixture fixture;
    paste(*fixture.harness, kSixLines);
    InputModel& input = fixture.input();
    const std::size_t start = 0;
    const std::size_t end   = kChip1.size();
    ASSERT_EQ(input.cursor, end);

    fixture.harness->dispatch_event(ftxui::Event::ArrowLeft);
    EXPECT_EQ(input.cursor, start);
    fixture.harness->dispatch_event(ftxui::Event::ArrowRight);
    EXPECT_EQ(input.cursor, end);

    fixture.harness->dispatch_event(ftxui::Event::ArrowLeft);
    EXPECT_EQ(input.cursor, start);
    fixture.harness->dispatch_event(ftxui::Event::Backspace);
    EXPECT_TRUE(input.draft.empty());
    EXPECT_TRUE(input.paste_chips.empty());
}

// 86-U4.
TEST(Errata86Paste, DeleteAtChipStartAndAdjacentTie) {
    {
        Fixture fixture;
        paste(*fixture.harness, kSixLines);
        InputModel& input = fixture.input();
        fixture.harness->dispatch_event(ftxui::Event::ArrowLeft);
        ASSERT_EQ(input.cursor, 0u);
        fixture.harness->dispatch_event(ftxui::Event::Delete);
        EXPECT_TRUE(input.draft.empty());
        EXPECT_EQ(input.cursor, 0u);
        EXPECT_TRUE(input.paste_chips.empty());
    }
    {
        Fixture fixture;
        paste(*fixture.harness, kSixLines);
        paste(*fixture.harness, kFourLines);
        InputModel& input = fixture.input();
        const std::size_t t1 = kChip1.size();
        ASSERT_EQ(input.draft, kChip1 + "[Pasted text #2 +4 lines]");

        fixture.harness->dispatch_event(ftxui::Event::ArrowLeft);
        ASSERT_EQ(input.cursor, t1);
        fixture.harness->dispatch_event(ftxui::Event::Backspace);
        EXPECT_EQ(input.draft, "[Pasted text #2 +4 lines]");
        EXPECT_EQ(input.cursor, 0u);
        ASSERT_EQ(input.paste_chips.size(), 1u);
        EXPECT_EQ(input.paste_chips.count(1u), 0u);
        EXPECT_EQ(input.paste_chips.count(2u), 1u);
    }
    {
        Fixture fixture;
        paste(*fixture.harness, kSixLines);
        paste(*fixture.harness, kFourLines);
        InputModel& input = fixture.input();
        const std::size_t t1 = kChip1.size();

        fixture.harness->dispatch_event(ftxui::Event::ArrowLeft);
        ASSERT_EQ(input.cursor, t1);
        fixture.harness->dispatch_event(ftxui::Event::Delete);
        EXPECT_EQ(input.draft, kChip1);
        EXPECT_EQ(input.cursor, t1);
        ASSERT_EQ(input.paste_chips.size(), 1u);
        EXPECT_EQ(input.paste_chips.count(1u), 1u);
        EXPECT_EQ(input.paste_chips.count(2u), 0u);
    }
}

// 86-U5.
TEST(Errata86Paste, SubmitExpandsChipToFullText) {
    Fixture fixture;
    paste(*fixture.harness, kSixLines);
    InputModel& input = fixture.input();
    ASSERT_EQ(input.paste_chips.size(), 1u);

    fixture.harness->dispatch_event(ftxui::Event::Return);

    const std::optional<nlohmann::json> params =
        fixture.harness->last_submitted_params("agent.prompt");
    ASSERT_TRUE(params.has_value());
    ASSERT_TRUE(params->contains("message"));
    const std::string message = (*params)["message"].get<std::string>();
    EXPECT_EQ(message, kSixLines);
    EXPECT_EQ(message.find("[Pasted text"), std::string::npos);
    EXPECT_TRUE(input.paste_chips.empty());
    EXPECT_EQ(input.next_chip_index, 1u);
    EXPECT_TRUE(input.draft.empty());

    ASSERT_TRUE(input.history_up());
    EXPECT_EQ(input.draft, kSixLines);
}

// 86-U6.
TEST(Errata86Paste, NumberingIsStableAndMonotonic) {
    Fixture fixture;
    paste(*fixture.harness, kSixLines);
    paste(*fixture.harness, kFourLines);
    InputModel& input = fixture.input();
    ASSERT_EQ(input.draft, kChip1 + "[Pasted text #2 +4 lines]");

    fixture.harness->dispatch_event(ftxui::Event::Backspace);
    ASSERT_EQ(input.draft, kChip1);
    ASSERT_EQ(input.paste_chips.size(), 1u);

    paste(*fixture.harness, "x\ny\nz\nw");
    EXPECT_EQ(input.draft, kChip1 + "[Pasted text #3 +4 lines]");
    ASSERT_EQ(input.paste_chips.size(), 2u);
    EXPECT_EQ(input.paste_chips.count(1u), 1u);
    EXPECT_EQ(input.paste_chips.count(2u), 0u);
    EXPECT_EQ(input.paste_chips.count(3u), 1u);
}

// 86-U7.
TEST(Errata86Paste, RegistryClearedOnClearLineAndSubjectToCaps) {
    {
        Fixture fixture;
        paste(*fixture.harness, kSixLines);
        ASSERT_EQ(fixture.input().paste_chips.size(), 1u);
        fixture.harness->dispatch_event(ftxui::Event::CtrlU);
        EXPECT_TRUE(fixture.input().draft.empty());
        EXPECT_TRUE(fixture.input().paste_chips.empty());
        EXPECT_EQ(fixture.input().next_chip_index, 1u);
    }
    {
        Fixture fixture;
        InputModel& input = fixture.input();
        (void)input.insert_paste(kSixLines);
        input.draft  = "/exi";
        input.cursor = input.draft.size();
        ASSERT_EQ(input.paste_chips.size(), 1u);

        fixture.harness->dispatch_event(ftxui::Event::Tab);
        EXPECT_EQ(input.draft, "/exit ");
        EXPECT_TRUE(input.paste_chips.empty());
    }
    {
        Fixture fixture;
        InputModel& input = fixture.input();
        for (std::size_t index = 0; index < kMaxPasteChips + 1; ++index) {
            (void)input.insert_paste(kFourLines);
        }
        EXPECT_EQ(input.paste_chips.size(), kMaxPasteChips);
        EXPECT_EQ(input.draft.substr(input.draft.size() - kFourLines.size()), kFourLines);
    }
}

// 86-U8 (58-E8 / 66-I7 preserved).
TEST(Errata86Paste, ReadOnlyChildPasteProducesNoChip) {
    Fixture fixture;
    const SessionId child{"a1b2c3d4-1111-4111-8111-111111111111"};
    fixture.state()->subagents.agents.push_back(
        SubagentView{child, "task", AgentState::Idle, SubagentStatus::Running});
    fixture.harness->mutable_model().ensureSubagentState(fixture.workspace, child);

    ASSERT_TRUE(fixture.harness->dispatch_key("ctrl-t"));
    ASSERT_TRUE(fixture.harness->dispatch_key("down"));
    ASSERT_TRUE(fixture.harness->dispatch_key("enter"));
    fixture.harness->drain_actions();
    ASSERT_EQ(fixture.harness->mutable_model().subagent_path.size(), 1u);

    SessionUiState* viewed = fixture.harness->mutable_model().viewedSession();
    ASSERT_NE(viewed, nullptr);
    const std::string before = viewed->input.draft;
    paste(*fixture.harness, kSixLines);
    EXPECT_EQ(viewed->input.draft, before);
    EXPECT_TRUE(viewed->input.paste_chips.empty());
}

// 86-U9 (86-A2 / 86-F9).
TEST(Errata86Paste, ChipSurvivesModalCloseViaSnapshotRegistry) {
    Fixture fixture;
    fixture.harness->open_switcher();
    paste(*fixture.harness, kSixLines);
    EXPECT_EQ(fixture.input().draft, kChip1);

    fixture.harness->dispatch_event(ftxui::Event::Escape);
    EXPECT_EQ(fixture.input().draft, kChip1);
    EXPECT_EQ(fixture.input().expand_paste_chips(), kSixLines);
}

// 86-U10 (86-D8 / 86-F6).
TEST(Errata86Paste, NonBracketedBurstIsNotChipped) {
    Fixture fixture;
    const std::string burst(600, 'x');
    fixture.harness->dispatch_event(ftxui::Event::Character(burst));
    EXPECT_EQ(fixture.input().draft, burst);
    EXPECT_TRUE(fixture.input().paste_chips.empty());
}

// 86-U11 (PP11).
TEST(Errata86Paste, ExpansionAndMotionArePure) {
    InputModel input;
    (void)input.insert_paste(kSixLines);
    const std::string draft_before = input.draft;
    const std::string text_before  = input.paste_chips.at(1).text;

    (void)input.expand_paste_chips();
    (void)input.expand_paste_chips();
    (void)input.chip_ending_at(0);
    (void)input.chip_ending_at(0);
    (void)input.cursor_left(input.draft.size());
    (void)input.cursor_left(input.draft.size());

    EXPECT_EQ(input.draft, draft_before);
    ASSERT_EQ(input.paste_chips.size(), 1u);
    EXPECT_EQ(input.paste_chips.at(1).text, text_before);
}

// 86-U13 (PP2 proof).
TEST(Errata86Paste, ExpansionLeavesNoChipSpan) {
    const std::string body_a = "A1\nA2\nA3\nA4\nA5\nA6";
    const std::string body_b = "BBBB\nB2\nB3\nB4";
    {
        InputModel input;
        (void)input.insert_paste(body_a);
        (void)input.insert_paste(body_b);
        ASSERT_EQ(input.draft, kChip1 + kChip2);
        const std::string t1 = kChip1;
        const std::string t2 = kChip2;
        input.draft  = t1 + t1 + t2 + t1;
        input.cursor = input.draft.size();

        const std::string result = input.expand_paste_chips();
        EXPECT_EQ(result, body_a + body_a + body_b + body_a);
        EXPECT_EQ(result.find("[Pasted text"), std::string::npos);
    }
    {
        InputModel input;
        (void)input.insert_paste(body_a);
        (void)input.insert_paste(body_b);
        const std::string t2 = kChip2;
        input.paste_chips.at(1).text = "X" + t2 + "Y";

        EXPECT_EQ(input.expand_paste_chips(), "X" + t2 + "Y" + body_b);
    }
    {
        InputModel input;
        (void)input.insert_paste(body_a);
        const std::string inert = "[Pasted text #9 +2 lines]";
        input.draft  = kChip1 + inert;
        input.cursor = input.draft.size();
        EXPECT_EQ(input.expand_paste_chips(), body_a + inert);
    }
}

// 86-U14 (86-A9 / PP13 / 86-F12).
TEST(Errata86Paste, VerticalMotionSnapsOutOfChip) {
    InputModel input;
    (void)input.insert_paste(kSixLines);
    const std::vector<ChipSpan> spans = input.chip_spans();
    ASSERT_EQ(spans.size(), 1u);
    const ChipSpan span = spans.front();

    std::size_t cursor = span.first;
    const bool moved   = composer_move_cursor_vertical(input.draft, cursor, 1, 5, spans);
    EXPECT_TRUE(moved);
    EXPECT_FALSE(span.first < cursor && cursor < span.second);
    EXPECT_TRUE(cursor == span.first || cursor == span.second);

    Fixture fixture;
    InputModel& live = fixture.input();
    (void)live.insert_paste(kSixLines);
    fixture.harness->set_terminal_width(8);
    live.cursor = 0;
    fixture.harness->dispatch_key("down");

    const std::vector<ChipSpan> live_spans = live.chip_spans();
    ASSERT_EQ(live_spans.size(), 1u);
    EXPECT_FALSE(live_spans.front().first < live.cursor &&
                 live.cursor < live_spans.front().second);
}

} // namespace
