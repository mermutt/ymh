#pragma once

// TerminalLayer: thin low-level terminal handling beneath FTXUI (10 §8.3, §20.19).
//
// `enterRawMode()` clears `IXON` so `Ctrl+S`/`Ctrl+Q` reach the application
// (U17, §7.4). The termios state is restored by an RAII guard on every exit path.

#include <optional>
#include <termios.h>

namespace ymh::ui {

struct TerminalCapabilities {
    bool trueColor = true;
    bool color256 = true;
    bool mouse = false;
    bool bracketedPaste = false;
    bool unicode = true;
};

struct TerminalSize {
    int width = 80;
    int height = 24;
};

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

    [[nodiscard]] TerminalSize size() const;
    [[nodiscard]] TerminalCapabilities capabilities() const;

    [[nodiscard]] static std::optional<termios> snapshot(int fd);
    [[nodiscard]] static bool ixon_disabled(const termios& state) noexcept;

private:
    int               fd_;
    bool              raw_ = false;
    std::optional<termios> saved_;
};

} // namespace ymh::ui
