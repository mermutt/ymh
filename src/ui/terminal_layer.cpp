#include "ymh/ui/terminal_layer.hpp"

#include <cerrno>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <string_view>

#include "ymh/config/config.hpp"

namespace ymh::ui {
namespace {

bool contains(std::string_view haystack, std::string_view needle) noexcept {
    return haystack.find(needle) != std::string_view::npos;
}

std::string_view env_value(const char* name) noexcept {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string_view{value} : std::string_view{};
}

} // namespace

TerminalCapabilities detect_capabilities(const TerminalEnv& env) noexcept {
    TerminalCapabilities caps;
    // 68-D4: NO_COLOR (no-color.org) — any non-empty value disables colour.
    if (!env.no_color.empty()) {
        return caps;
    }
    // 68-D2: an unknown or dumb terminal cannot interpret escapes; dumb also
    // beats a contradictory COLORTERM.
    if (env.term.empty() || env.term == "dumb") {
        return caps;
    }
    const bool colorterm_true =
        contains(env.colorterm, "truecolor") || contains(env.colorterm, "24bit");
    caps.color = true;
    caps.trueColor = colorterm_true;
    // 68-I8: truecolor implies 256 colours.
    caps.color256 = colorterm_true || contains(env.term, "256color");
    return caps;
}

bool resolve_color(const TerminalCapabilities& caps, ColorMode mode) noexcept {
    switch (mode) {
        case ColorMode::Always:
            return true;
        case ColorMode::Never:
            return false;
        case ColorMode::Auto:
            break;
    }
    return caps.color;
}

bool write_bracketed_paste(bool enabled, int out_fd) noexcept {
    if (out_fd < 0) {
        return false;
    }
    const std::string_view sequence =
        enabled ? kBracketedPasteEnable : kBracketedPasteDisable;
    const char* cursor    = sequence.data();
    std::size_t remaining = sequence.size();
    while (remaining > 0) {
        const ssize_t written = ::write(out_fd, cursor, remaining);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (written == 0) {
            return false;
        }
        cursor += static_cast<std::size_t>(written);
        remaining -= static_cast<std::size_t>(written);
    }
    return true;
}

TerminalLayer::TerminalLayer(int fd) : fd_(fd) {}

TerminalLayer::~TerminalLayer() {
    leaveBracketedPaste();
    leaveRawMode();
}

bool TerminalLayer::enterBracketedPaste() {
    if (paste_) {
        return true;
    }
    if (::isatty(STDOUT_FILENO) == 0) {
        return false;
    }
    paste_ = write_bracketed_paste(true, STDOUT_FILENO);
    return paste_;
}

void TerminalLayer::leaveBracketedPaste() {
    if (!paste_) {
        return;
    }
    (void)write_bracketed_paste(false, STDOUT_FILENO);
    paste_ = false;
}

std::optional<termios> TerminalLayer::snapshot(int fd) {
    termios state{};
    if (::tcgetattr(fd, &state) != 0) {
        return std::nullopt;
    }
    return state;
}

bool TerminalLayer::ixon_disabled(const termios& state) noexcept {
    return (state.c_iflag & IXON) == 0;
}

void TerminalLayer::enterRawMode() {
    if (raw_) {
        return;
    }
    const std::optional<termios> current = snapshot(fd_);
    if (!current.has_value()) {
        return;
    }
    saved_ = *current;
    termios raw = *current;
    raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO | ECHONL | ISIG | IEXTEN));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (::tcsetattr(fd_, TCSANOW, &raw) == 0) {
        raw_ = true;
    }
}

void TerminalLayer::leaveRawMode() {
    if (!raw_) {
        return;
    }
    if (saved_.has_value()) {
        ::tcsetattr(fd_, TCSANOW, &*saved_);
    }
    raw_ = false;
}

TerminalSize TerminalLayer::size() const {
    winsize window{};
    if (::ioctl(fd_, TIOCGWINSZ, &window) != 0 || window.ws_col == 0 || window.ws_row == 0) {
        if (::ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) != 0 || window.ws_col == 0 ||
            window.ws_row == 0) {
            const char* columns = std::getenv("COLUMNS");
            const char* lines = std::getenv("LINES");
            TerminalSize fallback;
            if (columns != nullptr) {
                fallback.width = std::atoi(columns);
            }
            if (lines != nullptr) {
                fallback.height = std::atoi(lines);
            }
            return fallback;
        }
    }
    return TerminalSize{static_cast<int>(window.ws_col), static_cast<int>(window.ws_row)};
}

TerminalCapabilities TerminalLayer::capabilities() const {
    return detect_capabilities(
        TerminalEnv{env_value("TERM"), env_value("COLORTERM"), env_value("NO_COLOR")});
}

} // namespace ymh::ui
