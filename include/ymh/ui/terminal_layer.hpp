#pragma once

// TerminalLayer: thin low-level terminal handling beneath FTXUI (10 §8.3, §20.19).
//
// `enterRawMode()` clears `IXON` so `Ctrl+S`/`Ctrl+Q` reach the application
// (U17, §7.4). The termios state is restored by an RAII guard on every exit path.

#include <cstdint>
#include <optional>
#include <string_view>
#include <termios.h>

// 68-D3/D6: `ui.color = auto|always|never`, defined with the config schema
// (`ymh/config/config.hpp`); forward-declared here so this low-level header does
// not pull the whole config header.
namespace ymh {
enum class ColorMode : std::uint8_t;
}

namespace ymh::ui {

// 68-D5/68-I9: every field is assigned explicitly by `detect_capabilities`; the
// in-class defaults are deliberately conservative (false), so a
// default-constructed set is monochrome rather than accidentally "colour on".
struct TerminalCapabilities {
    bool trueColor = false;
    bool color256 = false;
    // 68-D1: at least 8 colours — the gate `make_theme` now receives. A
    // truecolor terminal implies 256 colours implies this (68-I8).
    bool color = false;
    // Not probed today (reserved); UTF-8 output is assumed.
    bool mouse = false;
    bool unicode = true;
};

// 68-D1/D2/D4/68-I6: the pure detection inputs. `detect_capabilities` never
// touches the process environment or spawns a process, so every colour rule is
// hermetic; `TerminalLayer::capabilities()` is the only caller that reads the
// colour environment.
struct TerminalEnv {
    std::string_view term;
    std::string_view colorterm;
    std::string_view no_color;
};

// 68-D1: any non-empty `TERM` other than `dumb` is at least an 8-colour
// terminal; `COLORTERM=truecolor|24bit` upgrades the tiers. 68-D2: an empty or
// `dumb` `TERM` is monochrome (and `dumb` beats a contradictory `COLORTERM`).
// 68-D4: a non-empty `NO_COLOR` disables colour. 68-I8 keeps the tiers monotone.
[[nodiscard]] TerminalCapabilities detect_capabilities(const TerminalEnv& env) noexcept;

// 68-D3/68-I4/68-I5: apply the `ui.color` override on top of detection.
// `Always` forces colour even on a `dumb`/`NO_COLOR` terminal (the PuTTY escape
// hatch); `Never` forces monochrome even on a truecolor terminal; `Auto`
// returns `caps.color`.
[[nodiscard]] bool resolve_color(const TerminalCapabilities& caps, ColorMode mode) noexcept;

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
