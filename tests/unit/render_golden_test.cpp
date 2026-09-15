#include <gtest/gtest.h>

#include <iostream>
#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "ymh/ui/render/diff_renderer.hpp"
#include "ymh/ui/render/markdown_renderer.hpp"
#include "ymh/ui/ui_model.hpp"
#include "ymh/ui/ui_render.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

const char* kSampleMarkdown =
    "# Release notes\n"
    "\n"
    "This is **bold** and `inline` code.\n"
    "\n"
    "```cpp\n"
    "int main() { return 0; }\n"
    "```\n"
    "\n"
    "- first\n"
    "- second\n"
    "\n"
    "> note\n";

const char* kSampleDiff =
    "diff --git a/src/foo.cpp b/src/foo.cpp\n"
    "--- a/src/foo.cpp\n"
    "+++ b/src/foo.cpp\n"
    "@@ -1,2 +1,3 @@\n"
    " int main() {\n"
    "-    return 1;\n"
    "+    return 0;\n"
    "+    // done\n"
    " }\n";

std::string strip_ansi(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '\x1b' && index + 1 < input.size() && input[index + 1] == '[') {
            index += 2;
            while (index < input.size() &&
                   (input[index] == ';' || (input[index] >= '0' && input[index] <= '9'))) {
                ++index;
            }
            continue;
        }
        output.push_back(input[index]);
    }
    return output;
}

std::string normalize(const std::string& input) {
    std::vector<std::string> lines;
    std::string current;
    for (const char character : strip_ansi(input)) {
        if (character == '\r') {
            continue;
        }
        if (character == '\n') {
            while (!current.empty() && current.back() == ' ') {
                current.pop_back();
            }
            lines.push_back(current);
            current.clear();
        } else {
            current.push_back(character);
        }
    }
    while (!current.empty() && current.back() == ' ') {
        current.pop_back();
    }
    lines.push_back(current);
    while (!lines.empty() && lines.back().empty()) {
        lines.pop_back();
    }
    std::string result;
    for (std::size_t index = 0; index < lines.size(); ++index) {
        if (index != 0) {
            result += '\n';
        }
        result += lines[index];
    }
    return result;
}

std::string render_element(ftxui::Element element, int width, int height) {
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimensions{width, height});
    ftxui::Render(screen, element);
    return normalize(screen.ToString());
}

std::string render_markdown_sample() {
    const RenderContext context{60, Theme{false}, false};
    return render_element(MarkdownRenderer{}.render(kSampleMarkdown, context), 60, 30);
}

std::string render_diff_sample() {
    const RenderContext context{60, Theme{false}, false};
    return render_element(DiffRenderer{}.render(DiffModel::parse(kSampleDiff), context), 60,
                          30);
}

const char* kMarkdownGolden = R"GOLDEN(# Release notes
This is bold and inline code.
int main() { return 0; }
• first
• second
> note)GOLDEN";

const char* kDiffGolden = R"GOLDEN(src/foo.cpp  +2 -1
@@ -1,2 +1,3 @@
 int main() {
-    return 1;
+    return 0;
+    // done
 })GOLDEN";

TEST(RenderGolden, MarkdownSample) {
    const std::string rendered = render_markdown_sample();
    std::cerr << "\n---MARKDOWN-GOLDEN-BEGIN---\n" << rendered
              << "\n---MARKDOWN-GOLDEN-END---\n";
    EXPECT_EQ(rendered, kMarkdownGolden);
}

TEST(RenderGolden, DiffSample) {
    const std::string rendered = render_diff_sample();
    std::cerr << "\n---DIFF-GOLDEN-BEGIN---\n" << rendered << "\n---DIFF-GOLDEN-END---\n";
    EXPECT_EQ(rendered, kDiffGolden);
}

TEST(RenderGolden, MarkdownContainsExpectedLines) {
    const std::string rendered = render_markdown_sample();
    EXPECT_NE(rendered.find("# Release notes"), std::string::npos);
    EXPECT_NE(rendered.find("bold"), std::string::npos);
    EXPECT_NE(rendered.find("int main() { return 0; }"), std::string::npos);
    EXPECT_NE(rendered.find("first"), std::string::npos);
}

TEST(RenderGolden, DiffContainsExpectedLines) {
    const std::string rendered = render_diff_sample();
    EXPECT_NE(rendered.find("src/foo.cpp"), std::string::npos);
    EXPECT_NE(rendered.find("-    return 1;"), std::string::npos);
    EXPECT_NE(rendered.find("+    return 0;"), std::string::npos);
}

TEST(RenderGolden, TuiRoutesAssistantMarkdownAndToolDiff) {
    UiModel model;
    model.activeWorkspaceId = WorkspaceId{"workspace"};
    WorkspaceModel& workspace = model.workspaces[model.activeWorkspaceId];
    workspace.id = model.activeWorkspaceId;
    workspace.activeSessionId = SessionId{"session"};
    model.ensureSession(SessionId{"session"});

    SessionUiState* session = model.session(SessionId{"session"});
    ASSERT_NE(session, nullptr);
    session->conversation.entries.push_back(
        ConversationEntry{ConversationRole::Assistant,
                          "# Title\n\n```cpp\nint x = 1;\n```\n", "", "", false});
    session->conversation.entries.push_back(
        ConversationEntry{ConversationRole::Tool, kSampleDiff, "git_diff", "call", false});

    const std::string rendered =
        normalize(render_to_ansi(model, TerminalSize{60, 40}, Theme{false}));
    EXPECT_NE(rendered.find("# Title"), std::string::npos);
    EXPECT_NE(rendered.find("int x = 1;"), std::string::npos);
    EXPECT_NE(rendered.find("src/foo.cpp"), std::string::npos);
    EXPECT_NE(rendered.find("-    return 1;"), std::string::npos);
}

} // namespace
