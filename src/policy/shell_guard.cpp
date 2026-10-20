// Spec 89 (rev 3): the L1 shell-command classifier. Pure, total, deterministic,
// I/O-free: it never executes the command, never touches the filesystem, never
// resolves symlinks, never blocks, and never throws (SG1/SG12). It is a
// fail-safe clamp (89-D1): a recognized destructive verb with an ambiguous or
// unresolved operand is never `None` (SG5).

#include "ymh/policy/shell_guard.hpp"

#include <optional>

#include "ymh/execution/glob.hpp"
#include "ymh/execution/path_safety.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace ymh {
namespace {

constexpr std::size_t kMaxTokens     = 4096;
constexpr std::size_t kMaxTokenBytes = 65536;
constexpr int         kMaxDepth      = 2;

enum class TokKind : std::uint8_t {
    Word,
    Separator,   // ; && || | & and newline
    Redirect,    // > >> < << <> >& <& >| &>
    Structural,  // ( ) { }
};

struct Token {
    std::string        text;
    TokKind            kind            = TokKind::Word;
    bool               single_quoted   = false;
    bool               double_quoted   = false;
    bool               had_unexpanded  = false;   // unquoted $VAR / $(...) / `...`
};

struct Scan {
    std::vector<Token> tokens;
    bool               truncated = false;
};

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool is_op_char(char c) {
    return c == ';' || c == '&' || c == '|' || c == '<' || c == '>';
}

bool is_ident_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
}

bool is_sep(char c) {
    return is_space(c) || is_op_char(c) || c == '(' || c == ')' || c == '{' || c == '}';
}

bool starts_at(std::string_view s, std::size_t pos, std::string_view needle) {
    return pos + needle.size() <= s.size() && s.compare(pos, needle.size(), needle) == 0;
}

bool is_env_assignment(std::string_view s) {
    const std::size_t eq = s.find('=');
    if (eq == std::string_view::npos || eq == 0) {
        return false;
    }
    if (!(std::isalpha(static_cast<unsigned char>(s[0])) != 0 || s[0] == '_')) {
        return false;
    }
    for (std::size_t k = 1; k < eq; ++k) {
        if (!is_ident_char(s[k])) {
            return false;
        }
    }
    return true;
}

bool is_home_ref(std::string_view s, bool single_quoted) {
    if (s == "~" || s.rfind("~/", 0) == 0) {
        return true;
    }
    if (single_quoted) {
        return false;
    }
    return s == "$HOME" || s.rfind("$HOME/", 0) == 0 ||
           s == "${HOME}" || s.rfind("${HOME}/", 0) == 0;
}

bool is_option(std::string_view s) {
    return !s.empty() && s[0] == '-';
}

bool is_block_device(std::string_view s) {
    return s.rfind("/dev/sd", 0) == 0 || s.rfind("/dev/nvme", 0) == 0 ||
           s.rfind("/dev/hd", 0) == 0 || s.rfind("/dev/mmcblk", 0) == 0 ||
           s.rfind("/dev/loop", 0) == 0 || s.rfind("/dev/disk/", 0) == 0;
}

bool is_dev_whitelist(std::string_view s) {
    return s == "/dev/null" || s == "/dev/stdin" || s == "/dev/stdout" ||
           s == "/dev/stderr" || s == "/dev/tty" || s.rfind("/dev/fd/", 0) == 0;
}

bool is_mkfs_like(std::string_view w) {
    return w == "mkfs" || w.rfind("mkfs.", 0) == 0 || w == "mke2fs" ||
           w == "newfs" || w == "wipefs";
}

bool is_shell(std::string_view w) {
    return w == "bash" || w == "sh" || w == "dash" || w == "zsh" || w == "ksh" ||
           w == "busybox" || w == "ash";
}

bool is_output_only_redirect(std::string_view op) {
    return op == ">" || op == ">>" || op == ">|" || op == "&>";
}

bool is_read_write_redirect(std::string_view op) { return op == "<>"; }

bool is_output_redirect(std::string_view op) {
    return is_output_only_redirect(op) || is_read_write_redirect(op);
}

bool is_fork_bomb(std::string_view text) {
    return text.find(":|:") != std::string_view::npos &&
           text.find("(){") != std::string_view::npos;
}

bool contains_word(std::string_view text, std::string_view needle) {
    std::size_t pos = text.find(needle);
    while (pos != std::string_view::npos) {
        const bool left_ok = pos == 0 || !is_ident_char(text[pos - 1]);
        const std::size_t after = pos + needle.size();
        const bool right_ok = after >= text.size() || !is_ident_char(text[after]);
        if (left_ok && right_ok) {
            return true;
        }
        pos = text.find(needle, pos + 1);
    }
    return false;
}

// Tokenize (89-D2/89-D3). No execution, no globbing, no expansion, no I/O.
Scan scan(std::string_view text) {
    Scan out;
    const std::size_t n = text.size();
    std::size_t       i = 0;

    auto push = [&out](Token token) {
        out.tokens.push_back(std::move(token));
        if (out.tokens.size() >= kMaxTokens) {
            out.truncated = true;
            return false;
        }
        return true;
    };

    while (i < n) {
        const char c = text[i];
        if (is_space(c) || c == '\n') {
            ++i;
            continue;
        }
        if (c == '#') {
            while (i < n && text[i] != '\n') {
                ++i;
            }
            continue;
        }
        if (is_op_char(c)) {
            Token token;
            token.kind = TokKind::Separator;
            std::string op;
            if (c == ';' || c == '&' || c == '|') {
                if (i + 1 < n && text[i + 1] == c) {
                    op.assign(2, c);
                    i += 2;
                } else if (c == '&' && i + 1 < n && text[i + 1] == '>') {
                    op = "&>";
                    i += 2;
                    token.kind = TokKind::Redirect;
                } else {
                    op.assign(1, c);
                    ++i;
                }
            } else if (c == '>') {
                token.kind = TokKind::Redirect;
                if (i + 1 < n && text[i + 1] == '>') {
                    op = ">>";
                    i += 2;
                } else if (i + 1 < n && text[i + 1] == '&') {
                    op = ">&";
                    i += 2;
                } else if (i + 1 < n && text[i + 1] == '|') {
                    op = ">|";
                    i += 2;
                } else {
                    op = ">";
                    ++i;
                }
            } else {
                token.kind = TokKind::Redirect;
                if (i + 1 < n && text[i + 1] == '<') {
                    op = "<<";
                    i += 2;
                } else if (i + 1 < n && text[i + 1] == '>') {
                    op = "<>";
                    i += 2;
                } else if (i + 1 < n && text[i + 1] == '&') {
                    op = "<&";
                    i += 2;
                } else {
                    op = "<";
                    ++i;
                }
            }
            token.text = std::move(op);
            if (!push(std::move(token))) {
                break;
            }
            continue;
        }
        if (c == '(' || c == ')') {
            Token token;
            token.kind = TokKind::Structural;
            token.text = std::string(1, c);
            ++i;
            if (!push(std::move(token))) {
                break;
            }
            continue;
        }
        if (c == '{' || c == '}') {
            const bool isolated =
                (i == 0 || is_sep(text[i - 1])) && (i + 1 >= n || is_sep(text[i + 1]));
            if (isolated) {
                Token token;
                token.kind = TokKind::Structural;
                token.text = std::string(1, c);
                ++i;
                if (!push(std::move(token))) {
                    break;
                }
                continue;
            }
        }

        Token       token;
        std::string word;
        bool        single    = false;
        bool        double_q  = false;
        bool        saw_single = false;
        bool        saw_double = false;

        auto consume_dollar = [&](std::size_t& j, std::string& w, Token& tok) {
            const std::size_t m = text.size();
            if (starts_at(text, j, "${HOME}")) {
                const std::size_t after = j + 7;
                w += "${HOME}";
                if (!(after >= m || text[after] == '/')) {
                    tok.had_unexpanded = true;
                }
                j += 7;
                return;
            }
            if (starts_at(text, j, "$HOME")) {
                const std::size_t after = j + 5;
                if (after >= m || text[after] == '/') {
                    w += "$HOME";
                    j += 5;
                    return;
                }
                tok.had_unexpanded = true;
                w.push_back('$');
                ++j;
                return;
            }
            if (j + 1 < m && text[j + 1] == '(') {
                tok.had_unexpanded = true;
                w += "$(";
                j += 2;
                int  depth = 1;
                bool sq    = false;
                bool dq    = false;
                while (j < m && depth > 0) {
                    const char d = text[j];
                    if (sq) {
                        if (d == '\'') {
                            sq = false;
                        }
                        ++j;
                    } else if (dq) {
                        if (d == '"') {
                            dq = false;
                            ++j;
                        } else if (d == '\\') {
                            j += 2;
                        } else {
                            w.push_back(d);
                            ++j;
                        }
                    } else if (d == '\'') {
                        sq = true;
                        ++j;
                    } else if (d == '"') {
                        dq = true;
                        ++j;
                    } else if (d == '\\') {
                        j += 2;
                    } else if (d == '(') {
                        ++depth;
                        w.push_back(d);
                        ++j;
                    } else if (d == ')') {
                        --depth;
                        if (depth > 0) {
                            w.push_back(d);
                        }
                        ++j;
                    } else {
                        w.push_back(d);
                        ++j;
                    }
                }
                w.push_back(')');
                return;
            }
            if (j + 1 < m && text[j + 1] == '{') {
                tok.had_unexpanded = true;
                w += "${";
                std::size_t k     = j + 2;
                int         depth = 1;
                while (k < m && depth > 0) {
                    if (text[k] == '{') {
                        ++depth;
                    } else if (text[k] == '}') {
                        --depth;
                        if (depth == 0) {
                            ++k;
                            break;
                        }
                    }
                    w.push_back(text[k]);
                    ++k;
                }
                w.push_back('}');
                j = k;
                return;
            }
            tok.had_unexpanded = true;
            w.push_back('$');
            ++j;
        };

        auto consume_backtick = [&](std::size_t& j, std::string& w, Token& tok) {
            tok.had_unexpanded = true;
            w.push_back('`');
            ++j;
            const std::size_t m = text.size();
            while (j < m && text[j] != '`') {
                if (text[j] == '\\' && j + 1 < m) {
                    w.push_back(text[j + 1]);
                    j += 2;
                } else {
                    w.push_back(text[j]);
                    ++j;
                }
            }
            if (j < m) {
                ++j;
            }
            w.push_back('`');
        };

        bool capped = false;
        while (i < n) {
            const char d = text[i];
            if (single) {
                if (d == '\'') {
                    single = false;
                    ++i;
                } else {
                    word.push_back(d);
                    ++i;
                }
            } else if (double_q) {
                if (d == '"') {
                    double_q = false;
                    ++i;
                } else if (d == '\\') {
                    if (i + 1 < n) {
                        const char e = text[i + 1];
                        if (e == '"' || e == '\\' || e == '`' || e == '$') {
                            word.push_back(e);
                            i += 2;
                        } else if (e == '\n') {
                            i += 2;
                        } else {
                            word.push_back('\\');
                            ++i;
                        }
                    } else {
                        word.push_back('\\');
                        ++i;
                    }
                } else if (d == '$') {
                    consume_dollar(i, word, token);
                } else if (d == '`') {
                    consume_backtick(i, word, token);
                } else {
                    word.push_back(d);
                    ++i;
                }
            } else {
                if (is_space(d) || is_op_char(d) || d == '(' || d == ')') {
                    break;
                }
                if (d == '{' || d == '}') {
                    const bool isolated =
                        (i == 0 || is_sep(text[i - 1])) &&
                        (i + 1 >= n || is_sep(text[i + 1]));
                    if (isolated) {
                        break;
                    }
                    word.push_back(d);
                    token.had_unexpanded = true;
                    ++i;
                } else if (d == '\'') {
                    single     = true;
                    saw_single = true;
                    ++i;
                } else if (d == '"') {
                    double_q   = true;
                    saw_double = true;
                    ++i;
                } else if (d == '\\') {
                    if (i + 1 < n) {
                        word.push_back(text[i + 1]);
                        i += 2;
                    } else {
                        ++i;
                    }
                } else if (d == '$') {
                    consume_dollar(i, word, token);
                } else if (d == '`') {
                    consume_backtick(i, word, token);
                } else {
                    word.push_back(d);
                    ++i;
                }
            }
            if (word.size() > kMaxTokenBytes) {
                out.truncated = true;
                capped        = true;
                break;
            }
        }
        (void)capped;

        if (single || double_q) {
            out.truncated = true;
        }
        token.text          = std::move(word);
        token.single_quoted = saw_single;
        token.double_quoted = saw_double;
        if (!push(std::move(token))) {
            break;
        }
    }
    return out;
}

std::vector<std::string> collect_substitutions(std::string_view text) {
    std::vector<std::string> out;
    const std::size_t        n = text.size();
    std::size_t              i = 0;
    bool                     single   = false;
    bool                     double_q = false;

    while (i < n) {
        const char c = text[i];
        if (single) {
            if (c == '\'') {
                single = false;
            }
            ++i;
            continue;
        }
        if (double_q) {
            if (c == '"') {
                double_q = false;
                ++i;
                continue;
            }
            if (c == '\\') {
                i += 2;
                continue;
            }
        } else {
            if (c == '\'') {
                single = true;
                ++i;
                continue;
            }
            if (c == '"') {
                double_q = true;
                ++i;
                continue;
            }
            if (c == '\\') {
                i += 2;
                continue;
            }
        }
        if (c == '$' && i + 1 < n && text[i + 1] == '(') {
            std::size_t j     = i + 2;
            int         depth = 1;
            bool        sq    = false;
            bool        dq    = false;
            while (j < n && depth > 0) {
                const char d = text[j];
                if (sq) {
                    if (d == '\'') {
                        sq = false;
                    }
                    ++j;
                } else if (dq) {
                    if (d == '"') {
                        dq = false;
                        ++j;
                    } else if (d == '\\') {
                        j += 2;
                    } else {
                        ++j;
                    }
                } else if (d == '\'') {
                    sq = true;
                    ++j;
                } else if (d == '"') {
                    dq = true;
                    ++j;
                } else if (d == '\\') {
                    j += 2;
                } else if (d == '(') {
                    ++depth;
                    ++j;
                } else if (d == ')') {
                    --depth;
                    ++j;
                } else {
                    ++j;
                }
            }
            const std::size_t inner_end = depth == 0 ? j - 1 : n;
            out.emplace_back(text.substr(i + 2, inner_end - (i + 2)));
            i = j;
            continue;
        }
        if (c == '`') {
            std::size_t j = i + 1;
            while (j < n && text[j] != '`') {
                if (text[j] == '\\' && j + 1 < n) {
                    j += 2;
                } else {
                    ++j;
                }
            }
            out.emplace_back(text.substr(i + 1, j - (i + 1)));
            i = j < n ? j + 1 : n;
            continue;
        }
        ++i;
    }
    return out;
}

bool lexically_inside(const std::filesystem::path& root,
                      const std::filesystem::path& candidate) {
    const std::filesystem::path r = root.lexically_normal();
    const std::filesystem::path c = candidate.lexically_normal();
    auto                        ri = r.begin();
    auto                        ci = c.begin();
    for (; ri != r.end(); ++ri, ++ci) {
        if (ci == c.end() || *ci != *ri) {
            return false;
        }
    }
    return true;
}

enum class OpRisk : std::uint8_t { Skip, InsideBounded, Broad, Outside, Device, Ambiguous };

OpRisk classify_operand(const Token& token,
                        const std::filesystem::path& root,
                        const std::filesystem::path& cwd,
                        bool                         cwd_known) {
    const std::string s = token.text;
    if (s.empty()) {
        return OpRisk::Skip;
    }
    if (is_home_ref(s, token.single_quoted)) {
        return OpRisk::Outside;
    }
    if (token.had_unexpanded) {
        return OpRisk::Ambiguous;
    }

    const bool quoted = token.single_quoted || token.double_quoted;
    const bool glob   = !quoted && s.find_first_of("*?[") != std::string::npos;
    if (glob) {
        const std::size_t m      = s.find_first_of("*?[");
        const std::string prefix = s.substr(0, m);
        if (prefix.empty() || prefix == ".") {
            return OpRisk::Broad;
        }
        if (is_home_ref(prefix, false)) {
            return OpRisk::Outside;
        }
        const std::filesystem::path pp{prefix};
        std::filesystem::path       abs;
        if (pp.is_absolute()) {
            abs = pp;
        } else {
            if (!cwd_known) {
                return OpRisk::Ambiguous;
            }
            abs = cwd / pp;
        }
        if (abs.lexically_normal() == root.lexically_normal()) {
            return OpRisk::Broad;
        }
        return lexically_inside(root, abs) ? OpRisk::InsideBounded : OpRisk::Outside;
    }

    if (s == ".") {
        return OpRisk::Broad;
    }
    if (s == "..") {
        return OpRisk::Outside;
    }

    const std::filesystem::path pp{s};
    std::filesystem::path       abs;
    if (pp.is_absolute()) {
        abs = pp;
    } else {
        if (!cwd_known) {
            return OpRisk::Ambiguous;
        }
        abs = cwd / pp;
    }
    const std::string normalized = abs.lexically_normal().string();
    if (is_block_device(normalized)) {
        return OpRisk::Device;
    }
    if (is_dev_whitelist(normalized)) {
        return OpRisk::Skip;
    }
    return lexically_inside(root, abs) ? OpRisk::InsideBounded : OpRisk::Outside;
}

std::size_t strip_prefixes(const std::vector<Token>& toks, std::size_t b, std::size_t e) {
    std::size_t i = b;
    for (;;) {
        if (i >= e || toks[i].kind != TokKind::Word) {
            return i;
        }
        const std::string& w = toks[i].text;
        if (is_env_assignment(w)) {
            ++i;
            continue;
        }
        if (is_option(w) && w != "-") {
            ++i;
            continue;
        }
        if (w == "sudo") {
            ++i;
            while (i < e && toks[i].kind == TokKind::Word && is_option(toks[i].text)) {
                if (toks[i].text == "-u" || toks[i].text == "-g" ||
                    toks[i].text == "--user" || toks[i].text == "--group") {
                    i += 2;
                } else {
                    ++i;
                }
            }
            continue;
        }
        if (w == "env") {
            ++i;
            continue;
        }
        if (w == "nice") {
            ++i;
            while (i < e && toks[i].kind == TokKind::Word && is_option(toks[i].text)) {
                if (toks[i].text == "-n" || toks[i].text == "--adjustment") {
                    i += 2;
                } else {
                    ++i;
                }
            }
            continue;
        }
        if (w == "timeout") {
            ++i;
            while (i < e && toks[i].kind == TokKind::Word && is_option(toks[i].text)) {
                ++i;
            }
            if (i < e && toks[i].kind == TokKind::Word) {
                ++i;   // the duration operand
            }
            continue;
        }
        if (w == "command" || w == "exec" || w == "nohup" || w == "time" ||
            w == "stdbuf" || w == "setsid") {
            ++i;
            while (i < e && toks[i].kind == TokKind::Word && is_option(toks[i].text)) {
                ++i;
            }
            continue;
        }
        return i;
    }
}

std::vector<std::size_t> collect_operands(const std::vector<Token>& toks,
                                          std::size_t               cw,
                                          std::size_t               e) {
    std::vector<std::size_t> out;
    for (std::size_t k = cw + 1; k < e; ++k) {
        const Token& t = toks[k];
        if (t.kind == TokKind::Redirect) {
            ++k;   // skip the redirect target too
            continue;
        }
        if (t.kind != TokKind::Word) {
            continue;
        }
        if (is_option(t.text)) {
            continue;
        }
        out.push_back(k);
    }
    return out;
}

bool has_recursive_flag(const std::vector<Token>& toks, std::size_t cw, std::size_t e) {
    for (std::size_t k = cw + 1; k < e; ++k) {
        if (toks[k].kind != TokKind::Word) {
            continue;
        }
        const std::string& t = toks[k].text;
        if (t == "--recursive") {
            return true;
        }
        if (t.size() >= 2 && t[0] == '-' && t[1] != '-' && t.find('R') != std::string::npos) {
            return true;
        }
    }
    return false;
}

int risk_rank(ShellCommandRisk risk) {
    switch (risk) {
        case ShellCommandRisk::Forbid:
            return 2;
        case ShellCommandRisk::Confirm:
            return 1;
        case ShellCommandRisk::None:
        default:
            return 0;
    }
}

ShellCommandRisk max_risk(ShellCommandRisk a, ShellCommandRisk b) {
    return risk_rank(a) >= risk_rank(b) ? a : b;
}

ShellCommandRisk map_operand(OpRisk risk, bool inside_is_confirm, bool kernel_confinement) {
    switch (risk) {
        case OpRisk::Skip:
            return ShellCommandRisk::None;
        case OpRisk::Outside:
        case OpRisk::Device:
            return ShellCommandRisk::Forbid;
        case OpRisk::Broad:
        case OpRisk::Ambiguous:
            return ShellCommandRisk::Confirm;
        case OpRisk::InsideBounded:
        default:
            return (inside_is_confirm || !kernel_confinement) ? ShellCommandRisk::Confirm
                                                             : ShellCommandRisk::None;
    }
}

bool token_destructive(std::string_view w) {
    return w == "rm" || w == "rmdir" || w == "shred" || w == "truncate" || w == "dd" ||
           w == "chmod" || w == "chown" || w == "mv" || w == "cp" || w == "find" ||
           is_mkfs_like(w);
}

bool looks_destructive(std::string_view text) {
    const Scan s = scan(text);
    const std::size_t n = s.tokens.size();
    std::size_t       i = 0;
    while (i < n) {
        const Token& t = s.tokens[i];
        if (t.kind != TokKind::Word) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && s.tokens[j].kind == TokKind::Word) {
            ++j;
        }
        const std::size_t cw = strip_prefixes(s.tokens, i, j);
        if (cw < j && token_destructive(s.tokens[cw].text)) {
            return true;
        }
        i = j;
    }
    static constexpr std::string_view kVerbs[] = {
        "rm", "rmdir", "shred", "truncate", "dd", "mkfs", "mke2fs", "newfs",
        "wipefs", "chmod", "chown", "mv", "cp", "find"};
    for (const std::string_view verb : kVerbs) {
        if (contains_word(text, verb)) {
            return true;
        }
    }
    return false;
}

ShellCommandRisk classify_impl(std::string_view               text,
                               const std::filesystem::path&   root,
                               const std::filesystem::path&   cwd,
                               bool                           kernel_confinement,
                               int                            depth);

ShellCommandRisk classify_verb(std::string_view               cmd,
                               const std::vector<Token>&       toks,
                               std::size_t                     cw,
                               std::size_t                     e,
                               const std::filesystem::path&    root,
                               const std::filesystem::path&    cur,
                               bool                            cwd_known,
                               bool                            kernel_confinement,
                               int                             depth) {
    (void)depth;
    auto operand_risk = [&](std::size_t index, bool inside_is_confirm) {
        return map_operand(classify_operand(toks[index], root, cur, cwd_known),
                           inside_is_confirm, kernel_confinement);
    };
    auto redirect_risk = [&](std::size_t index) {
        const OpRisk o = classify_operand(toks[index], root, cur, cwd_known);
        if (o == OpRisk::Skip) {
            return ShellCommandRisk::None;
        }
        return map_operand(o, false, kernel_confinement);
    };

    if (cmd == "rm" || cmd == "rmdir" || cmd == "shred" || cmd == "truncate") {
        ShellCommandRisk best = ShellCommandRisk::None;
        for (std::size_t index : collect_operands(toks, cw, e)) {
            best = max_risk(best, operand_risk(index, false));
        }
        return best;
    }
    if (cmd == "chmod" || cmd == "chown") {
        if (!has_recursive_flag(toks, cw, e)) {
            return ShellCommandRisk::None;
        }
        ShellCommandRisk best = ShellCommandRisk::None;
        for (std::size_t index : collect_operands(toks, cw, e)) {
            best = max_risk(best, operand_risk(index, false));
        }
        return best;
    }
    if (cmd == "mv" || cmd == "cp") {
        const std::vector<std::size_t> ops = collect_operands(toks, cw, e);
        if (ops.empty()) {
            return ShellCommandRisk::None;
        }
        return operand_risk(ops.back(), false);
    }
    if (cmd == "find") {
        bool destructive = false;
        for (std::size_t k = cw + 1; k < e; ++k) {
            if (toks[k].text == "-delete" || toks[k].text == "-exec") {
                destructive = true;
                break;
            }
        }
        if (!destructive) {
            return ShellCommandRisk::None;
        }
        const std::vector<std::size_t> ops = collect_operands(toks, cw, e);
        OpRisk                         o   = OpRisk::Broad;
        if (!ops.empty()) {
            o = classify_operand(toks[ops.front()], root, cur, cwd_known);
        }
        if (o == OpRisk::Outside || o == OpRisk::Device) {
            return ShellCommandRisk::Forbid;
        }
        return ShellCommandRisk::Confirm;
    }
    if (cmd == "dd") {
        for (std::size_t k = cw + 1; k < e; ++k) {
            if (toks[k].kind != TokKind::Word || !starts_at(toks[k].text, 0, "of=")) {
                continue;
            }
            Token target;
            target.text               = toks[k].text.substr(3);
            target.had_unexpanded     = toks[k].had_unexpanded;
            target.single_quoted      = toks[k].single_quoted;
            const OpRisk o            = classify_operand(target, root, cur, cwd_known);
            if (o == OpRisk::Skip) {
                return ShellCommandRisk::None;
            }
            if (o == OpRisk::Outside || o == OpRisk::Device) {
                return ShellCommandRisk::Forbid;
            }
            return ShellCommandRisk::Confirm;
        }
        return ShellCommandRisk::None;
    }
    if (is_mkfs_like(cmd)) {
        return ShellCommandRisk::Forbid;
    }
    (void)redirect_risk;
    return ShellCommandRisk::None;
}

ShellCommandRisk classify_segment(const std::vector<Token>&     toks,
                                  std::size_t                   b,
                                  std::size_t                   e,
                                  const std::filesystem::path&  root,
                                  std::filesystem::path&        cur,
                                  bool&                         cwd_known,
                                  bool                          kernel_confinement,
                                  int                           depth) {
    ShellCommandRisk best = ShellCommandRisk::None;

    for (std::size_t k = b; k < e; ++k) {
        if (toks[k].kind != TokKind::Redirect || !is_output_redirect(toks[k].text)) {
            continue;
        }
        std::size_t t = k + 1;
        while (t < e && toks[t].kind == TokKind::Redirect) {
            ++t;
        }
        if (t < e && toks[t].kind == TokKind::Word) {
            const OpRisk o = classify_operand(toks[t], root, cur, cwd_known);
            best = max_risk(best, map_operand(o, false, kernel_confinement));
        }
    }

    const std::size_t cw = strip_prefixes(toks, b, e);
    if (cw >= e || toks[cw].kind != TokKind::Word) {
        return best;
    }
    const std::string& cmd = toks[cw].text;

    if (cmd == "cd" || cmd == "pushd") {
        std::size_t count  = 0;
        std::string operand;
        bool        unknown = false;
        for (std::size_t k = cw + 1; k < e; ++k) {
            if (toks[k].kind == TokKind::Redirect) {
                ++k;
                continue;
            }
            if (toks[k].kind != TokKind::Word) {
                continue;
            }
            if (is_option(toks[k].text)) {
                unknown = true;
                continue;
            }
            ++count;
            operand = toks[k].text;
            if (toks[k].had_unexpanded) {
                unknown = true;
            }
        }
        if (count != 1 || unknown) {
            cwd_known = false;
        } else if (is_home_ref(operand, false)) {
            cur       = std::filesystem::path{"/"};
            cwd_known = true;
        } else {
            const std::filesystem::path pp{operand};
            cur       = pp.is_absolute() ? pp.lexically_normal()
                                         : (cur / pp).lexically_normal();
            cwd_known = true;
        }
        return best;
    }

    if (is_shell(cmd)) {
        for (std::size_t k = cw + 1; k < e; ++k) {
            if (toks[k].kind != TokKind::Word) {
                break;
            }
            const std::string& opt = toks[k].text;
            const bool         short_flag =
                opt.size() >= 2 && opt[0] == '-' && opt[1] != '-';
            if (!short_flag || opt.find('c') == std::string::npos) {
                continue;
            }
            std::size_t t = k + 1;
            while (t < e && toks[t].kind != TokKind::Word) {
                ++t;
            }
            if (t >= e) {
                break;
            }
            const std::string& inner = toks[t].text;
            if (depth >= kMaxDepth) {
                return max_risk(best, looks_destructive(inner) ? ShellCommandRisk::Confirm
                                                               : ShellCommandRisk::None);
            }
            return max_risk(best, classify_impl(inner, root, cur, kernel_confinement, depth + 1));
        }
        return best;
    }

    best = max_risk(best, classify_verb(cmd, toks, cw, e, root, cur, cwd_known,
                                        kernel_confinement, depth));
    return best;
}

ShellCommandRisk classify_impl(std::string_view             text,
                               const std::filesystem::path& root,
                               const std::filesystem::path& cwd,
                               bool                         kernel_confinement,
                               int                          depth) {
    if (text.empty()) {
        return ShellCommandRisk::None;
    }
    if (is_fork_bomb(text)) {
        return ShellCommandRisk::Forbid;
    }

    const Scan        s   = scan(text);
    ShellCommandRisk  best = ShellCommandRisk::None;
    std::filesystem::path cur = cwd;
    bool              cwd_known = true;

    const std::size_t n = s.tokens.size();
    std::size_t       i = 0;
    while (i < n) {
        const Token& t = s.tokens[i];
        if (t.kind == TokKind::Separator || t.kind == TokKind::Structural) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && s.tokens[j].kind != TokKind::Separator &&
               s.tokens[j].kind != TokKind::Structural) {
            ++j;
        }
        best = max_risk(best,
                        classify_segment(s.tokens, i, j, root, cur, cwd_known,
                                         kernel_confinement, depth));
        i = j;
    }

    for (const std::string& sub : collect_substitutions(text)) {
        if (depth >= kMaxDepth) {
            if (looks_destructive(sub)) {
                best = max_risk(best, ShellCommandRisk::Confirm);
            }
        } else {
            best = max_risk(best, classify_impl(sub, root, cur, kernel_confinement, depth + 1));
        }
    }

    if (s.truncated && looks_destructive(text)) {
        best = max_risk(best, ShellCommandRisk::Confirm);
    }
    return best;
}

// ---------------------------------------------------------------------------
// Spec 90: additive shell target scope (90-D5/90-D17/90-D19..D22)
// ---------------------------------------------------------------------------

#include <optional>

struct ScopeResult {
    ShellTargetScope scope = ShellTargetScope::InRoot;
    bool             protected_target = false;
};

std::string basename_of(std::string_view word) {
    const std::size_t slash = word.find_last_of('/');
    return std::string{slash == std::string_view::npos ? word : word.substr(slash + 1)};
}

bool has_glob(std::string_view text) {
    return text.find_first_of("*?") != std::string_view::npos;
}

bool component_is_protected(std::string_view component) {
    if (component == ".ymh" || component == ".git") {
        return true;
    }
    if (!has_glob(component)) {
        return false;
    }
    const std::size_t glob = component.find_first_of("*?");
    if (glob == std::string_view::npos || glob == 0) {
        return false;
    }
    const std::string_view prefix = component.substr(0, glob);
    if (prefix.empty() || prefix.front() != '.') {
        return false;
    }
    return glob_match(component, ".ymh") || glob_match(component, ".git");
}

bool path_components_protected(const std::filesystem::path& relative) {
    for (const std::filesystem::path& component : relative) {
        if (component_is_protected(component.string())) {
            return true;
        }
    }
    return false;
}

bool resolve_lexical(const std::filesystem::path& root,
                     const std::filesystem::path& cwd,
                     std::string_view             text,
                     std::filesystem::path&       out) {
    if (text.empty()) {
        return false;
    }
    const std::filesystem::path candidate{std::string{text}};
    out = (candidate.is_absolute() ? candidate : (cwd / candidate)).lexically_normal();
    (void)root;
    return true;
}

bool candidate_is_protected(const std::filesystem::path& root,
                            const std::filesystem::path& cwd,
                            std::string_view             text) {
    if (text.empty()) {
        return false;
    }
    std::filesystem::path target;
    if (!resolve_lexical(root, cwd, text, target)) {
        return false;
    }
    if (!lexically_inside(root, target)) {
        return false;
    }
    const std::filesystem::path relative = target.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) {
        return false;
    }
    return path_components_protected(relative);
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return 10 + (c - 'a');
    }
    if (c >= 'A' && c <= 'F') {
        return 10 + (c - 'A');
    }
    return -1;
}

// Parses a `file:` URI. Returns false when the token is not a file URI;
// `unknown` is set for a remote/malformed authority or percent escape.
bool parse_file_uri(std::string_view text, std::string& decoded, bool& unknown) {
    unknown = false;
    if (text.size() < 5) {
        return false;
    }
    const std::string scheme = [&] {
        std::string value;
        for (char c : text.substr(0, 5)) {
            value.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return value;
    }();
    if (scheme != "file:") {
        return false;
    }
    std::string_view rest = text.substr(5);
    std::string_view path = rest;
    if (rest.starts_with("//")) {
        const std::size_t slash = rest.find('/', 2);
        const std::string_view host =
            slash == std::string_view::npos ? rest.substr(2)
                                            : rest.substr(2, slash - 2);
        if (!host.empty() && host != "localhost") {
            unknown = true;
            return true;
        }
        path = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash);
    }
    for (std::size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '%') {
            decoded.push_back(path[i]);
            continue;
        }
        if (i + 2 >= path.size()) {
            unknown = true;
            return true;
        }
        const int hi = hex_value(path[i + 1]);
        const int lo = hex_value(path[i + 2]);
        if (hi < 0 || lo < 0) {
            unknown = true;
            return true;
        }
        decoded.push_back(static_cast<char>((hi << 4) | lo));
        i += 2;
    }
    return true;
}

bool token_is_known_uri(std::string_view text, std::string_view& scheme) {
    const std::size_t colon = text.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return false;
    }
    for (std::size_t i = 0; i < colon; ++i) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const bool alpha = std::isalpha(c) != 0;
        if (i == 0 && !alpha) {
            return false;
        }
        if (i > 0 && !alpha && !std::isdigit(c) && text[i] != '+' && text[i] != '-' &&
            text[i] != '.') {
            return false;
        }
    }
    std::string lowered;
    for (char c : text.substr(0, colon)) {
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    static constexpr std::string_view kKnown[] = {
        "file", "http", "https", "ssh", "git", "ftp", "ftps", "sftp", "ws", "wss"};
    for (std::string_view candidate : kKnown) {
        if (lowered == candidate) {
            scheme = candidate;
            return true;
        }
    }
    if (text.substr(colon + 1).starts_with("//")) {
        scheme = lowered;
        return true;
    }
    return false;
}

std::optional<ShellTargetScope> classify_scope_path(
    std::string_view text, const Token& token, const std::filesystem::path& root,
    const std::filesystem::path& cwd, bool force_path, bool git_context) {
    std::string s{text};
    if (s.empty()) {
        return std::nullopt;
    }

    // URI schemes: `file:` is a local target; other known schemes are neutral.
    std::string_view scheme;
    if (token_is_known_uri(s, scheme)) {
        if (scheme == "file") {
            std::string decoded;
            bool        unknown = false;
            if (!parse_file_uri(s, decoded, unknown)) {
                return ShellTargetScope::Unknown;
            }
            if (unknown || decoded.empty()) {
                return ShellTargetScope::Unknown;
            }
            std::filesystem::path target;
            resolve_lexical(root, cwd, decoded, target);
            return lexically_inside(root, target) ? ShellTargetScope::InRoot
                                                  : ShellTargetScope::Outside;
        }
        return std::nullopt;
    }
    // scp-style `host:/path`: classify the part after the first colon.
    const std::size_t colon = s.find(':');
    if (colon != std::string::npos && colon + 1 < s.size() &&
        scheme.empty() && s.find('/') != std::string::npos) {
        const std::string_view after = std::string_view{s}.substr(colon + 1);
        if (!after.empty() && (after.front() == '/' || after.front() == '.' ||
                               after.find('/') != std::string_view::npos)) {
            return classify_scope_path(after, token, root, cwd, true, git_context);
        }
    }

    if (s.front() == '@') {
        if (s == "@-") {
            return std::nullopt;
        }
        if (s.size() == 1) {
            return ShellTargetScope::Unknown;   // malformed curl local-file form
        }
        s.erase(0, 1);
    } else {
        // 90-D20: curl-form `name=@path` / `name=<path` / `name@path`.
        const std::size_t at = s.find('@');
        if (at != std::string::npos && at > 0 && at + 1 == s.size()) {
            return ShellTargetScope::Unknown;   // malformed `name@`
        }
        if (at != std::string::npos && at > 0 && at + 1 < s.size()) {
            const std::string_view remainder = std::string_view{s}.substr(at + 1);
            if (remainder != "-" &&
                (remainder.front() == '/' || remainder.find('/') != std::string_view::npos ||
                 remainder.starts_with(".."))) {
                return classify_scope_path(remainder, token, root, cwd, true,
                                           git_context);
            }
        }
    }
    if (is_env_assignment(s)) {
        const std::size_t eq = s.find('=');
        std::string       value = s.substr(eq + 1);
        if (value == "@" || value == "<") {
            return ShellTargetScope::Unknown;
        }
        if (!value.empty() && (value.front() == '@' || value.front() == '<')) {
            value.erase(0, 1);
        } else if (value.find('/') == std::string::npos &&
                   value.find('~') == std::string::npos &&
                   value.find('$') == std::string::npos &&
                   !value.starts_with("..")) {
            return std::nullopt;
        }
        return classify_scope_path(value, token, root, cwd, true, git_context);
    }

    if (s.size() > 1 && s.front() == '-') {
        if (s.starts_with("--")) {
            const std::size_t eq = s.find('=');
            if (eq == std::string::npos) {
                return std::nullopt;
            }
            return classify_scope_path(std::string_view{s}.substr(eq + 1), token, root,
                                       cwd, false, git_context);
        }
        const char        option = s[1];
        const std::string letters = git_context ? "tCfbBcCutmM" : "tCf";
        if (s.size() > 2 && letters.find(option) != std::string::npos) {
            return classify_scope_path(std::string_view{s}.substr(2), token, root, cwd,
                                       true, git_context);
        }
        if (s.find('/') != std::string::npos || s.find('~') != std::string::npos ||
            s.find('$') != std::string::npos || s.find("..") != std::string::npos) {
            return ShellTargetScope::Unknown;
        }
        return std::nullopt;
    }

    if (is_home_ref(s, token.single_quoted)) {
        return ShellTargetScope::Outside;
    }
    if (token.had_unexpanded && s.find('$') != std::string::npos) {
        return ShellTargetScope::Unknown;
    }

    const bool path_punctuation =
        s == "." || s == ".." || s.find('/') != std::string::npos ||
        has_glob(s) || s.front() == '~' || s.front() == '$' || s.starts_with("..");
    if (!force_path && !path_punctuation) {
        return std::nullopt;
    }

    if (has_glob(s)) {
        const std::size_t glob = s.find_first_of("*?");
        std::string       prefix = s.substr(0, glob);
        if (prefix.empty()) {
            return std::nullopt;   // bare `*`/`**`: no directional evidence
        }
        std::filesystem::path probe;
        if (!resolve_lexical(root, cwd, prefix, probe)) {
            return ShellTargetScope::Unknown;
        }
        return lexically_inside(root, probe) ? ShellTargetScope::InRoot
                                             : ShellTargetScope::Outside;
    }
    std::filesystem::path target;
    resolve_lexical(root, cwd, s, target);
    if (target == root.lexically_normal()) {
        return ShellTargetScope::InRoot;
    }
    return lexically_inside(root, target) ? ShellTargetScope::InRoot
                                          : ShellTargetScope::Outside;
}

void note_scope(ScopeResult& result, ShellTargetScope scope) {
    if (scope == ShellTargetScope::Outside) {
        result.scope = ShellTargetScope::Outside;
    } else if (scope == ShellTargetScope::Unknown &&
               result.scope != ShellTargetScope::Outside) {
        result.scope = ShellTargetScope::Unknown;
    }
}

bool scan_token_protected(const std::string& token_text, const std::filesystem::path& root,
                          const std::filesystem::path& cwd) {
    if (candidate_is_protected(root, cwd, token_text)) {
        return true;
    }
    std::string fragment;
    const auto flush = [&]() {
        if (!fragment.empty()) {
            if (candidate_is_protected(root, cwd, fragment)) {
                return true;
            }
            fragment.clear();
        }
        return false;
    };
    for (char c : token_text) {
        const bool boundary = std::isspace(static_cast<unsigned char>(c)) != 0 ||
                              c == '=' || c == '@' || c == '<' || c == '>' ||
                              c == ':' || c == '"' || c == '\'' || c == '`' ||
                              c == ';' || c == '&' || c == '|' || c == '(' ||
                              c == ')' || c == '{' || c == '}';
        if (boundary) {
            if (flush()) {
                return true;
            }
            continue;
        }
        fragment.push_back(c);
    }
    return flush();
}

struct GitScope {
    std::optional<ShellTargetScope> scope;
    bool                            protected_target = false;
};

GitScope classify_git_subcommand(const std::vector<Token>& toks, std::size_t cw,
                                 std::size_t e, const std::filesystem::path& root,
                                 const std::filesystem::path& cwd) {
    GitScope out;
    std::filesystem::path git_cwd = cwd;

    auto is_word = [&](std::size_t i) {
        return i < e && toks[i].kind == TokKind::Word;
    };
    auto classify_cwd = [&](const std::string& path) -> bool {
        std::filesystem::path target;
        if (!resolve_lexical(root, git_cwd, path, target)) {
            return false;
        }
        if (!lexically_inside(root, target)) {
            out.scope = ShellTargetScope::Outside;
            return false;
        }
        if (path_components_protected(target.lexically_relative(root))) {
            out.protected_target = true;
        }
        git_cwd = target;
        return true;
    };

    std::size_t i = cw + 1;
    while (i < e) {
        if (!is_word(i)) {
            ++i;
            continue;
        }
        const std::string& w = toks[i].text;
        if (w == "-C" || w == "--git-dir" || w == "--work-tree" ||
            w == "--namespace" || w == "--exec-path" || w == "--config-env" ||
            w == "-c") {
            if (i + 1 < e && is_word(i + 1)) {
                const std::string& value = toks[i + 1].text;
                if (w == "-C" || w == "--git-dir" || w == "--work-tree") {
                    if (!value.empty() && value.front() != '-') {
                        if (!classify_cwd(value)) {
                            return out;
                        }
                    }
                }
                i += 2;
                continue;
            }
            ++i;
            continue;
        }
        if (w.starts_with("--git-dir=") || w.starts_with("--work-tree=") ||
            w.starts_with("-C")) {
            std::string value;
            if (w.starts_with("-C") && !w.starts_with("--")) {
                value = w.substr(2);
            } else {
                value = w.substr(w.find('=') + 1);
            }
            if (!value.empty() && !classify_cwd(value)) {
                return out;
            }
            ++i;
            continue;
        }
        if (is_option(w)) {
            ++i;
            continue;
        }
        break;   // subcommand
    }
    if (i >= e) {
        return out;
    }
    if (out.scope == ShellTargetScope::Outside || out.scope == ShellTargetScope::Unknown) {
        return out;
    }
    if (out.protected_target) {
        return out;
    }

    const std::string sub = toks[i].text;
    std::vector<std::string> args;
    for (std::size_t k = i + 1; k < e; ++k) {
        if (toks[k].kind == TokKind::Word) {
            args.push_back(toks[k].text);
        }
    }
    auto any_arg = [&](std::string_view needle) {
        return std::find(args.begin(), args.end(), needle) != args.end();
    };
    auto positional = [&]() {
        for (const std::string& value : args) {
            if (!value.empty() && value.front() != '-') {
                return true;
            }
        }
        return false;
    };

    if (sub == "config") {
        if (any_arg("--get") || any_arg("--get-all") || any_arg("--get-regexp") ||
            any_arg("--list")) {
            return out;
        }
        if (any_arg("--global") || any_arg("--system")) {
            out.scope = ShellTargetScope::Outside;
            return out;
        }
        out.protected_target = true;
        return out;
    }
    if (sub == "clean") {
        out.protected_target = true;
        return out;
    }
    if (sub == "submodule") {
        if (args.empty() || args.front() == "status" || args.front() == "summary") {
            return out;
        }
        out.protected_target = true;
        return out;
    }
    if (sub == "worktree") {
        if (!args.empty() && args.front() == "list") {
            return out;
        }
        out.protected_target = true;
        return out;
    }
    if (sub == "remote") {
        static constexpr std::string_view kMutations[] = {
            "add", "remove", "rename", "set-url", "set-branches"};
        for (std::string_view mutation : kMutations) {
            if (!args.empty() && args.front() == mutation) {
                out.protected_target = true;
                return out;
            }
        }
        return out;   // -v/show/get-url/prune/list: read-only
    }
    if (sub == "stash") {
        bool untracked = any_arg("--all") || any_arg("--include-untracked");
        for (const std::string& value : args) {
            if (value.size() >= 2 && value[0] == '-' && value[1] != '-') {
                for (std::size_t k = 1; k < value.size(); ++k) {
                    if (value[k] == 'a' || value[k] == 'u') {
                        untracked = true;
                    }
                }
            }
        }
        if (untracked) {
            out.protected_target = true;
        }
        return out;
    }
    if (sub == "branch") {
        const bool no_track = any_arg("--no-track");
        for (const std::string& value : args) {
            if (value == "-m" || value == "-M" || value == "-c" || value == "-C" ||
                value == "--move" || value == "--copy" || value == "-d" ||
                value == "-D" || value == "--delete" || value == "--track" ||
                value == "-t" || value.starts_with("--set-upstream-to") ||
                value == "--unset-upstream" || value == "--edit-description" ||
                value == "-u") {
                out.protected_target = true;
                return out;
            }
            if (value.size() > 2 && value[0] == '-' && value[1] != '-') {
                const char option = value[1];
                if (option == 'm' || option == 'M' || option == 'c' || option == 'C' ||
                    option == 'u' || option == 't') {
                    out.protected_target = true;
                    return out;
                }
            }
        }
        if (positional() && !no_track) {
            out.protected_target = true;
        }
        return out;
    }
    if (sub == "checkout" || sub == "switch") {
        const bool no_track = any_arg("--no-track");
        bool       creation = false;
        bool       tracking = false;
        for (const std::string& value : args) {
            if (value == "-b" || value == "-B" || value == "-c" || value == "-C" ||
                value == "--create" || value == "--force-create" ||
                value.starts_with("--create=") || value.starts_with("--force-create=")) {
                creation = true;
            }
            if (value == "--track" || value == "-t") {
                tracking = true;
            }
            if (value.size() > 2 && value[0] == '-' && value[1] != '-') {
                const char option = value[1];
                if (option == 'b' || option == 'B' || option == 'c' || option == 'C') {
                    creation = true;
                }
                if (option == 't') {
                    tracking = true;
                }
            }
        }
        if (tracking || (creation && !no_track)) {
            out.protected_target = true;
        }
        return out;
    }
    if (sub == "fetch" || sub == "pull") {
        if (any_arg("--set-upstream")) {
            out.protected_target = true;
        }
        return out;
    }
    if (sub == "push") {
        if (any_arg("--set-upstream") || any_arg("-u")) {
            out.protected_target = true;
        }
        return out;
    }
    static constexpr std::string_view kDataPlane[] = {
        "status", "diff", "log", "show", "rev-parse", "ls-files", "ls-tree",
        "cat-file", "blame", "grep", "describe", "shortlog", "whatchanged",
        "add", "commit", "restore", "reset", "merge", "rebase", "cherry-pick",
        "revert", "gc", "prune", "repack", "tag"};
    for (std::string_view candidate : kDataPlane) {
        if (sub == candidate) {
            return out;
        }
    }
    out.scope = ShellTargetScope::Unknown;
    return out;
}

ScopeResult scope_impl(std::string_view text, const std::filesystem::path& root,
                       const std::filesystem::path& cwd, bool kernel_confinement);

ScopeResult classify_segment_scope(const std::vector<Token>& toks, std::size_t b,
                                   std::size_t e, const std::filesystem::path& root,
                                   std::filesystem::path& cur, bool cwd_known,
                                   bool kernel_confinement) {
    ScopeResult result;
    if (b >= e) {
        return result;
    }
    for (std::size_t k = b; k < e; ++k) {
        if (toks[k].kind == TokKind::Redirect && k + 1 < e &&
            toks[k + 1].kind == TokKind::Word) {
            const auto scope = classify_scope_path(toks[k + 1].text, toks[k + 1], root, cur,
                                                   true, false);
            if (scope.has_value()) {
                note_scope(result, *scope);
            }
            ++k;
        }
    }

    std::size_t cw = strip_prefixes(toks, b, e);
    if (cw >= e || toks[cw].kind != TokKind::Word) {
        return result;
    }
    const std::string executable = basename_of(toks[cw].text);

    if (executable == "cd") {
        std::size_t operand = e;
        for (std::size_t k = cw + 1; k < e; ++k) {
            if (toks[k].kind == TokKind::Word && !is_option(toks[k].text)) {
                operand = k;
                break;
            }
        }
        if (operand == e) {
            return result;
        }
        const auto scope = classify_scope_path(toks[operand].text, toks[operand], root, cur,
                                               true, false);
        if (scope.has_value()) {
            note_scope(result, *scope);
            if (*scope == ShellTargetScope::InRoot) {
                std::filesystem::path target;
                if (resolve_lexical(root, cur, toks[operand].text, target)) {
                    cur = target;
                }
            }
        }
        return result;
    }

    static constexpr std::string_view kInterpreterUnknown[] = {
        "python", "python3", "node", "nodejs", "ruby", "perl", "php", "lua", "awk", "sed"};
    for (std::string_view interpreter : kInterpreterUnknown) {
        if (executable == interpreter) {
            note_scope(result, ShellTargetScope::Unknown);
            return result;
        }
    }
    if (executable == "xargs") {
        note_scope(result, ShellTargetScope::Unknown);
        return result;
    }
    if (executable == "find") {
        for (std::size_t k = cw + 1; k < e; ++k) {
            if (toks[k].kind == TokKind::Word &&
                (toks[k].text == "-exec" || toks[k].text == "-execdir" ||
                 toks[k].text == "-ok")) {
                note_scope(result, ShellTargetScope::Unknown);
                return result;
            }
        }
    }
    static constexpr std::string_view kNestedShells[] = {
        "sh", "bash", "zsh", "dash", "ksh", "fish"};
    for (std::string_view shell : kNestedShells) {
        if (executable == shell) {
            for (std::size_t k = cw + 1; k + 1 < e; ++k) {
                if (toks[k].kind == TokKind::Word && toks[k].text == "-c" &&
                    toks[k + 1].kind == TokKind::Word) {
                    const ShellCommandRisk inner_risk =
                        classify_impl(toks[k + 1].text, root, cur, kernel_confinement, 1);
                    if (inner_risk == ShellCommandRisk::Forbid) {
                        note_scope(result, ShellTargetScope::Outside);
                    } else if (inner_risk == ShellCommandRisk::Confirm) {
                        note_scope(result, ShellTargetScope::Unknown);
                    } else {
                        const ScopeResult inner = scope_impl(toks[k + 1].text, root, cur,
                                                             kernel_confinement);
                        note_scope(result, inner.scope);
                        result.protected_target = result.protected_target || inner.protected_target;
                    }
                    return result;
                }
            }
            return result;
        }
    }

    if (executable == "git") {
        const GitScope git = classify_git_subcommand(toks, cw, e, root, cur);
        if (git.scope.has_value()) {
            note_scope(result, *git.scope);
        }
        result.protected_target = result.protected_target || git.protected_target;
        return result;
    }

    const bool git_context = false;
    for (std::size_t k = cw + 1; k < e; ++k) {
        if (toks[k].kind == TokKind::Redirect) {
            continue;
        }
        if (toks[k].kind != TokKind::Word) {
            continue;
        }
        const std::string& word = toks[k].text;
        if (is_option(word)) {
            const auto scope = classify_scope_path(word, toks[k], root, cur, false,
                                                   git_context);
            if (scope.has_value()) {
                note_scope(result, *scope);
            }
            continue;
        }
        const auto scope = classify_scope_path(word, toks[k], root, cur, false, git_context);
        if (scope.has_value()) {
            note_scope(result, *scope);
        }
    }
    (void)cwd_known;
    return result;
}

ScopeResult scope_impl(std::string_view text, const std::filesystem::path& root,
                       const std::filesystem::path& cwd, bool kernel_confinement) {
    ScopeResult result;
    if (text.empty()) {
        return result;
    }
    const Scan s = scan(text);
    std::filesystem::path cur = cwd;
    bool cwd_known = true;
    const std::size_t n = s.tokens.size();
    std::size_t i = 0;
    while (i < n) {
        const Token& t = s.tokens[i];
        if (t.kind == TokKind::Separator || t.kind == TokKind::Structural) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && s.tokens[j].kind != TokKind::Separator &&
               s.tokens[j].kind != TokKind::Structural) {
            ++j;
        }
        const ScopeResult segment =
            classify_segment_scope(s.tokens, i, j, root, cur, cwd_known, kernel_confinement);
        note_scope(result, segment.scope);
        result.protected_target = result.protected_target || segment.protected_target;
        i = j;
    }
    if (s.truncated) {
        note_scope(result, ShellTargetScope::Unknown);
    }
    return result;
}


// ---------------------------------------------------------------------------
// Spec 90 (Rev 9/13/16): the I/O-free temp-root lexical precursor
// ---------------------------------------------------------------------------

struct TempInfo {
    std::vector<std::filesystem::path> targets;
    bool                               has_write = false;
    bool                               has_read = false;
    bool                               has_unknown = false;
    bool                               concrete = true;
    bool                               protected_target = false;
    bool                               untargeted_forbid = false;
};

enum class TempRole : std::uint8_t { Write, Read, Neutral };

bool temp_has_dotdot(std::string_view raw) {
    std::size_t start = 0;
    for (;;) {
        const std::size_t slash = raw.find('/', start);
        const std::string_view component =
            slash == std::string_view::npos ? raw.substr(start)
                                            : raw.substr(start, slash - start);
        if (component == "..") {
            return true;
        }
        if (slash == std::string_view::npos) {
            return false;
        }
        start = slash + 1;
    }
}

bool temp_token_concrete(const Token& token) {
    if (token.text.empty() || token.had_unexpanded) {
        return false;
    }
    if (token.text.find('{') != std::string::npos ||
        token.text.find('}') != std::string::npos ||
        token.text.find_first_of("*?[") != std::string::npos ||
        token.text.find('$') != std::string::npos ||
        token.text.find('`') != std::string::npos ||
        token.text.find('~') != std::string::npos) {
        return false;
    }
    return !temp_has_dotdot(token.text);
}

void temp_add_target(TempInfo& info, const Token& token,
                     const std::filesystem::path& cwd,
                     const std::filesystem::path& root, TempRole role) {
    if (!temp_token_concrete(token)) {
        info.concrete = false;
        return;
    }
    const std::filesystem::path candidate{token.text};
    std::filesystem::path       absolute =
        candidate.is_absolute() ? candidate : (cwd / candidate);
    absolute = absolute.lexically_normal();
    info.targets.push_back(absolute);
    if (path_is_protected(root, absolute)) {
        info.protected_target = true;
    }
    if (role == TempRole::Write) {
        info.has_write = true;
    } else if (role == TempRole::Read) {
        info.has_read = true;
    }
}

bool temp_path_like(std::string_view text) {
    return !text.empty() && (text[0] == '/' || text[0] == '.' ||
                             text.find('/') != std::string_view::npos);
}

bool temp_option_takes_value(std::string_view option,
                             std::initializer_list<std::string_view> valued) {
    for (const std::string_view name : valued) {
        if (option == name) {
            return true;
        }
    }
    return false;
}

std::vector<std::size_t> temp_operands(
    const std::vector<Token>& toks, std::size_t cw, std::size_t e,
    std::initializer_list<std::string_view> valued_options) {
    std::vector<std::size_t> out;
    for (std::size_t k = cw + 1; k < e; ++k) {
        const Token& token = toks[k];
        if (token.kind == TokKind::Redirect) {
            ++k;
            continue;
        }
        if (token.kind != TokKind::Word) {
            continue;
        }
        if (is_option(token.text)) {
            if (temp_option_takes_value(token.text, valued_options) && k + 1 < e) {
                ++k;
            }
            continue;
        }
        if (token.text == "--") {
            continue;
        }
        out.push_back(k);
    }
    return out;
}

bool temp_has_short_flag(const std::vector<Token>& toks, std::size_t cw,
                         std::size_t e, std::string_view letters) {
    for (std::size_t k = cw + 1; k < e; ++k) {
        if (toks[k].kind != TokKind::Word) {
            continue;
        }
        const std::string& text = toks[k].text;
        if (text.size() >= 2 && text[0] == '-' && text[1] != '-') {
            for (const char letter : letters) {
                if (text.find(letter) != std::string::npos) {
                    return true;
                }
            }
        }
    }
    return false;
}

void temp_run(std::string_view text, const std::filesystem::path& root,
              std::filesystem::path cwd, TempInfo& info, int depth);

void temp_segment(const std::vector<Token>& toks, std::size_t b, std::size_t e,
                  const std::filesystem::path& root, std::filesystem::path& cur,
                  TempInfo& info, int depth) {
    if (b >= e || depth > 2) {
        if (depth > 2) {
            info.has_unknown = true;
        }
        return;
    }
    bool output_redirect = false;
    for (std::size_t k = b; k < e; ++k) {
        if (toks[k].kind != TokKind::Redirect) {
            continue;
        }
        const std::string& op = toks[k].text;
        if (k + 1 >= e || toks[k + 1].kind != TokKind::Word) {
            info.has_unknown = true;
            continue;
        }
        const Token& target = toks[k + 1];
        if (op == "<") {
            temp_add_target(info, target, cur, root, TempRole::Read);
        } else if (op == "<<") {
            info.has_unknown = true;
            info.concrete = false;
        } else if (is_read_write_redirect(op)) {
            temp_add_target(info, target, cur, root, TempRole::Read);
        } else if (is_output_only_redirect(op)) {
            output_redirect = true;
            temp_add_target(info, target, cur, root, TempRole::Neutral);
        }
        ++k;
    }

    const std::size_t cw = strip_prefixes(toks, b, e);
    if (cw >= e || toks[cw].kind != TokKind::Word) {
        if (output_redirect) {
            info.has_unknown = true;
        }
        return;
    }
    const std::string executable = basename_of(toks[cw].text);
    const bool harmless_producer = executable == "printf" || executable == "echo" ||
                                   executable == ":" || executable == "true";

    if (output_redirect) {
        if (harmless_producer) {
            info.has_write = true;
        } else {
            info.has_unknown = true;
        }
    }

    auto add_operands = [&](TempRole role,
                            std::initializer_list<std::string_view> valued) {
        for (const std::size_t index : temp_operands(toks, cw, e, valued)) {
            if (role == TempRole::Read && !temp_path_like(toks[index].text)) {
                continue;
            }
            temp_add_target(info, toks[index], cur, root, role);
        }
    };

    if (executable == "cd" || executable == "pushd") {
        std::optional<std::size_t> operand;
        for (std::size_t k = cw + 1; k < e; ++k) {
            if (toks[k].kind == TokKind::Redirect) {
                ++k;
                continue;
            }
            if (toks[k].kind != TokKind::Word || is_option(toks[k].text)) {
                continue;
            }
            if (operand.has_value()) {
                info.has_unknown = true;
                return;
            }
            operand = k;
        }
        if (!operand.has_value() || !temp_token_concrete(toks[*operand])) {
            info.concrete = false;
            info.has_unknown = true;
            return;
        }
        const std::filesystem::path target{toks[*operand].text};
        cur = (target.is_absolute() ? target : (cur / target)).lexically_normal();
        return;
    }

    if (is_mkfs_like(executable)) {
        info.untargeted_forbid = true;
        info.has_unknown = true;
        return;
    }

    if (is_shell(executable)) {
        for (std::size_t k = cw + 1; k + 1 < e; ++k) {
            const std::string& option = toks[k].text;
            const bool short_flag =
                option.size() >= 2 && option[0] == '-' && option[1] != '-';
            if (short_flag && option.find('c') != std::string::npos &&
                toks[k + 1].kind == TokKind::Word) {
                temp_run(toks[k + 1].text, root, cur, info, depth + 1);
                return;
            }
        }
        info.has_unknown = true;
        return;
    }

    static constexpr std::string_view kInterpreters[] = {
        "python", "python3", "node", "nodejs", "ruby", "perl",
        "php",    "lua",     "awk",  "sed",    "xargs"};
    for (const std::string_view interpreter : kInterpreters) {
        if (executable == interpreter) {
            info.has_unknown = true;
            return;
        }
    }

    if (executable == "find") {
        for (std::size_t k = cw + 1; k < e; ++k) {
            if (toks[k].kind != TokKind::Word) {
                continue;
            }
            const std::string& word = toks[k].text;
            if (word == "-L" || word == "-H" || word == "--follow" ||
                word == "-delete" || word == "-exec" || word == "-execdir" ||
                word == "-ok" || word == "-okdir") {
                info.has_unknown = true;
                return;
            }
        }
        add_operands(TempRole::Read, {});
        info.has_read = true;
        return;
    }

    if (executable == "rm") {
        if (temp_has_short_flag(toks, cw, e, "rR") ||
            std::any_of(toks.begin() + static_cast<std::ptrdiff_t>(cw + 1),
                        toks.begin() + static_cast<std::ptrdiff_t>(e),
                        [](const Token& token) {
                            return token.kind == TokKind::Word &&
                                   token.text == "--recursive";
                        })) {
            info.has_unknown = true;
            return;
        }
        add_operands(TempRole::Write, {});
        return;
    }

    if (executable == "mkdir") {
        add_operands(TempRole::Write, {"-m", "--mode"});
        return;
    }
    if (executable == "touch") {
        add_operands(TempRole::Write,
                     {"-t", "-d", "-r", "--date", "--reference", "--time"});
        return;
    }
    if (executable == "truncate") {
        add_operands(TempRole::Write, {"-s", "-r", "--size", "--reference"});
        return;
    }
    if (executable == "tee") {
        add_operands(TempRole::Write, {});
        return;
    }
    if (executable == "mv") {
        add_operands(TempRole::Write, {"-t", "-S", "--target-directory", "--suffix"});
        return;
    }
    if (executable == "dd") {
        bool wrote = false;
        for (std::size_t k = cw + 1; k < e; ++k) {
            if (toks[k].kind != TokKind::Word) {
                continue;
            }
            const std::string& word = toks[k].text;
            if (starts_at(word, 0, "of=")) {
                Token value = toks[k];
                value.text = word.substr(3);
                temp_add_target(info, value, cur, root, TempRole::Write);
                wrote = true;
            } else if (starts_at(word, 0, "if=")) {
                Token value = toks[k];
                value.text = word.substr(3);
                temp_add_target(info, value, cur, root, TempRole::Read);
            }
        }
        if (!wrote) {
            info.has_unknown = true;
        }
        return;
    }
    if (executable == "cp" || executable == "install" || executable == "ln") {
        add_operands(TempRole::Read, {"-t", "-S", "--target-directory", "--suffix"});
        info.has_read = true;
        return;
    }
    if (executable == "chmod" || executable == "chown" || executable == "rsync" ||
        executable == "rmdir" || executable == "shred") {
        info.has_unknown = true;
        return;
    }
    if (executable == "sed") {
        info.has_read = true;
        return;
    }

    static constexpr std::string_view kReadVerbs[] = {
        "cat", "head", "tail", "less", "more", "grep", "rg", "ls",
        "stat", "file", "du", "wc", "diff", "cmp", "git"};
    for (const std::string_view verb : kReadVerbs) {
        if (executable == verb) {
            info.has_read = true;
            return;
        }
    }

    if (harmless_producer) {
        return;
    }
    info.has_unknown = true;
}

void temp_run(std::string_view text, const std::filesystem::path& root,
              std::filesystem::path cwd, TempInfo& info, int depth) {
    if (text.empty()) {
        return;
    }
    if (is_fork_bomb(text)) {
        info.untargeted_forbid = true;
    }
    const Scan scanned = scan(text);
    if (scanned.truncated) {
        info.concrete = false;
        info.has_unknown = true;
    }
    const std::size_t n = scanned.tokens.size();
    std::size_t       i = 0;
    while (i < n) {
        if (scanned.tokens[i].kind == TokKind::Separator ||
            scanned.tokens[i].kind == TokKind::Structural) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < n && scanned.tokens[j].kind != TokKind::Separator &&
               scanned.tokens[j].kind != TokKind::Structural) {
            ++j;
        }
        temp_segment(scanned.tokens, i, j, root, cwd, info, depth);
        i = j;
    }
}

TempInfo temp_pass(std::string_view text, const std::filesystem::path& root,
                   const std::filesystem::path& cwd,
                   const std::filesystem::path& canonical_tmp_root) {
    TempInfo info;
    temp_run(text, root, cwd, info, 0);
    if (!info.concrete || !info.has_write || info.has_read || info.has_unknown ||
        info.untargeted_forbid || info.targets.empty()) {
        return info;
    }
    for (const std::filesystem::path& target : info.targets) {
        if (!path_is_under_tmp_root(canonical_tmp_root, target)) {
            return info;
        }
    }
    return info;
}

bool temp_pass_eligible(const TempInfo& info,
                        const std::filesystem::path& canonical_tmp_root) {
    if (!info.concrete || !info.has_write || info.has_read || info.has_unknown ||
        info.untargeted_forbid || info.targets.empty()) {
        return false;
    }
    for (const std::filesystem::path& target : info.targets) {
        if (!path_is_under_tmp_root(canonical_tmp_root, target)) {
            return false;
        }
    }
    return true;
}

bool temp_cwd_concrete(std::string_view raw) {
    return !raw.empty() && raw.find('{') == std::string_view::npos &&
           raw.find('}') == std::string_view::npos &&
           raw.find_first_of("*?[") == std::string_view::npos &&
           raw.find('$') == std::string_view::npos &&
           raw.find('`') == std::string_view::npos &&
           raw.find('~') == std::string_view::npos && !temp_has_dotdot(raw);
}

std::optional<std::filesystem::path> temp_declared_cwd(
    const PermissionRequest& request, const std::filesystem::path& root,
    bool& concrete) {
    concrete = true;
    if (request.tool != "terminal" || !request.arguments.is_object()) {
        return std::nullopt;
    }
    const auto action = request.arguments.find("action");
    if (action == request.arguments.end() || !action->is_string() ||
        action->get<std::string>() != "open") {
        return std::nullopt;
    }
    const auto cwd = request.arguments.find("cwd");
    if (cwd == request.arguments.end()) {
        return root.lexically_normal();
    }
    if (!cwd->is_string() || cwd->get<std::string>().empty()) {
        concrete = false;
        return std::nullopt;
    }
    const std::string raw = cwd->get<std::string>();
    concrete = temp_cwd_concrete(raw);
    const std::filesystem::path value{raw};
    return (value.is_absolute() ? value : (root / value)).lexically_normal();
}

bool temp_resolve_canonical(const std::filesystem::path& raw,
                            std::filesystem::path&       canonical) {
    std::error_code error;
    std::filesystem::path prefix;
    for (const std::filesystem::path& component : raw) {
        prefix /= component;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(prefix, error);
        if (error) {
            if (error == std::errc::no_such_file_or_directory) {
                error.clear();
                continue;
            }
            return false;
        }
        if (std::filesystem::is_symlink(status)) {
            if (!std::filesystem::exists(prefix, error) || error) {
                return false;
            }
        }
    }
    canonical = std::filesystem::weakly_canonical(raw, error);
    return !error;
}

ShellText shell_text_of(const PermissionRequest& request) {
    ShellText out;
    out.cwd = request.root;
    if (!request.arguments.is_object()) {
        return out;
    }
    if (request.tool == "shell") {
        const auto it = request.arguments.find("command");
        if (it != request.arguments.end() && it->is_string()) {
            out.text = it->get<std::string>();
        }
        return out;
    }
    if (request.tool == "terminal") {
        std::string action;
        const auto  a = request.arguments.find("action");
        if (a != request.arguments.end() && a->is_string()) {
            action = a->get<std::string>();
        }
        if (action == "write") {
            const auto it = request.arguments.find("input");
            if (it != request.arguments.end() && it->is_string()) {
                out.text = it->get<std::string>();
            }
            return out;
        }
        const auto it = request.arguments.find("command");
        if (it != request.arguments.end() && it->is_string()) {
            out.text = it->get<std::string>();
            return out;
        }
        const auto av = request.arguments.find("argv");
        if (av != request.arguments.end() && av->is_array()) {
            std::string joined;
            for (const auto& element : *av) {
                if (!element.is_string()) {
                    continue;
                }
                if (!joined.empty()) {
                    joined.push_back(' ');
                }
                joined += element.get<std::string>();
            }
            out.text = std::move(joined);
        }
    }
    return out;
}

} // namespace

std::string shell_guard_text(const PermissionRequest& request) {
    return shell_text_of(request).text;
}

ShellCommandRisk classify_shell_command(std::string_view             text,
                                        const std::filesystem::path& root,
                                        const std::filesystem::path& cwd,
                                        bool                         kernel_confinement) {
    return classify_impl(text, root, cwd.empty() ? root : cwd, kernel_confinement, 0);
}

bool has_unterminated_quote(std::string_view text) {
    bool single = false;
    bool double_q = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (single) {
            if (c == '\'') {
                single = false;
            }
            continue;
        }
        if (double_q) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                double_q = false;
            }
            continue;
        }
        if (c == '\'') {
            single = true;
        } else if (c == '"') {
            double_q = true;
        } else if (c == '\\') {
            ++i;
        }
    }
    return single || double_q;
}

ShellClassification classify_shell_command_with_scope(
    std::string_view text, const std::filesystem::path& root,
    const std::filesystem::path& cwd, bool kernel_confinement,
    const std::filesystem::path& canonical_tmp_root) {
    ShellClassification out;
    if (text.empty()) {
        out.scope = ShellTargetScope::NotApplicable;
        return out;
    }
    const std::filesystem::path effective = cwd.empty() ? root : cwd;
    out.risk = classify_impl(text, root, effective, kernel_confinement, 0);

    const ScopeResult scope = scope_impl(text, root, effective, kernel_confinement);
    bool              protected_target = scope.protected_target;
    const Scan        scanned = scan(text);
    for (const Token& token : scanned.tokens) {
        if (scan_token_protected(token.text, root, effective)) {
            protected_target = true;
            break;
        }
    }
    const TempInfo temp = temp_pass(text, root, effective, canonical_tmp_root);
    out.tmp_exempt = temp_pass_eligible(temp, canonical_tmp_root);
    out.tmp_targets = temp.targets;
    out.protected_target = protected_target || temp.protected_target;

    if (out.risk == ShellCommandRisk::Forbid) {
        out.scope = ShellTargetScope::Outside;
    } else if (out.risk == ShellCommandRisk::Confirm) {
        out.scope = ShellTargetScope::Unknown;
    } else {
        out.scope = scope.scope;
        if (has_unterminated_quote(text)) {
            out.scope = ShellTargetScope::Unknown;
        }
    }
    return out;
}

bool is_simple_shell_command(std::string_view text) {
    bool single = false;
    bool double_q = false;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (single) {
            if (c == '\'') {
                single = false;
            }
            continue;
        }
        if (double_q) {
            if (c == '\\') {
                ++i;
            } else if (c == '"') {
                double_q = false;
            }
            continue;
        }
        if (c == '\'') {
            single = true;
            continue;
        }
        if (c == '"') {
            double_q = true;
            continue;
        }
        if (c == '\\') {
            ++i;
            continue;
        }
        if (c == ';' || c == '&' || c == '|' || c == '\n') {
            return false;
        }
    }
    return !single && !double_q;
}

std::string command_prefix(std::string_view text) {
    if (!is_simple_shell_command(text)) {
        return {};
    }
    const Scan s = scan(text);
    const std::size_t n = s.tokens.size();
    std::size_t end = 0;
    while (end < n && s.tokens[end].kind != TokKind::Separator &&
           s.tokens[end].kind != TokKind::Structural) {
        ++end;
    }
    const std::size_t cw = strip_prefixes(s.tokens, 0, end);
    if (cw >= end || s.tokens[cw].kind != TokKind::Word) {
        return {};
    }
    return s.tokens[cw].text;
}

ShellClassification classify_shell_request(
    const PermissionRequest& request,
    const std::filesystem::path& canonical_tmp_root) {
    ShellClassification out;
    if (request.tool != "shell" && request.tool != "terminal") {
        out.scope = ShellTargetScope::NotApplicable;
        return out;
    }

    const ShellText text = shell_text_of(request);
    const std::filesystem::path root =
        request.root.empty() ? std::filesystem::path{"."} : request.root;
    const std::filesystem::path default_cwd = text.cwd.empty() ? root : text.cwd;

    bool terminal_write = false;
    bool terminal_open = false;
    if (request.tool == "terminal" && request.arguments.is_object()) {
        const auto action = request.arguments.find("action");
        if (action != request.arguments.end() && action->is_string()) {
            terminal_write = action->get<std::string>() == "write";
            terminal_open = action->get<std::string>() == "open";
        }
    }

    if (terminal_open) {
        std::filesystem::path effective = default_cwd;
        bool                  cwd_unknown = false;
        bool                  cwd_outside = false;
        bool                  cwd_protected = false;
        bool                  cwd_declared = false;
        bool                  cwd_concrete = true;
        const auto            cwd = request.arguments.find("cwd");
        if (cwd != request.arguments.end()) {
            cwd_declared = true;
            if (!cwd->is_string() || cwd->get<std::string>().empty()) {
                cwd_unknown = true;
                cwd_concrete = false;
            } else {
                const std::string raw = cwd->get<std::string>();
                cwd_concrete = temp_cwd_concrete(raw);
                const std::filesystem::path value{raw};
                effective = (value.is_absolute() ? value : (root / value)).lexically_normal();
                if (!lexically_inside(root, effective)) {
                    cwd_outside = true;
                } else {
                    const std::filesystem::path relative = effective.lexically_relative(root);
                    cwd_protected = path_components_protected(relative);
                }
            }
        }
        out.risk = classify_impl(text.text, root, effective, request.kernel_confinement, 0);
        const ScopeResult scope = scope_impl(text.text, root, effective,
                                             request.kernel_confinement);
        bool protected_target = scope.protected_target || cwd_protected;
        const Scan scanned = scan(text.text);
        for (const Token& token : scanned.tokens) {
            if (scan_token_protected(token.text, root, effective)) {
                protected_target = true;
                break;
            }
        }
        const TempInfo temp = temp_pass(text.text, root, effective, canonical_tmp_root);
        out.tmp_targets = temp.targets;
        out.tmp_targets.push_back(effective.lexically_normal());
        out.tmp_exempt = temp_pass_eligible(temp, canonical_tmp_root) &&
                         !text.text.empty() && cwd_concrete &&
                         path_is_under_tmp_root(canonical_tmp_root, effective);
        out.protected_target = protected_target || temp.protected_target;
        if (cwd_unknown) {
            out.scope = ShellTargetScope::Unknown;
        } else if (cwd_outside) {
            out.scope = ShellTargetScope::Outside;
        } else if (text.text.empty()) {
            out.scope = cwd_declared ? ShellTargetScope::InRoot
                                     : ShellTargetScope::Unknown;
        } else {
            out.scope = scope.scope;
        }
        return out;
    }

    if (text.text.empty()) {
        out.scope = ShellTargetScope::Unknown;
        out.tmp_exempt = false;
        return out;
    }

    out = classify_shell_command_with_scope(text.text, root, default_cwd,
                                            request.kernel_confinement,
                                            canonical_tmp_root);
    if (terminal_write) {
        out.scope = ShellTargetScope::Unknown;
    }
    return out;
}

bool request_is_tmp_exempt(
    const PermissionRequest& request, const ShellClassification& classification,
    const std::filesystem::path& canonical_tmp_root) {
    if (request.tool != "shell" && request.tool != "terminal") {
        return false;
    }
    if (!classification.tmp_exempt || canonical_tmp_root.empty() ||
        request.root.empty()) {
        return false;
    }
    const std::filesystem::path root = request.root.lexically_normal();
    const std::filesystem::path tmp  = canonical_tmp_root.lexically_normal();
    if (root == tmp || path_is_under_tmp_root(tmp, root)) {
        return false;
    }
    if (classification.tmp_targets.empty()) {
        return false;
    }

    bool cwd_concrete = true;
    std::optional<std::filesystem::path> declared_cwd =
        temp_declared_cwd(request, root, cwd_concrete);

    for (const std::filesystem::path& raw : classification.tmp_targets) {
        if (!raw.is_absolute() || temp_has_dotdot(raw.string())) {
            return false;
        }
        std::filesystem::path canonical;
        if (!temp_resolve_canonical(raw, canonical)) {
            return false;
        }
        if (!path_is_under_tmp_root(tmp, canonical)) {
            return false;
        }
        const std::filesystem::path relative = canonical.lexically_relative(root);
        if (canonical == root ||
            (!relative.empty() && !relative.is_absolute() &&
             relative.begin()->string() != "..")) {
            return false;
        }
        if (path_is_protected(root, canonical)) {
            return false;
        }
        std::error_code error;
        const std::filesystem::file_status status =
            std::filesystem::symlink_status(canonical, error);
        bool exists = false;
        if (error) {
            if (error != std::errc::no_such_file_or_directory) {
                return false;
            }
            error.clear();
        } else {
            exists = std::filesystem::exists(status);
        }
        const bool is_declared_cwd =
            declared_cwd.has_value() &&
            raw.lexically_normal() == declared_cwd->lexically_normal();
        if (is_declared_cwd) {
            if (!cwd_concrete || !exists || !std::filesystem::is_directory(status)) {
                return false;
            }
        } else if (exists && !std::filesystem::is_regular_file(status)) {
            return false;
        }
    }
    return true;
}

bool path_within_root(const std::filesystem::path& root,
                      const std::filesystem::path& candidate,
                      bool                         single_quoted) {
    const std::string s = candidate.string();
    if (s.empty()) {
        return false;
    }
    if (is_home_ref(s, single_quoted)) {
        return false;
    }
    if (candidate.is_absolute()) {
        return lexically_inside(root, candidate);
    }
    return lexically_inside(root, root / candidate);
}

} // namespace ymh
