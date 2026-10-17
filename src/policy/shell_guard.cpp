// Spec 89 (rev 3): the L1 shell-command classifier. Pure, total, deterministic,
// I/O-free: it never executes the command, never touches the filesystem, never
// resolves symlinks, never blocks, and never throws (SG1/SG12). It is a
// fail-safe clamp (89-D1): a recognized destructive verb with an ambiguous or
// unresolved operand is never `None` (SG5).

#include "ymh/policy/shell_guard.hpp"

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

bool is_output_redirect(std::string_view op) {
    return (!op.empty() && op[0] == '>') || op == "&>" || op == "<>";
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
