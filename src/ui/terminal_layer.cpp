#include "ymh/ui/terminal_layer.hpp"

#include <cerrno>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

namespace ymh::ui {
namespace {

bool env_contains(const char* name, const char* needle) {
    const char* actual = std::getenv(name);
    return actual != nullptr && std::strstr(actual, needle) != nullptr;
}

} // namespace

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
    TerminalCapabilities caps;
    caps.trueColor = env_contains("COLORTERM", "truecolor") || env_contains("COLORTERM", "24bit");
    caps.color256 = caps.trueColor || env_contains("TERM", "256color");
    caps.unicode = true;
    return caps;
}

} // namespace ymh::ui
