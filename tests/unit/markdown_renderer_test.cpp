#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "ymh/ui/render/markdown_renderer.hpp"

namespace {

using namespace ymh::ui;

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

std::string render_text(const std::string& markdown) {
    const RenderContext context{80, Theme{false}, false};
    ftxui::Element element = MarkdownRenderer{}.render(markdown, context);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimensions{80, 40});
    ftxui::Render(screen, element);
    return strip_ansi(screen.ToString());
}

TEST(MarkdownRenderer, RendersHeading) {
    const std::string rendered = render_text("# Title\n");
    EXPECT_NE(rendered.find("# Title"), std::string::npos);
}

TEST(MarkdownRenderer, RendersBoldAndItalic) {
    const std::string rendered = render_text("This is **bold** and *italic* text.\n");
    EXPECT_NE(rendered.find("bold"), std::string::npos);
    EXPECT_NE(rendered.find("italic"), std::string::npos);
}

TEST(MarkdownRenderer, RendersInlineCode) {
    const std::string rendered = render_text("Use `std::vector` here.\n");
    EXPECT_NE(rendered.find("std::vector"), std::string::npos);
}

TEST(MarkdownRenderer, RendersFencedCodeBlock) {
    const std::string rendered = render_text("```cpp\nint main() { return 0; }\n```\n");
    EXPECT_NE(rendered.find("int"), std::string::npos);
    EXPECT_NE(rendered.find("main"), std::string::npos);
}

TEST(MarkdownRenderer, RendersList) {
    const std::string rendered = render_text("- one\n- two\n");
    EXPECT_NE(rendered.find("one"), std::string::npos);
    EXPECT_NE(rendered.find("two"), std::string::npos);
    EXPECT_NE(rendered.find("\u2022"), std::string::npos);
}

TEST(MarkdownRenderer, RendersOrderedList) {
    const std::string rendered = render_text("1. first\n2. second\n");
    EXPECT_NE(rendered.find("first"), std::string::npos);
    EXPECT_NE(rendered.find("second"), std::string::npos);
}

TEST(MarkdownRenderer, RendersBlockQuote) {
    const std::string rendered = render_text("> quoted line\n");
    EXPECT_NE(rendered.find("> quoted line"), std::string::npos);
}

TEST(MarkdownRenderer, RendersLinkWithUrl) {
    const std::string rendered = render_text("[label](https://example.com)\n");
    EXPECT_NE(rendered.find("label"), std::string::npos);
    EXPECT_NE(rendered.find("https://example.com"), std::string::npos);
}

TEST(MarkdownRenderer, RendersThematicBreak) {
    EXPECT_NO_THROW({
        const std::string rendered = render_text("before\n\n---\n\nafter\n");
        EXPECT_NE(rendered.find("before"), std::string::npos);
        EXPECT_NE(rendered.find("after"), std::string::npos);
    });
}

TEST(MarkdownRenderer, StreamingPartialDoesNotThrow) {
    EXPECT_NO_THROW({
        const std::string rendered = render_text("# Head\n\n```cpp\nint x");
        EXPECT_NE(rendered.find("# Head"), std::string::npos);
        EXPECT_NE(rendered.find("int x"), std::string::npos);
    });
    EXPECT_NO_THROW(render_text("**unterminated"));
    EXPECT_NO_THROW(render_text("[unterminated link]("));
    EXPECT_NO_THROW(render_text(""));
}

TEST(MarkdownRenderer, BlockOverloadMatchesStringOverload) {
    const RenderContext context{80, Theme{false}, false};
    const std::string direct = render_text("plain text\n");
    ftxui::Element element =
        MarkdownRenderer{}.render(MarkdownBlock{"plain text\n"}, context);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimensions{80, 40});
    ftxui::Render(screen, element);
    EXPECT_NE(strip_ansi(screen.ToString()).find("plain text"), std::string::npos);
    EXPECT_NE(direct.find("plain text"), std::string::npos);
}

} // namespace
