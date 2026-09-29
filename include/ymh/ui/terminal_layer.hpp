#pragma once

// TerminalLayer: thin low-level terminal handling beneath FTXUI (10 §8.3, §20.19).
//
// `enterRawMode()` clears `IXON` so `Ctrl+S`/`Ctrl+Q` reach the application
// (U17, §7.4). The termios state is restored by an RAII guard on every exit path.

#include <optional>
#include <string_view>
#include <termios.h>

namespace ymh::ui {

struct TerminalCapabilities {
    bool trueColor = true;
    bool color256 = true;
    bool mouse = false;
    bool unicode = true;
};

struct TerminalSize {
    int width = 80;
    int height = 24;
};

// 66-D2: bracketed paste (DECSET/DECRST 2004). The enable sequence asks the
// terminal to wrap every paste in `ESC [ 200 ~` … `ESC [ 201 ~`, so pasted
// newlines are no longer indistinguishable from a pressed Return. Emitted only
// on a tty; terminals that do not implement the private mode ignore it, so the
// emission is unconditional (no capability probe — DECSET 2004 is not
// introspectable and a false negative would reintroduce the bug).
inline constexpr std::string_view kBracketedPasteEnable  = "\x1b[?2004h";
inline constexpr std::string_view kBracketedPasteDisable = "\x1b[?2004l";

// Writes the DECSET/DECRST 2004 sequence to `out_fd`, looping over partial
// writes and retrying `EINTR`. Returns `true` only when the whole sequence was
// written. `fd < 0` is a no-op returning `false`.
[[nodiscard]] bool write_bracketed_paste(bool enabled, int out_fd) noexcept;

class TerminalLayer {
public:
    explicit TerminalLayer(int fd);
    ~TerminalLayer();

    TerminalLayer(const TerminalLayer&) = delete;
    TerminalLayer& operator=(const TerminalLayer&) = delete;
    TerminalLayer(TerminalLayer&&) = delete;
    TerminalLayer& operator=(TerminalLayer&&) = delete;

    void enterRawMode();
    void leaveRawMode();
    [[nodiscard]] bool rawMode() const noexcept { return raw_; }

    // Bracketed-paste lifecycle (66-D2). `enterBracketedPaste` emits DECSET 2004
    // on `STDOUT_FILENO` and is a no-op when stdout is not a tty; the destructor
    // (and `leaveBracketedPaste`) restores DECRST 2004 on every exit path.
    bool enterBracketedPaste();
    void leaveBracketedPaste();
    [[nodiscard]] bool bracketedPasteActive() const noexcept { return paste_; }

    [[nodiscard]] TerminalSize size() const;
    [[nodiscard]] TerminalCapabilities capabilities() const;

    [[nodiscard]] static std::optional<termios> snapshot(int fd);
    [[nodiscard]] static bool ixon_disabled(const termios& state) noexcept;

private:
    int               fd_;
    bool              raw_ = false;
    bool              paste_ = false;
    std::optional<termios> saved_;
};

} // namespace ymh::ui
