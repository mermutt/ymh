#pragma once

// cmark-gfm-backed markdown renderer (00 §22, 10 §8.2). Parsing is pure: no I/O,
// no globals advanced, and partial/streaming input degrades to plain text rather
// than throwing. Fenced code blocks are delegated to `SyntaxRenderer`.

#include <string>
#include <string_view>

#include <ftxui/dom/elements.hpp>

#include "ymh/ui/render/render_context.hpp"

namespace ymh::ui {

struct MarkdownBlock {
    std::string text;
};

class MarkdownRenderer {
public:
    [[nodiscard]] ftxui::Element render(const MarkdownBlock& block,
                                        const RenderContext& context) const;
    [[nodiscard]] ftxui::Element render(std::string_view markdown,
                                        const RenderContext& context) const;
};

} // namespace ymh::ui
