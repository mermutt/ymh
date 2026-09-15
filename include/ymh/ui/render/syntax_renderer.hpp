#pragma once

// Lightweight, deterministic syntax highlighter (00 §23). The design names
// `tree-sitter`; this MVP ships a hand-written lexer for C/C++, Python, and
// shell because only the C grammar is installed locally and fetching grammars
// over the network is prohibited. Token classification is pure and testable;
// FTXUI knows nothing about the lexer and vice versa.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>

#include "ymh/ui/render/render_context.hpp"

namespace ymh::ui {

enum class TokenKind : std::uint8_t {
    Plain,
    Keyword,
    Type,
    String,
    Comment,
    Number,
    Preprocessor,
};

struct SyntaxToken {
    TokenKind   kind = TokenKind::Plain;
    std::string text;
};

// Canonicalizes a fenced-code language tag (e.g. "C++" -> "cpp"); unknown tags
// return an empty string so the caller can fall back to plain rendering.
[[nodiscard]] std::string normalize_language(std::string_view language);

[[nodiscard]] std::vector<SyntaxToken> tokenize(std::string_view code,
                                                std::string_view language);

class SyntaxRenderer {
public:
    [[nodiscard]] ftxui::Element render(std::string_view code,
                                        std::string_view language,
                                        const RenderContext& context) const;
};

using CodeBlockRenderer = SyntaxRenderer;

} // namespace ymh::ui
