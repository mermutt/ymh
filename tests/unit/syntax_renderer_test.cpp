#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "ymh/ui/render/syntax_renderer.hpp"

namespace {

using namespace ymh::ui;

void expect_token(const std::vector<SyntaxToken>& tokens, const std::string& text,
                  TokenKind kind) {
    for (const SyntaxToken& token : tokens) {
        if (token.text == text) {
            EXPECT_EQ(token.kind, kind) << "token: " << text;
            return;
        }
    }
    ADD_FAILURE() << "token not found: " << text;
}

std::string render_text(std::string_view code, std::string_view language) {
    const RenderContext context{
        .width = 80, .content_width = 80, .theme = Theme{false}, .compact = false};
    ftxui::Element element = SyntaxRenderer{}.render(code, language, context);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimensions{80, 20});
    ftxui::Render(screen, element);
    return screen.ToString();
}

TEST(SyntaxRenderer, NormalizesLanguageTags) {
    EXPECT_EQ(normalize_language("C++"), "cpp");
    EXPECT_EQ(normalize_language("c"), "cpp");
    EXPECT_EQ(normalize_language("Python"), "python");
    EXPECT_EQ(normalize_language("bash"), "shell");
    EXPECT_EQ(normalize_language("sh"), "shell");
    EXPECT_EQ(normalize_language("rust"), "");
}

TEST(SyntaxRenderer, ClassifiesCppTokens) {
    const auto tokens = tokenize("int main() { return 0; }", "cpp");
    expect_token(tokens, "int", TokenKind::Type);
    expect_token(tokens, "main", TokenKind::Plain);
    expect_token(tokens, "return", TokenKind::Keyword);
    expect_token(tokens, "0", TokenKind::Number);
}

TEST(SyntaxRenderer, ClassifiesCppCommentsPreprocessorAndStrings) {
    const auto comment = tokenize("// comment\n", "cpp");
    expect_token(comment, "// comment", TokenKind::Comment);

    const auto preprocessor = tokenize("#include <string>\n", "cpp");
    expect_token(preprocessor, "#include <string>", TokenKind::Preprocessor);

    const auto string = tokenize("const char* s = \"hi\";", "cpp");
    expect_token(string, "\"hi\"", TokenKind::String);
}

TEST(SyntaxRenderer, ClassifiesPythonTokens) {
    const auto tokens = tokenize("def foo():\n    return 1  # done\n", "python");
    expect_token(tokens, "def", TokenKind::Keyword);
    expect_token(tokens, "return", TokenKind::Keyword);
    expect_token(tokens, "1", TokenKind::Number);
    expect_token(tokens, "# done", TokenKind::Comment);
}

TEST(SyntaxRenderer, ClassifiesPythonTripleString) {
    const auto tokens = tokenize("\"\"\"doc\"\"\"\n", "python");
    expect_token(tokens, "\"\"\"doc\"\"\"", TokenKind::String);
}

TEST(SyntaxRenderer, ClassifiesShellTokens) {
    const auto tokens = tokenize("if [ -f $file ]; then echo hi; fi\n", "shell");
    expect_token(tokens, "if", TokenKind::Keyword);
    expect_token(tokens, "then", TokenKind::Keyword);
    expect_token(tokens, "echo", TokenKind::Keyword);
    expect_token(tokens, "$file", TokenKind::Type);
    expect_token(tokens, "hi", TokenKind::Plain);
}

TEST(SyntaxRenderer, ClassifiesShellString) {
    const auto tokens = tokenize("echo \"$file\"\n", "shell");
    expect_token(tokens, "\"$file\"", TokenKind::String);
}

TEST(SyntaxRenderer, ClassifiesShellComment) {
    const auto tokens = tokenize("#!/bin/bash\n", "shell");
    expect_token(tokens, "#!/bin/bash", TokenKind::Comment);
}

TEST(SyntaxRenderer, RendersCodeWithoutThrowing) {
    const std::string rendered = render_text("int x = 1;\n", "cpp");
    EXPECT_NE(rendered.find("int"), std::string::npos);
    EXPECT_NE(rendered.find("x"), std::string::npos);
}

TEST(SyntaxRenderer, UnknownLanguageIsPlain) {
    const auto tokens = tokenize("plain text", "rust");
    ASSERT_EQ(tokens.size(), 1u);
    EXPECT_EQ(tokens.front().kind, TokenKind::Plain);
}

} // namespace
