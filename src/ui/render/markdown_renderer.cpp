#include "ymh/ui/render/markdown_renderer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cmark-gfm-core-extensions.h>
#include <cmark-gfm.h>
#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>
#include <ftxui/screen/string.hpp>

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

RenderContext with_indent(const RenderContext& context, int extra) {
    RenderContext nested = context;
    nested.indent = context.indent + extra;
    return nested;
}

constexpr int kTableCellMaxWidth = 32;
constexpr int kTableMinColumnWidth = 8;

std::size_t utf8_glyph_length(unsigned char lead) {
    if (lead < 0x80U) {
        return 1;
    }
    if ((lead >> 5U) == 0x6U) {
        return 2;
    }
    if ((lead >> 4U) == 0xEU) {
        return 3;
    }
    if ((lead >> 3U) == 0x1EU) {
        return 4;
    }
    return 1;
}

int cell_text_width(cmark_node* cell) {
    const std::vector<Span> spans = inline_spans(cell);
    int max_width = 0;
    int current = 0;
    for (const Span& span : spans) {
        std::size_t start = 0;
        while (true) {
            const std::size_t newline = span.text.find('\n', start);
            const std::string segment =
                newline == std::string::npos ? span.text.substr(start)
                                             : span.text.substr(start, newline - start);
            current += ftxui::string_width(segment);
            if (newline == std::string::npos) {
                break;
            }
            max_width = std::max(max_width, current);
            current = 0;
            start = newline + 1;
        }
    }
    return std::max(max_width, current);
}

std::vector<std::string> split_wide_token(const std::string& token, int column_width) {
    std::vector<std::string> chunks;
    std::string current;
    int width = 0;
    std::size_t index = 0;
    while (index < token.size()) {
        std::size_t length = utf8_glyph_length(static_cast<unsigned char>(token[index]));
        if (index + length > token.size()) {
            length = 1;
        }
        const std::string glyph = token.substr(index, length);
        const int glyph_width = ftxui::string_width(glyph);
        if (!current.empty() && width + glyph_width > column_width) {
            chunks.push_back(std::move(current));
            current.clear();
            width = 0;
        }
        current += glyph;
        width += glyph_width;
        index += length;
    }
    if (!current.empty()) {
        chunks.push_back(std::move(current));
    }
    return chunks;
}

std::vector<std::vector<Span>> wrap_cell(const std::vector<Span>& spans,
                                         int column_width) {
    if (column_width < 1) {
        column_width = 1;
    }
    std::vector<std::vector<Span>> lines;
    std::vector<Span> current;
    int current_width = 0;
    bool pending_space = false;
    InlineStyle space_style;

    const auto flush = [&]() {
        lines.push_back(std::move(current));
        current = std::vector<Span>{};
        current_width = 0;
        pending_space = false;
    };

    const auto append_word = [&](const std::string& word, const InlineStyle& style) {
        const int word_width = ftxui::string_width(word);
        const int separator = (!current.empty() && pending_space) ? 1 : 0;
        if (word_width <= column_width &&
            current_width + separator + word_width <= column_width) {
            if (separator == 1) {
                current.push_back(Span{" ", space_style});
            }
            current.push_back(Span{word, style});
            current_width += separator + word_width;
        } else if (word_width <= column_width) {
            if (!current.empty()) {
                flush();
            }
            current.push_back(Span{word, style});
            current_width = word_width;
        } else {
            if (!current.empty()) {
                flush();
            }
            const std::vector<std::string> chunks = split_wide_token(word, column_width);
            for (std::size_t index = 0; index + 1 < chunks.size(); ++index) {
                current.push_back(Span{chunks[index], style});
                flush();
            }
            if (!chunks.empty()) {
                current.push_back(Span{chunks.back(), style});
                current_width = ftxui::string_width(chunks.back());
            }
        }
        pending_space = false;
    };

    for (const Span& span : spans) {
        std::size_t index = 0;
        while (index < span.text.size()) {
            const char character = span.text[index];
            if (character == '\n') {
                flush();
                ++index;
                continue;
            }
            if (character == ' ' || character == '\t') {
                while (index < span.text.size() &&
                       (span.text[index] == ' ' || span.text[index] == '\t')) {
                    ++index;
                }
                pending_space = true;
                space_style = span.style;
                continue;
            }
            const std::size_t start = index;
            while (index < span.text.size() && span.text[index] != ' ' &&
                   span.text[index] != '\t' && span.text[index] != '\n') {
                ++index;
            }
            append_word(span.text.substr(start, index - start), span.style);
        }
    }
    flush();
    return lines;
}

ftxui::Element line_to_element(const std::vector<Span>& line,
                               const RenderContext& context) {
    ftxui::Elements words;
    for (const Span& span : line) {
        append_words(words, span.text, span.style, context);
    }
    if (words.empty()) {
        return ftxui::text("");
    }
    return ftxui::hbox(std::move(words));
}

struct TableCell {
    std::vector<std::vector<Span>> lines;
    int                            width = 0;
};

struct TableLayout {
    std::vector<int>     column_width;
    std::vector<uint8_t> alignment;
    std::vector<int>     row_height;
    bool                 fits = false;
};

TableCell wrap_table_cell(cmark_node* cell, int column_width) {
    TableCell result;
    result.width = cell_text_width(cell);
    result.lines = wrap_cell(inline_spans(cell), column_width);
    return result;
}

int table_total_width(const std::vector<int>& column_width) {
    int total = 1;
    for (const int width : column_width) {
        total += width;
    }
    total += 3 * static_cast<int>(column_width.size());
    return total;
}

std::vector<cmark_node*> table_rows(cmark_node* table) {
    std::vector<cmark_node*> rows;
    for (cmark_node* row = cmark_node_first_child(table); row != nullptr;
         row = cmark_node_next(row)) {
        rows.push_back(row);
    }
    return rows;
}

TableLayout compute_table_layout(cmark_node* table, int available_width) {
    TableLayout layout;
    const int ncols = static_cast<int>(cmark_gfm_extensions_get_table_columns(table));
    if (ncols <= 0) {
        return layout;
    }
    layout.alignment.assign(static_cast<std::size_t>(ncols), 0);
    if (uint8_t* alignments = cmark_gfm_extensions_get_table_alignments(table);
        alignments != nullptr) {
        for (int column = 0; column < ncols; ++column) {
            layout.alignment[static_cast<std::size_t>(column)] = alignments[column];
        }
    }

    std::vector<int> natural(static_cast<std::size_t>(ncols), kTableMinColumnWidth);
    const std::vector<cmark_node*> rows = table_rows(table);
    for (cmark_node* row : rows) {
        int column = 0;
        for (cmark_node* cell = cmark_node_first_child(row);
             cell != nullptr && column < ncols;
             cell = cmark_node_next(cell), ++column) {
            natural[static_cast<std::size_t>(column)] =
                std::max(natural[static_cast<std::size_t>(column)], cell_text_width(cell));
        }
    }
    for (int& width : natural) {
        width = std::clamp(width, kTableMinColumnWidth, kTableCellMaxWidth);
    }
    layout.column_width = natural;
    while (table_total_width(layout.column_width) > available_width) {
        int widest = -1;
        for (std::size_t column = 0; column < layout.column_width.size(); ++column) {
            if (layout.column_width[column] <= kTableMinColumnWidth) {
                continue;
            }
            if (widest < 0 ||
                layout.column_width[column] >
                    layout.column_width[static_cast<std::size_t>(widest)]) {
                widest = static_cast<int>(column);
            }
        }
        if (widest < 0) {
            break;
        }
        --layout.column_width[static_cast<std::size_t>(widest)];
    }
    layout.fits = table_total_width(layout.column_width) <= available_width;
    if (!layout.fits) {
        return layout;
    }
    for (cmark_node* row : rows) {
        int height = 1;
        int column = 0;
        for (cmark_node* cell = cmark_node_first_child(row);
             cell != nullptr && column < ncols;
             cell = cmark_node_next(cell), ++column) {
            const TableCell measured =
                wrap_table_cell(cell, layout.column_width[static_cast<std::size_t>(column)]);
            height = std::max(height, static_cast<int>(measured.lines.size()));
        }
        layout.row_height.push_back(height);
    }
    return layout;
}

std::string table_rule(const std::vector<int>& column_width, const std::string& left,
                       const std::string& middle, const std::string& right) {
    std::string rule = left;
    for (std::size_t column = 0; column < column_width.size(); ++column) {
        if (column != 0) {
            rule += middle;
        }
        for (int index = 0; index < column_width[column]; ++index) {
            rule += "\u2500";
        }
    }
    rule += right;
    return rule;
}

ftxui::Element render_cell(cmark_node* cell, int column_width, uint8_t alignment,
                           const RenderContext& context) {
    const TableCell measured = wrap_table_cell(cell, column_width);
    ftxui::Elements elements;
    for (const std::vector<Span>& line : measured.lines) {
        ftxui::Element content = line_to_element(line, context);
        ftxui::Element aligned;
        if (alignment == 'r') {
            aligned = ftxui::hbox({ftxui::filler(), content});
        } else if (alignment == 'c') {
            aligned = ftxui::hbox({ftxui::filler(), content, ftxui::filler()});
        } else {
            aligned = ftxui::hbox({content, ftxui::filler()});
        }
        elements.push_back(aligned | ftxui::size(ftxui::WIDTH, ftxui::EQUAL, column_width));
    }
    elements.push_back(ftxui::filler());
    return ftxui::vbox(std::move(elements));
}

ftxui::Element render_table_as_text(cmark_node* table, const RenderContext& context);

ftxui::Element render_table(cmark_node* table, const RenderContext& context) {
    const int available_width = std::max(1, context.content_width - context.indent);
    const TableLayout layout = compute_table_layout(table, available_width);
    if (!layout.fits) {
        return render_table_as_text(table, context);
    }
    const int ncols = static_cast<int>(layout.column_width.size());
    ftxui::Elements rows;
    rows.push_back(
        ftxui::text(table_rule(layout.column_width, "\u256d", "\u252c", "\u256e")));
    for (cmark_node* row = cmark_node_first_child(table); row != nullptr;
         row = cmark_node_next(row)) {
        const bool is_header = cmark_gfm_extensions_get_table_row_is_header(row) != 0;
        ftxui::Elements parts;
        parts.push_back(ftxui::text("\u2502 "));
        int column = 0;
        for (cmark_node* cell = cmark_node_first_child(row); cell != nullptr;
             cell = cmark_node_next(cell)) {
            if (column >= ncols) {
                break;
            }
            if (column != 0) {
                parts.push_back(ftxui::text(" \u2502 "));
            }
            parts.push_back(render_cell(
                cell, layout.column_width[static_cast<std::size_t>(column)],
                layout.alignment[static_cast<std::size_t>(column)], context));
            ++column;
        }
        while (column < ncols) {
            if (column != 0) {
                parts.push_back(ftxui::text(" \u2502 "));
            }
            parts.push_back(ftxui::text(
                std::string(static_cast<std::size_t>(
                                layout.column_width[static_cast<std::size_t>(column)]),
                            ' ')));
            ++column;
        }
        parts.push_back(ftxui::text(" \u2502"));
        ftxui::Element row_element = ftxui::hbox(std::move(parts));
        if (is_header) {
            row_element = row_element | ftxui::bold;
        }
        rows.push_back(std::move(row_element));
        if (is_header) {
            rows.push_back(
                ftxui::text(table_rule(layout.column_width, "\u251c", "\u253c", "\u2524")));
        }
    }
    rows.push_back(
        ftxui::text(table_rule(layout.column_width, "\u2570", "\u2534", "\u256f")));
    return ftxui::vbox(std::move(rows));
}

ftxui::Element render_table_as_text(cmark_node* table, const RenderContext& context) {
    const int available_width = std::max(1, context.content_width - context.indent);
    std::vector<Span> spans;
    bool first_row = true;
    for (cmark_node* row = cmark_node_first_child(table); row != nullptr;
         row = cmark_node_next(row)) {
        if (!first_row) {
            spans.push_back(Span{"\n", InlineStyle{}});
        }
        first_row = false;
        spans.push_back(Span{"| ", InlineStyle{}});
        int column = 0;
        for (cmark_node* cell = cmark_node_first_child(row); cell != nullptr;
             cell = cmark_node_next(cell)) {
            if (column != 0) {
                spans.push_back(Span{" | ", InlineStyle{}});
            }
            for (Span& span : inline_spans(cell)) {
                spans.push_back(std::move(span));
            }
            ++column;
        }
        spans.push_back(Span{" |", InlineStyle{}});
    }
    ftxui::Elements lines;
    for (const std::vector<Span>& line : wrap_cell(spans, available_width)) {
        lines.push_back(line_to_element(line, context));
    }
    if (lines.empty()) {
        return ftxui::text("");
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
        items.push_back(ftxui::hbox(
            {ftxui::text(marker),
             render_children(child, with_indent(context, ftxui::string_width(marker)))}));
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
        ftxui::Element element = ftxui::hbox(
            {ftxui::text("> "), render_children(node, with_indent(context, 2))});
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
    if (std::string_view{cmark_node_get_type_string(node)} == "table") {
        return render_table(node, context);
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
