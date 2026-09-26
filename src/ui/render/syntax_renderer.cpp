#include "ymh/ui/render/syntax_renderer.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

namespace ymh::ui {
namespace {

enum class Language : std::uint8_t { Plain, Cpp, Python, Shell };

using WordSet = std::unordered_set<std::string>;

bool is_ident_start(char c) {
    return std::isalpha(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_digit(char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; }

const WordSet& cpp_keywords() {
    static const WordSet words = {
        "alignas", "alignof", "and", "asm", "auto", "bitand", "bitor", "bool",
        "break", "case", "catch", "class", "compl", "concept", "const",
        "consteval", "constexpr", "constinit", "const_cast", "continue",
        "co_await", "co_return", "co_yield", "decltype", "default", "delete",
        "do", "dynamic_cast", "else", "enum", "explicit", "export", "extern",
        "false", "for", "friend", "goto", "if", "inline", "mutable", "namespace",
        "new", "noexcept", "not", "nullptr", "operator", "or", "private",
        "protected", "public", "register", "reinterpret_cast", "requires",
        "return", "sizeof", "static", "static_assert", "static_cast", "struct",
        "switch", "template", "this", "throw", "true", "try", "typedef",
        "typeid", "typename", "union", "using", "virtual", "volatile", "while",
        "xor",
    };
    return words;
}

const WordSet& cpp_types() {
    static const WordSet words = {
        "char", "char8_t", "char16_t", "char32_t", "double", "float", "int",
        "long", "short", "signed", "unsigned", "void", "wchar_t", "size_t",
        "ssize_t", "int8_t", "int16_t", "int32_t", "int64_t", "uint8_t",
        "uint16_t", "uint32_t", "uint64_t", "std", "string", "string_view",
        "vector", "size_t",
    };
    return words;
}

const WordSet& python_keywords() {
    static const WordSet words = {
        "and", "as", "assert", "async", "await", "break", "class", "continue",
        "def", "del", "elif", "else", "except", "False", "finally", "for",
        "from", "global", "if", "import", "in", "is", "lambda", "None",
        "nonlocal", "not", "or", "pass", "raise", "return", "True", "try",
        "while", "with", "yield", "match", "case",
    };
    return words;
}

const WordSet& shell_keywords() {
    static const WordSet words = {
        "if", "then", "else", "elif", "fi", "for", "while", "until", "do",
        "done", "case", "esac", "function", "in", "select", "time", "return",
        "break", "continue", "exit", "local", "export", "readonly", "declare",
        "unset", "shift", "set", "trap", "source", "alias", "echo", "printf",
        "cd", "pwd", "read", "test", "eval", "exec", "ulimit", "wait",
    };
    return words;
}

void push(std::vector<SyntaxToken>& out, TokenKind kind, std::string text) {
    if (!text.empty()) {
        out.push_back(SyntaxToken{kind, std::move(text)});
    }
}

bool at_line_start(std::string_view code, std::size_t index) {
    while (index > 0) {
        const char previous = code[index - 1];
        if (previous == '\n') {
            return true;
        }
        if (previous != ' ' && previous != '\t' && previous != '\r') {
            return false;
        }
        --index;
    }
    return true;
}

std::size_t consume_line(std::string_view code, std::size_t start) {
    const std::size_t newline = code.find('\n', start);
    return newline == std::string_view::npos ? code.size() : newline;
}

std::size_t consume_block_comment(std::string_view code, std::size_t start) {
    const std::size_t close = code.find("*/", start + 2);
    return close == std::string_view::npos ? code.size() : close + 2;
}

std::size_t consume_string(std::string_view code, std::size_t start, char quote,
                           bool escapes) {
    std::size_t index = start + 1;
    while (index < code.size()) {
        const char current = code[index];
        if (escapes && current == '\\' && index + 1 < code.size()) {
            index += 2;
            continue;
        }
        if (current == quote) {
            return index + 1;
        }
        if (current == '\n' && quote != '`') {
            return index;
        }
        ++index;
    }
    return code.size();
}

std::size_t consume_triple_string(std::string_view code, std::size_t start,
                                  char quote) {
    const std::string closing(3, quote);
    const std::size_t close = code.find(closing, start + 3);
    return close == std::string_view::npos ? code.size() : close + 3;
}

std::size_t consume_number(std::string_view code, std::size_t start) {
    std::size_t index = start;
    while (index < code.size()) {
        const char current = code[index];
        if (is_ident_char(current) || current == '.' || current == '\'') {
            const bool exponent = current == 'e' || current == 'E' || current == 'p' ||
                                  current == 'P';
            if (exponent && index + 1 < code.size() &&
                (code[index + 1] == '+' || code[index + 1] == '-')) {
                index += 2;
                continue;
            }
            ++index;
            continue;
        }
        break;
    }
    return index;
}

TokenKind classify_identifier(std::string_view word, const WordSet& keywords,
                              const WordSet* types) {
    const std::string text(word);
    if (keywords.contains(text)) {
        return TokenKind::Keyword;
    }
    if (types != nullptr && types->contains(text)) {
        return TokenKind::Type;
    }
    return TokenKind::Plain;
}

std::vector<SyntaxToken> tokenize_c_like(std::string_view code) {
    std::vector<SyntaxToken> out;
    std::size_t index = 0;
    while (index < code.size()) {
        const char current = code[index];
        const char next = index + 1 < code.size() ? code[index + 1] : '\0';

        if (current == '/' && next == '/') {
            const std::size_t end = consume_line(code, index);
            push(out, TokenKind::Comment, std::string(code.substr(index, end - index)));
            index = end;
        } else if (current == '/' && next == '*') {
            const std::size_t end = consume_block_comment(code, index);
            push(out, TokenKind::Comment, std::string(code.substr(index, end - index)));
            index = end;
        } else if (current == '#' && at_line_start(code, index)) {
            std::size_t end = index;
            while (end < code.size()) {
                if (code[end] == '\\' && end + 1 < code.size() && code[end + 1] == '\n') {
                    end += 2;
                    continue;
                }
                if (code[end] == '\n') {
                    break;
                }
                ++end;
            }
            push(out, TokenKind::Preprocessor,
                 std::string(code.substr(index, end - index)));
            index = end;
        } else if (current == '"' || current == '\'') {
            const std::size_t end = consume_string(code, index, current, true);
            push(out, TokenKind::String, std::string(code.substr(index, end - index)));
            index = end;
        } else if (is_digit(current) ||
                   (current == '.' && index + 1 < code.size() && is_digit(code[index + 1]))) {
            const std::size_t end = consume_number(code, index);
            push(out, TokenKind::Number, std::string(code.substr(index, end - index)));
            index = end;
        } else if (is_ident_start(current)) {
            std::size_t end = index + 1;
            while (end < code.size() && is_ident_char(code[end])) {
                ++end;
            }
            const std::string_view word = code.substr(index, end - index);
            push(out, classify_identifier(word, cpp_keywords(), &cpp_types()),
                 std::string(word));
            index = end;
        } else {
            push(out, TokenKind::Plain, std::string(1, current));
            ++index;
        }
    }
    return out;
}

std::vector<SyntaxToken> tokenize_python(std::string_view code) {
    std::vector<SyntaxToken> out;
    std::size_t index = 0;
    while (index < code.size()) {
        const char current = code[index];
        const char next = index + 1 < code.size() ? code[index + 1] : '\0';
        const char third = index + 2 < code.size() ? code[index + 2] : '\0';

        if (current == '#') {
            const std::size_t end = consume_line(code, index);
            push(out, TokenKind::Comment, std::string(code.substr(index, end - index)));
            index = end;
        } else if ((current == '"' || current == '\'') && current == next && next == third) {
            const std::size_t end = consume_triple_string(code, index, current);
            push(out, TokenKind::String, std::string(code.substr(index, end - index)));
            index = end;
        } else if (current == '"' || current == '\'') {
            const std::size_t end = consume_string(code, index, current, true);
            push(out, TokenKind::String, std::string(code.substr(index, end - index)));
            index = end;
        } else if (is_digit(current) ||
                   (current == '.' && index + 1 < code.size() && is_digit(code[index + 1]))) {
            const std::size_t end = consume_number(code, index);
            push(out, TokenKind::Number, std::string(code.substr(index, end - index)));
            index = end;
        } else if (is_ident_start(current)) {
            std::size_t end = index + 1;
            while (end < code.size() && is_ident_char(code[end])) {
                ++end;
            }
            const std::string_view word = code.substr(index, end - index);
            push(out, classify_identifier(word, python_keywords(), nullptr),
                 std::string(word));
            index = end;
        } else {
            push(out, TokenKind::Plain, std::string(1, current));
            ++index;
        }
    }
    return out;
}

std::size_t consume_shell_variable(std::string_view code, std::size_t start) {
    std::size_t index = start + 1;
    if (index >= code.size()) {
        return index;
    }
    if (code[index] == '{') {
        const std::size_t close = code.find('}', index + 1);
        return close == std::string_view::npos ? code.size() : close + 1;
    }
    if (code[index] == '(') {
        int depth = 1;
        std::size_t cursor = index + 1;
        while (cursor < code.size() && depth > 0) {
            if (code[cursor] == '(') {
                ++depth;
            } else if (code[cursor] == ')') {
                --depth;
            }
            ++cursor;
        }
        return cursor;
    }
    if (code[index] == '?' || code[index] == '#' || code[index] == '@' ||
        code[index] == '*' || code[index] == '$' || code[index] == '!' ||
        is_digit(code[index])) {
        return index + 1;
    }
    while (index < code.size() && is_ident_char(code[index])) {
        ++index;
    }
    return index;
}

std::vector<SyntaxToken> tokenize_shell(std::string_view code) {
    std::vector<SyntaxToken> out;
    std::size_t index = 0;
    while (index < code.size()) {
        const char current = code[index];

        if (current == '#') {
            const std::size_t end = consume_line(code, index);
            push(out, TokenKind::Comment, std::string(code.substr(index, end - index)));
            index = end;
        } else if (current == '"') {
            const std::size_t end = consume_string(code, index, '"', true);
            push(out, TokenKind::String, std::string(code.substr(index, end - index)));
            index = end;
        } else if (current == '\'') {
            const std::size_t end = consume_string(code, index, '\'', false);
            push(out, TokenKind::String, std::string(code.substr(index, end - index)));
            index = end;
        } else if (current == '`') {
            const std::size_t end = consume_string(code, index, '`', false);
            push(out, TokenKind::String, std::string(code.substr(index, end - index)));
            index = end;
        } else if (current == '$') {
            const std::size_t end = consume_shell_variable(code, index);
            push(out, TokenKind::Type, std::string(code.substr(index, end - index)));
            index = end;
        } else if (is_digit(current)) {
            const std::size_t end = consume_number(code, index);
            push(out, TokenKind::Number, std::string(code.substr(index, end - index)));
            index = end;
        } else if (is_ident_start(current)) {
            std::size_t end = index + 1;
            while (end < code.size() && is_ident_char(code[end])) {
                ++end;
            }
            const std::string_view word = code.substr(index, end - index);
            push(out, classify_identifier(word, shell_keywords(), nullptr),
                 std::string(word));
            index = end;
        } else {
            push(out, TokenKind::Plain, std::string(1, current));
            ++index;
        }
    }
    return out;
}

ftxui::Element style_token(const std::string& text, TokenKind kind,
                           const RenderContext& context) {
    ftxui::Element element = ftxui::text(text);
    if (!context.theme.color) {
        return element;
    }
    switch (kind) {
        case TokenKind::Keyword:
            return element | ftxui::color(ftxui::Color::Cyan);
        case TokenKind::Type:
            return element | ftxui::color(ftxui::Color::Yellow);
        case TokenKind::String:
            return element | ftxui::color(ftxui::Color::Green);
        case TokenKind::Comment:
            return element | ftxui::color(ftxui::Color::GrayDark) | ftxui::dim;
        case TokenKind::Number:
            return element | ftxui::color(ftxui::Color::Magenta);
        case TokenKind::Preprocessor:
            return element | ftxui::color(ftxui::Color::Red);
        case TokenKind::Plain:
            return element;
    }
    return element;
}

} // namespace

std::string normalize_language(std::string_view language) {
    std::string lowered;
    lowered.reserve(language.size());
    for (const char character : language) {
        lowered.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
    }
    if (lowered == "c" || lowered == "h" || lowered == "cpp" || lowered == "c++" ||
        lowered == "cxx" || lowered == "cc" || lowered == "hpp" || lowered == "hxx" ||
        lowered == "h++") {
        return "cpp";
    }
    if (lowered == "python" || lowered == "py" || lowered == "python3") {
        return "python";
    }
    if (lowered == "bash" || lowered == "sh" || lowered == "zsh" || lowered == "shell" ||
        lowered == "ksh") {
        return "shell";
    }
    return {};
}

std::vector<SyntaxToken> tokenize(std::string_view code, std::string_view language) {
    const std::string canonical = normalize_language(language);
    if (canonical == "cpp") {
        return tokenize_c_like(code);
    }
    if (canonical == "python") {
        return tokenize_python(code);
    }
    if (canonical == "shell") {
        return tokenize_shell(code);
    }
    if (code.empty()) {
        return {};
    }
    return {SyntaxToken{TokenKind::Plain, std::string(code)}};
}

ftxui::Element SyntaxRenderer::render(std::string_view code, std::string_view language,
                                      const RenderContext& context) const {
    const std::vector<SyntaxToken> tokens = tokenize(code, language);
    if (tokens.empty()) {
        return ftxui::text("");
    }

    ftxui::Elements lines;
    ftxui::Elements current;
    for (const SyntaxToken& token : tokens) {
        std::size_t start = 0;
        while (true) {
            const std::size_t newline = token.text.find('\n', start);
            const std::size_t length = newline == std::string::npos
                                           ? std::string::npos
                                           : newline - start;
            const std::string segment = token.text.substr(start, length);
            if (!segment.empty()) {
                current.push_back(style_token(segment, token.kind, context));
            }
            if (newline == std::string::npos) {
                break;
            }
            lines.push_back(ftxui::hbox(std::move(current)));
            current = ftxui::Elements{};
            start = newline + 1;
        }
    }
    lines.push_back(ftxui::hbox(std::move(current)));
    return ftxui::vbox(std::move(lines));
}

} // namespace ymh::ui
