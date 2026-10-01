// 60-D14/60-I13/60-U5 (spec 60 Rev 6): the transcript's content height is the
// laid-out (wrapped) row count, so a pane shrink cannot clamp `max_top()` short
// of the tail. These tests drive `build_ui` + geometry application + layout in
// the same order `SupervisorApp`'s frame callback does; a real PTY is not needed
// because the shrink is expressed as a build-size vs render-size pair.

#include <gtest/gtest.h>

#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "ymh/ui/ui_event.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const SessionId kSession{"errata60-session"};

UiModel make_model() {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"workspace"};
    WorkspaceModel workspace;
    workspace.id           = model.activeWorkspaceId;
    workspace.cwd          = "/work";
    workspace.daemonStatus = DaemonStatus::Attached;
    workspace.live         = true;
    model.workspaces.emplace(workspace.id, workspace);
    model.focusSessionIn(workspace.id, kSession);
    model.session(kSession)->status.model = "test-model";
    return model;
}

void append_message(UiModel& model, const std::string& text) {
    static int counter = 0;
    model.apply(UiEvent{UserMessage{kSession, "m" + std::to_string(counter++), text}});
}

std::string wrapped_paragraph(int seed) {
    std::string text;
    for (int index = 0; index < 12; ++index) {
        text += "p" + std::to_string(seed) + "w" + std::to_string(index) + " ";
    }
    return text;
}

// One supervisor frame: the renderer builds the element for `build_size`, applies
// the measured geometry to the viewed scroll, and FTXUI then lays that element
// out in a `render_size` screen. On a terminal resize the build size is the
// pre-resize `screen.dimx()/dimy()` while the render size is the new one.
ftxui::Screen draw_frame(UiModel& model, TerminalSize build_size, TerminalSize render_size,
                         TranscriptMetrics& metrics) {
    ftxui::Element element = build_ui(model, build_size, Theme{false}, &metrics);
    if (SessionUiState* state = model.session(kSession)) {
        state->scroll.observeGeometry(metrics.content_rows, metrics.viewport_rows);
    }
    ftxui::Screen screen =
        ftxui::Screen::Create(ftxui::Dimensions{render_size.width, render_size.height});
    ftxui::Render(screen, element);
    return screen;
}

int marker_row(const ftxui::Screen& screen, const std::string& marker) {
    for (int y = 0; y < screen.dimy(); ++y) {
        std::string row;
        for (int x = 0; x < screen.dimx(); ++x) {
            row += screen.PixelAt(x, y).character;
        }
        if (row.find(marker) != std::string::npos) {
            return y;
        }
    }
    return -1;
}

TEST(Errata60, ShrinkKeepsTailReachable) {
    UiModel model = make_model();
    for (int index = 0; index < 25; ++index) {
        append_message(model, wrapped_paragraph(index));
    }
    append_message(model, "TAIL-END-MARKER");
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    TranscriptMetrics metrics;
    ftxui::Screen screen = draw_frame(model, {60, 20}, {60, 20}, metrics);
    for (int index = 0; index < 3; ++index) {
        screen = draw_frame(model, {60, 20}, {60, 20}, metrics);
    }
    ASSERT_TRUE(state->scroll.following);
    EXPECT_GE(marker_row(screen, "TAIL-END-MARKER"), 0);

    for (int index = 0; index < 4; ++index) {
        state->scroll.lineUp();
    }
    screen = draw_frame(model, {60, 20}, {60, 20}, metrics);
    ASSERT_FALSE(state->scroll.following);
    EXPECT_EQ(marker_row(screen, "TAIL-END-MARKER"), -1);

    screen = draw_frame(model, {60, 20}, {44, 14}, metrics);
    for (int index = 0; index < 2; ++index) {
        screen = draw_frame(model, {44, 14}, {44, 14}, metrics);
    }

    state->scroll.toBottom();
    screen = draw_frame(model, {44, 14}, {44, 14}, metrics);
    ASSERT_TRUE(state->scroll.following);
    EXPECT_GE(marker_row(screen, "TAIL-END-MARKER"), 0);

    for (int index = 0; index < 4; ++index) {
        state->scroll.lineUp();
    }
    for (int index = 0; index < 500 && !state->scroll.following; ++index) {
        state->scroll.lineDown();
        screen = draw_frame(model, {44, 14}, {44, 14}, metrics);
    }
    ASSERT_TRUE(state->scroll.following);
    EXPECT_GE(marker_row(screen, "TAIL-END-MARKER"), 0);
}

// 60-U11 (Rev 7): the shrink fix must also hold across a shrink-then-grow, at
// extreme widths, and when a resize lands on the frame that appends content.
TEST(Errata60, ShrinkThenGrowKeepsTailReachable) {
    UiModel model = make_model();
    for (int index = 0; index < 25; ++index) {
        append_message(model, wrapped_paragraph(index));
    }
    append_message(model, "TAIL-END-MARKER");
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    TranscriptMetrics metrics;
    ftxui::Screen     screen = draw_frame(model, {60, 20}, {60, 20}, metrics);
    for (int index = 0; index < 3; ++index) {
        screen = draw_frame(model, {60, 20}, {60, 20}, metrics);
    }
    ASSERT_TRUE(state->scroll.following);
    EXPECT_GE(marker_row(screen, "TAIL-END-MARKER"), 0);

    for (int index = 0; index < 3; ++index) {
        screen = draw_frame(model, {44, 14}, {44, 14}, metrics);
    }
    EXPECT_GE(marker_row(screen, "TAIL-END-MARKER"), 0);
    const int narrow_content = state->scroll.content_rows;

    for (int index = 0; index < 3; ++index) {
        screen = draw_frame(model, {90, 28}, {90, 28}, metrics);
    }
    ASSERT_TRUE(state->scroll.following);
    EXPECT_GE(marker_row(screen, "TAIL-END-MARKER"), 0);
    // A wider pane wraps into fewer rows; the measured content height must shrink
    // with it (the Rev 6 measurement is post-layout, not a pre-layout constant).
    EXPECT_LT(state->scroll.content_rows, narrow_content);
}

TEST(Errata60, VeryNarrowWidthKeepsTailReachable) {
    UiModel model = make_model();
    for (int index = 0; index < 25; ++index) {
        append_message(model, wrapped_paragraph(index));
    }
    append_message(model, "ZZMARK");
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    TranscriptMetrics metrics;
    ftxui::Screen     screen = draw_frame(model, {16, 12}, {16, 12}, metrics);
    for (int index = 0; index < 3; ++index) {
        screen = draw_frame(model, {16, 12}, {16, 12}, metrics);
    }
    ASSERT_TRUE(state->scroll.following);
    EXPECT_GT(state->scroll.max_top(), 0);
    EXPECT_GE(marker_row(screen, "ZZMARK"), 0);
}

TEST(Errata60, VeryWideWidthKeepsTailReachable) {
    UiModel model = make_model();
    for (int index = 0; index < 40; ++index) {
        append_message(model, wrapped_paragraph(index));
    }
    append_message(model, "TAIL-END-MARKER");
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    TranscriptMetrics metrics;
    ftxui::Screen     screen = draw_frame(model, {240, 24}, {240, 24}, metrics);
    for (int index = 0; index < 3; ++index) {
        screen = draw_frame(model, {240, 24}, {240, 24}, metrics);
    }
    ASSERT_TRUE(state->scroll.following);
    EXPECT_GE(marker_row(screen, "TAIL-END-MARKER"), 0);
}

TEST(Errata60, ResizeWhileStreamingKeepsTailPinned) {
    UiModel model = make_model();
    for (int index = 0; index < 25; ++index) {
        append_message(model, wrapped_paragraph(index));
    }
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    TranscriptMetrics metrics;
    ftxui::Screen     screen = draw_frame(model, {60, 20}, {60, 20}, metrics);
    for (int index = 0; index < 3; ++index) {
        screen = draw_frame(model, {60, 20}, {60, 20}, metrics);
    }
    ASSERT_TRUE(state->scroll.following);

    append_message(model, "STREAM-TAIL-MARKER");
    screen = draw_frame(model, {60, 20}, {80, 30}, metrics);
    for (int index = 0; index < 3; ++index) {
        screen = draw_frame(model, {80, 30}, {80, 30}, metrics);
    }
    ASSERT_TRUE(state->scroll.following);
    EXPECT_GE(marker_row(screen, "STREAM-TAIL-MARKER"), 0);
}

TEST(Errata60, ContentRowsMatchWrappedHeight) {
    UiModel model = make_model();
    for (int index = 0; index < 20; ++index) {
        append_message(model, wrapped_paragraph(index));
    }
    SessionUiState* state = model.session(kSession);
    ASSERT_NE(state, nullptr);

    TranscriptMetrics metrics;
    ftxui::Screen screen = draw_frame(model, {44, 14}, {44, 14}, metrics);
    for (int index = 0; index < 3; ++index) {
        screen = draw_frame(model, {44, 14}, {44, 14}, metrics);
    }
    ASSERT_TRUE(state->scroll.following);
    EXPECT_GT(state->scroll.content_rows, 20);
    EXPECT_GT(state->scroll.max_top(), 0);
}

} // namespace
