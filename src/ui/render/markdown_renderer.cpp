#include "ymh/ui/render/markdown_renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "ymh/ui/render/syntax_renderer.hpp"

namespace ymh::ui {
namespace {

struct InlineStyle {
    bool bold = false;
    bool italic = false;
    bool dim = false;
    bool underlined = false;
    bool code = false;
};

struct Span {
    std::string text;
    InlineStyle style;
};

struct NodeGuard {
    cmark_node* node = nullptr;
    NodeGuard() = default;
    explicit NodeGuard(cmark_node* value) : node(value) {}
    ~NodeGuard() {
        if (node != nullptr) {
            cmark_node_free(node);
        }
    }
    NodeGuard(const NodeGuard&) = delete;
    NodeGuard& operator=(const NodeGuard&) = delete;
};

bool style_is_plain(const InlineStyle& style) {
    return !style.bold && !style.italic && !style.dim && !style.underlined &&
           !style.code;
}

void collect_inline(cmark_node* node, InlineStyle style, std::vector<Span>& out);

void collect_children_inline(cmark_node* node, InlineStyle style,
                             std::vector<Span>& out) {
    for (cmark_node* child = cmark_node_first_child(node); child != nullptr;
         child = cmark_node_next(child)) {
        collect_inline(child, style, out);
    }
}

void collect_inline(cmark_node* node, InlineStyle style, std::vector<Span>& out) {
    const cmark_node_type type = cmark_node_get_type(node);
    const char* literal = cmark_node_get_literal(node);

    if (type == CMARK_NODE_TEXT) {
        if (literal != nullptr && *literal != '\0') {
            out.push_back(Span{literal, style});
        }
    } else if (type == CMARK_NODE_CODE) {
        InlineStyle code_style = style;
        code_style.code = true;
        out.push_back(Span{literal != nullptr ? literal : "", code_style});
    } else if (type == CMARK_NODE_SOFTBREAK) {
        out.push_back(Span{" ", style});
    } else if (type == CMARK_NODE_LINEBREAK) {
        out.push_back(Span{"\n", style});
    } else if (type == CMARK_NODE_EMPH) {
        style.italic = true;
        collect_children_inline(node, style, out);
    } else if (type == CMARK_NODE_STRONG) {
        style.bold = true;
        collect_children_inline(node, style, out);
    } else if (type == CMARK_NODE_LINK) {
        InlineStyle link_style = style;
        link_style.underlined = true;
        const std::size_t begin = out.size();
        collect_children_inline(node, link_style, out);

        std::string text;
        for (std::size_t i = begin; i < out.size(); ++i) {
            text += out[i].text;
        }
        const char* url = cmark_node_get_url(node);
        if (url != nullptr && *url != '\0' && text != url) {
            InlineStyle url_style;
            url_style.dim = true;
            out.push_back(Span{" (" + std::string(url) + ")", url_style});
        }
    } else if (type == CMARK_NODE_IMAGE) {
        const char* url = cmark_node_get_url(node);
        std::string text = "[image";
        if (literal != nullptr && *literal != '\0') {
            text += ": ";
            text += literal;
        }
        text += "]";
        if (url != nullptr && *url != '\0') {
            text += "(";
            text += url;
            text += ")";
        }
        InlineStyle image_style = style;
        image_style.dim = true;
        out.push_back(Span{std::move(text), image_style});
    } else if (type == CMARK_NODE_HTML_INLINE) {
        InlineStyle html_style = style;
        html_style.dim = true;
        out.push_back(Span{literal != nullptr ? literal : "", html_style});
    } else {
        collect_children_inline(node, style, out);
    }
}

std::vector<Span> inline_spans(cmark_node* node) {
    std::vector<Span> spans;
    InlineStyle style;
    collect_children_inline(node, style, spans);
    return spans;
}

ftxui::Element span_element(const std::string& text, const InlineStyle& style,
                            const RenderContext& context) {
    ftxui::Element element = ftxui::text(text);
    if (style.bold) {
        element = element | ftxui::bold;
    }
    if (style.italic) {
        element = element | ftxui::italic;
    }
    if (style.underlined) {
        element = element | ftxui::underlined;
    }
    if (style.dim) {
        element = element | ftxui::dim;
    }
    if (context.theme.color && style.code) {
        element = element | ftxui::color(ftxui::Color::Yellow);
    }
    return element;
}

void append_words(ftxui::Elements& out, const std::string& text,
                  const InlineStyle& style, const RenderContext& context) {
    std::string current;
    for (const char character : text) {
        current.push_back(character);
        if (character == ' ') {
            out.push_back(span_element(current, style, context));
            current.clear();
        }
    }
    if (!current.empty()) {
        out.push_back(span_element(current, style, context));
    }
}

ftxui::Element spans_to_element(const std::vector<Span>& spans,
                                const RenderContext& context) {
    bool all_plain = true;
    std::string joined;
    for (const Span& span : spans) {
        all_plain = all_plain && style_is_plain(span.style);
        joined += span.text;
    }
    if (all_plain && joined.find('\n') == std::string::npos) {
        return ftxui::paragraph(joined);
    }

    ftxui::Elements lines;
    ftxui::Elements current;
    for (const Span& span : spans) {
        std::size_t start = 0;
        while (true) {
            const std::size_t newline = span.text.find('\n', start);
            const std::size_t length =
                newline == std::string::npos ? std::string::npos : newline - start;
            append_words(current, span.text.substr(start, length), span.style, context);
            if (newline == std::string::npos) {
                break;
            }
            lines.push_back(ftxui::hflow(std::move(current)));
            current = ftxui::Elements{};
            start = newline + 1;
        }
    }
    lines.push_back(ftxui::hflow(std::move(current)));
    if (lines.size() == 1) {
        return lines.front();
    }
    return ftxui::vbox(std::move(lines));
}

ftxui::Element render_block(cmark_node* node, const RenderContext& context);

ftxui::Element render_children(cmark_node* node, const RenderContext& context) {
    ftxui::Elements elements;
    for (cmark_node* child = cmark_node_first_child(node); child != nullptr;
         child = cmark_node_next(child)) {
        elements.push_back(render_block(child, context));
    }
    if (elements.empty()) {
        return ftxui::text("");
    }
    if (elements.size() == 1) {
        return elements.front();
    }
    return ftxui::vbox(std::move(elements));
}

ftxui::Element render_list(cmark_node* node, const RenderContext& context) {
    const bool ordered = cmark_node_get_list_type(node) == CMARK_ORDERED_LIST;
    int counter = cmark_node_get_list_start(node);

    ftxui::Elements items;
    for (cmark_node* child = cmark_node_first_child(node); child != nullptr;
         child = cmark_node_next(child)) {
        std::string marker;
        if (ordered) {
            marker = std::to_string(counter++);
            marker += ". ";
        } else {
            marker = "\u2022 ";
        }
        items.push_back(
            ftxui::hbox({ftxui::text(marker), render_children(child, context)}));
    }
    return ftxui::vbox(std::move(items));
}

std::string first_word(std::string_view text) {
    const std::size_t end = text.find_first_of(" \t");
    return std::string(text.substr(0, end));
}

ftxui::Element render_code_block(cmark_node* node, const RenderContext& context) {
    const char* info = cmark_node_get_fence_info(node);
    const char* literal = cmark_node_get_literal(node);
    std::string code = literal != nullptr ? literal : "";
    if (!code.empty() && code.back() == '\n') {
        code.pop_back();
    }
    const SyntaxRenderer renderer;
    return renderer.render(code, info != nullptr ? first_word(info) : "", context);
}

ftxui::Element render_block(cmark_node* node, const RenderContext& context) {
    const cmark_node_type type = cmark_node_get_type(node);

    if (type == CMARK_NODE_HEADING) {
        const int level = std::max(1, cmark_node_get_heading_level(node));
        std::string prefix(static_cast<std::size_t>(level), '#');
        prefix += ' ';
        ftxui::Element element = ftxui::hbox(
            {ftxui::text(prefix), spans_to_element(inline_spans(node), context)});
        element = element | ftxui::bold;
        if (context.theme.color) {
            element = element | ftxui::color(ftxui::Color::Blue);
        }
        return element;
    }
    if (type == CMARK_NODE_PARAGRAPH) {
        return spans_to_element(inline_spans(node), context);
    }
    if (type == CMARK_NODE_CODE_BLOCK) {
        return render_code_block(node, context);
    }
    if (type == CMARK_NODE_BLOCK_QUOTE) {
        ftxui::Element element =
            ftxui::hbox({ftxui::text("> "), render_children(node, context)});
        if (context.theme.color) {
            element = element | ftxui::dim;
        }
        return element;
    }
    if (type == CMARK_NODE_LIST) {
        return render_list(node, context);
    }
    if (type == CMARK_NODE_THEMATIC_BREAK) {
        return ftxui::separator();
    }
    if (type == CMARK_NODE_HTML_BLOCK) {
        const char* literal = cmark_node_get_literal(node);
        return ftxui::paragraph(literal != nullptr ? literal : "") | ftxui::dim;
    }
    return render_children(node, context);
}

cmark_node* parse_document(std::string_view text) {
    cmark_gfm_core_extensions_ensure_registered();
    cmark_parser* parser = cmark_parser_new(CMARK_OPT_DEFAULT);
    if (parser == nullptr) {
        return nullptr;
    }
    const char* extensions[] = {"table", "strikethrough", "autolink", "tasklist"};
    for (const char* name : extensions) {
        if (cmark_syntax_extension* extension = cmark_find_syntax_extension(name);
            extension != nullptr) {
            cmark_parser_attach_syntax_extension(parser, extension);
        }
    }
    cmark_parser_feed(parser, text.data(), text.size());
    cmark_node* document = cmark_parser_finish(parser);
    cmark_parser_free(parser);
    return document;
}

} // namespace

ftxui::Element MarkdownRenderer::render(std::string_view markdown,
                                        const RenderContext& context) const {
    NodeGuard document(parse_document(markdown));
    if (document.node == nullptr) {
        return ftxui::paragraph(std::string(markdown));
    }
    try {
        return render_children(document.node, context);
    } catch (...) {
        return ftxui::paragraph(std::string(markdown));
    }
}

ftxui::Element MarkdownRenderer::render(const MarkdownBlock& block,
                                        const RenderContext& context) const {
    return render(block.text, context);
}

} // namespace ymh::ui
