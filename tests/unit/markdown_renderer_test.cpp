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
    const RenderContext context{
        .width = 80, .content_width = 80, .theme = Theme{false}, .compact = false};
    ftxui::Element element = MarkdownRenderer{}.render(markdown, context);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimensions{80, 40});
    ftxui::Render(screen, element);
    return strip_ansi(screen.ToString());
}

std::string render_width(const std::string& markdown, int content_width) {
    RenderContext context{};
    context.width = content_width;
    context.content_width = content_width;
    context.theme = Theme{false};
    ftxui::Element element = MarkdownRenderer{}.render(markdown, context);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimensions{120, 60});
    ftxui::Render(screen, element);
    return strip_ansi(screen.ToString());
}

std::string squash_alnum(const std::string& input) {
    std::string output;
    for (const char character : input) {
        const bool digit = character >= '0' && character <= '9';
        const bool upper = character >= 'A' && character <= 'Z';
        const bool lower = character >= 'a' && character <= 'z';
        if (digit || upper || lower) {
            output.push_back(character);
        }
    }
    return output;
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
    const RenderContext context{
        .width = 80, .content_width = 80, .theme = Theme{false}, .compact = false};
    const std::string direct = render_text("plain text\n");
    ftxui::Element element =
        MarkdownRenderer{}.render(MarkdownBlock{"plain text\n"}, context);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimensions{80, 40});
    ftxui::Render(screen, element);
    EXPECT_NE(strip_ansi(screen.ToString()).find("plain text"), std::string::npos);
    EXPECT_NE(direct.find("plain text"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_SimpleTable) {
    const std::string rendered = render_text("| A | B |\n|---|---|\n| 1 | 2 |\n");
    EXPECT_NE(rendered.find("\u256d"), std::string::npos);
    EXPECT_NE(rendered.find("\u252c"), std::string::npos);
    EXPECT_NE(rendered.find("\u256e"), std::string::npos);
    EXPECT_NE(rendered.find("\u251c"), std::string::npos);
    EXPECT_NE(rendered.find("\u253c"), std::string::npos);
    EXPECT_NE(rendered.find("\u2524"), std::string::npos);
    EXPECT_NE(rendered.find("\u2570"), std::string::npos);
    EXPECT_NE(rendered.find("\u2534"), std::string::npos);
    EXPECT_NE(rendered.find("\u256f"), std::string::npos);
    EXPECT_NE(rendered.find("A"), std::string::npos);
    EXPECT_NE(rendered.find("B"), std::string::npos);
    EXPECT_NE(rendered.find("1"), std::string::npos);
    EXPECT_NE(rendered.find("2"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_HeaderRowNotDropped) {
    const std::string rendered = render_text("| HEAD |\n|---|\n| body |\n");
    const std::size_t header = rendered.find("HEAD");
    const std::size_t rule = rendered.find("\u251c");
    const std::size_t body = rendered.find("body");
    EXPECT_NE(header, std::string::npos);
    EXPECT_NE(rule, std::string::npos);
    EXPECT_NE(body, std::string::npos);
    EXPECT_LT(header, rule);
    EXPECT_LT(rule, body);
}

TEST(MarkdownRenderer, UI51_D3_AlignmentVariants) {
    const std::string rendered =
        render_text("| L | C | R |\n|:---|:---:|---:|\n| **a** | b | c |\n");
    EXPECT_NE(rendered.find("\u2502 a"), std::string::npos);
    EXPECT_NE(rendered.find("c \u2502"), std::string::npos);
    EXPECT_NE(rendered.find(" b "), std::string::npos);
    EXPECT_EQ(rendered.find("\u2502b"), std::string::npos);
    EXPECT_EQ(rendered.find("b\u2502"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_InlineFormattingInCells) {
    const std::string rendered = render_text(
        "| **b** | *i* | `code` |\n|---|---|---|\n| [link](http://x) | plain | ok |\n");
    EXPECT_NE(rendered.find("b"), std::string::npos);
    EXPECT_NE(rendered.find("i"), std::string::npos);
    EXPECT_NE(rendered.find("code"), std::string::npos);
    EXPECT_NE(rendered.find("link"), std::string::npos);
    EXPECT_NE(rendered.find("http://x"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_WiderThanPaneFallsBack) {
    const std::string rendered = render_width(
        "| a | b | c | d | e | f |\n|---|---|---|---|---|---|\n| 1 | 2 | 3 | 4 | 5 | 6 |\n",
        40);
    EXPECT_EQ(rendered.find("\u256d"), std::string::npos);
    EXPECT_EQ(rendered.find("\u252c"), std::string::npos);
    EXPECT_NE(rendered.find("| a | b | c | d | e | f |"), std::string::npos);
    EXPECT_NE(rendered.find("| 1 | 2 | 3 | 4 | 5 | 6 |"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_OverWideTokenHardBreak) {
    const std::string token = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcd";
    const std::string rendered =
        render_width("| H |\n|---|\n| " + token + " |\n", 12);
    EXPECT_NE(rendered.find("\u256d"), std::string::npos);
    EXPECT_NE(squash_alnum(rendered).find(token), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_RowHeightMatchesWrap) {
    const std::string rendered = render_width("| H |\n|---|\n| a bbbbbbb |\n", 12);
    const std::size_t first = rendered.find("\u2502 a");
    const std::size_t second = rendered.find("bbbbbbb");
    const std::size_t bottom = rendered.find("\u2570");
    EXPECT_NE(first, std::string::npos);
    EXPECT_NE(second, std::string::npos);
    EXPECT_LT(first, second);
    EXPECT_LT(second, bottom);
}

TEST(MarkdownRenderer, UI51_D3_EmptyCell) {
    const std::string rendered = render_text("| A | B |\n|---|---|\n|  | 2 |\n");
    EXPECT_NE(rendered.find("A"), std::string::npos);
    EXPECT_NE(rendered.find("2"), std::string::npos);
    EXPECT_NE(rendered.find("\u2570"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_RaggedTable) {
    const std::string rendered = render_text("| A | B | C |\n|---|---|---|\n| 1 |\n");
    EXPECT_NE(rendered.find("A"), std::string::npos);
    EXPECT_NE(rendered.find("1"), std::string::npos);
    EXPECT_NE(rendered.find("\u252c"), std::string::npos);
    EXPECT_NE(rendered.find("\u2570"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_ZeroColumnsFallsBack) {
    // GFM cannot produce a zero-column table node, so this pins that such
    // degenerate pipe input stays visible and never throws (51-F4).
    EXPECT_NO_THROW({
        const std::string rendered = render_text("|\n|");
        EXPECT_NE(rendered.find("|"), std::string::npos);
    });
    EXPECT_NO_THROW(render_text("||\n||"));
}

TEST(MarkdownRenderer, UI51_D3_NullAlignmentsMeansLeft) {
    const std::string rendered = render_text("| A | B |\n|---|---|\n| 1 | 2 |\n");
    EXPECT_NE(rendered.find("\u2502 1"), std::string::npos);
    EXPECT_NE(rendered.find("\u2502 2"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_ExtensionStillEnabled) {
    const std::string rendered = render_text("| a | b |\n|---|---|\n| 1 | 2 |\n");
    EXPECT_NE(rendered.find("\u256d"), std::string::npos);
    EXPECT_EQ(rendered.find("| a | b |"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_MalformedPipeTextStillVisible) {
    const std::string rendered = render_text("| a | b |\n");
    EXPECT_NE(rendered.find("| a | b |"), std::string::npos);
    EXPECT_EQ(rendered.find("\u256d"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_LargeTableBounded) {
    std::string markdown = "| n |\n|---|\n";
    for (int index = 0; index < 500; ++index) {
        markdown += "| row" + std::to_string(index) + " |\n";
    }
    EXPECT_NO_THROW({
        const std::string rendered = render_text(markdown);
        EXPECT_NE(rendered.find("row0"), std::string::npos);
        EXPECT_LT(rendered.size(), 10000U);
    });
}

TEST(MarkdownRenderer, UI51_D3_TableInBlockquoteUsesNestedWidth) {
    const std::string table =
        "| a | b | c | d | e | f |\n|---|---|---|---|---|---|\n| 1 | 2 | 3 | 4 | 5 | 6 |\n";
    const std::string plain = render_width(table, 68);
    EXPECT_NE(plain.find("\u252c"), std::string::npos);
    const std::string nested = render_width(
        "> | a | b | c | d | e | f |\n"
        "> |---|---|---|---|---|---|\n"
        "> | 1 | 2 | 3 | 4 | 5 | 6 |\n",
        68);
    EXPECT_EQ(nested.find("\u252c"), std::string::npos);
    EXPECT_NE(nested.find("| a | b | c | d | e | f |"), std::string::npos);
}

TEST(MarkdownRenderer, UI51_D3_PartialTableDoesNotThrow) {
    EXPECT_NO_THROW(render_text("| A | B |\n|---"));
    EXPECT_NO_THROW(render_text("| A | B |\n|---|---|\n| 1"));
    EXPECT_NO_THROW(render_text("| A |\n|--"));
}

TEST(MarkdownRenderer, UI51_D3_FallbackHardBreaksToken) {
    const std::string token = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789abcd";
    const std::string rendered = render_width(
        "| a | b | c | d | e | f |\n|---|---|---|---|---|---|\n| " + token +
            " | 2 | 3 | 4 | 5 | 6 |\n",
        30);
    EXPECT_EQ(rendered.find("\u252c"), std::string::npos);
    EXPECT_NE(squash_alnum(rendered).find(token), std::string::npos);
}

} // namespace
