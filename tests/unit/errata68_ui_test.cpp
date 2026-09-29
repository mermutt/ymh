// 68: terminal colour capability detection (68-D1/D2/D4/D5), the NO_COLOR
// convention, the `ui.color` override (68-D3), and the config wiring.
//
// The rule tests call the pure `detect_capabilities`/`resolve_color` so they
// never touch the process environment; only
// `CapabilitiesReadsTheProcessEnvironment` sets env vars (with a restoring
// guard) to pin the `TerminalLayer::capabilities()` plumbing.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>

#include "support/test_env.hpp"
#include "ymh/config/config.hpp"
#include "ymh/ui/terminal_layer.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

class ScopedEnv {
public:
    ScopedEnv(std::string name, std::string value) : name_(std::move(name)) {
        capture();
        ::setenv(name_.c_str(), value.c_str(), 1);
    }

    explicit ScopedEnv(std::string name) : name_(std::move(name)) {
        capture();
        ::unsetenv(name_.c_str());
    }

    ~ScopedEnv() {
        if (previous_.has_value()) {
            ::setenv(name_.c_str(), previous_->c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    void capture() {
        if (const char* previous = ::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
    }

    std::string                name_;
    std::optional<std::string> previous_;
};

TerminalCapabilities detect(std::string_view term, std::string_view colorterm = {},
                            std::string_view no_color = {}) {
    return detect_capabilities(TerminalEnv{term, colorterm, no_color});
}

// 68-U1: the reported PuTTY case — TERM=xterm, no COLORTERM.
TEST(Errata68Color, TermXtermWithoutColortermIsAtLeastEightColours) {
    const TerminalCapabilities caps = detect("xterm");
    EXPECT_TRUE(caps.color);
    EXPECT_FALSE(caps.trueColor);
    EXPECT_FALSE(caps.color256);
}

// 68-U2: a 256-colour terminal stays a 256-colour terminal, not truecolor.
TEST(Errata68Color, TermXterm256colorIs256) {
    const TerminalCapabilities caps = detect("xterm-256color");
    EXPECT_TRUE(caps.color);
    EXPECT_TRUE(caps.color256);
    EXPECT_FALSE(caps.trueColor);
}

// 68-U3: COLORTERM=truecolor upgrades both higher tiers (68-I8).
TEST(Errata68Color, ColortermTruecolorIs24bit) {
    const TerminalCapabilities caps = detect("xterm", "truecolor");
    EXPECT_TRUE(caps.color);
    EXPECT_TRUE(caps.color256);
    EXPECT_TRUE(caps.trueColor);
}

// 68-U4: a dumb terminal is monochrome.
TEST(Errata68Color, DumbTerminalIsMonochrome) {
    EXPECT_FALSE(detect("dumb").color);
}

// 68-U5: dumb beats a contradictory COLORTERM.
TEST(Errata68Color, DumbTerminalBeatsContradictoryColorterm) {
    const TerminalCapabilities caps = detect("dumb", "truecolor");
    EXPECT_FALSE(caps.color);
    EXPECT_FALSE(caps.trueColor);
}

// 68-U6: NO_COLOR disables colour even on a truecolor terminal.
TEST(Errata68Color, NoColorDisablesEvenTruecolor) {
    const TerminalCapabilities caps = detect("xterm-256color", "truecolor", "1");
    EXPECT_FALSE(caps.color);
    EXPECT_FALSE(caps.color256);
    EXPECT_FALSE(caps.trueColor);
}

// 68-U7: an empty NO_COLOR is not "set".
TEST(Errata68Color, EmptyNoColorDoesNotDisable) {
    EXPECT_TRUE(detect("xterm", "", "").color);
}

// 68-U8: no TERM at all is monochrome.
TEST(Errata68Color, UnsetTerminalIsMonochrome) {
    EXPECT_FALSE(detect("").color);
}

// 68-U9: ui.color=always forces colour on a dumb terminal.
TEST(Errata68Color, AlwaysForcesColourOnDumbTerminal) {
    EXPECT_TRUE(resolve_color(detect("dumb"), ColorMode::Always));
}

// 68-U16: ui.color=always also overrides NO_COLOR (68-I4).
TEST(Errata68Color, AlwaysOverridesNoColor) {
    EXPECT_TRUE(resolve_color(detect("xterm", "truecolor", "1"), ColorMode::Always));
}

// 68-U10: ui.color=never forces monochrome on a truecolor terminal.
TEST(Errata68Color, NeverForcesMonochromeOnTruecolor) {
    EXPECT_FALSE(resolve_color(detect("xterm", "truecolor"), ColorMode::Never));
}

// 68-U11: ui.color=auto follows detection.
TEST(Errata68Color, AutoFollowsDetection) {
    EXPECT_TRUE(resolve_color(detect("xterm"), ColorMode::Auto));
    EXPECT_FALSE(resolve_color(detect("dumb"), ColorMode::Auto));
}

// 68-U12: the in-class defaults are conservative (68-D5/68-I9).
TEST(Errata68Color, DefaultCapabilityFieldsAreConservative) {
    const TerminalCapabilities caps;
    EXPECT_FALSE(caps.color);
    EXPECT_FALSE(caps.color256);
    EXPECT_FALSE(caps.trueColor);
}

// 68-U13: TerminalLayer::capabilities() reads TERM/COLORTERM/NO_COLOR.
TEST(Errata68Color, CapabilitiesReadsTheProcessEnvironment) {
    ScopedEnv term("TERM", "xterm");
    ScopedEnv colorterm("COLORTERM");
    ScopedEnv no_color("NO_COLOR");

    const TerminalLayer        layer(0);
    const TerminalCapabilities caps = layer.capabilities();
    EXPECT_TRUE(caps.color);
    EXPECT_FALSE(caps.trueColor);
}

// 68-U14: the config parser accepts each mode, defaults to auto, and rejects a
// typo (68-I7).
TEST(Errata68Color, ConfigParsesColorModesAndRejectsUnknown) {
    test::TempWorkspace workspace("errata68_color_config");
    const std::filesystem::path global = workspace.path() / "global.jsonc";

    const auto load = [&](const std::string& document) {
        workspace.write("global.jsonc", document);
        ConfigPaths paths;
        paths.global = global;
        return load_config(paths);
    };

    EXPECT_EQ(load("{\"ui\":{\"color\":\"always\"}}\n").ui.color, ColorMode::Always);
    EXPECT_EQ(load("{\"ui\":{\"color\":\"never\"}}\n").ui.color, ColorMode::Never);
    EXPECT_EQ(load("{\"ui\":{\"color\":\"auto\"}}\n").ui.color, ColorMode::Auto);
    EXPECT_EQ(load("{}\n").ui.color, ColorMode::Auto);

    workspace.write("global.jsonc", "{\"ui\":{\"color\":\"alwys\"}}\n");
    ConfigPaths paths;
    paths.global = global;
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

// 68-U15: the scaffolded default file is valid and closes as auto.
TEST(Errata68Color, ScaffoldedDefaultConfigLoadsWithColorAuto) {
    test::TempWorkspace         workspace("errata68_scaffold");
    const std::filesystem::path global = workspace.path() / "xdg" / "ymh" / "config.jsonc";

    const ScaffoldResult result = scaffold_config(workspace.path(), global);
    ASSERT_TRUE(result.ok);
    ASSERT_TRUE(result.global_config_created);

    ConfigPaths paths;
    paths.global = global;
    EXPECT_EQ(load_config(paths).ui.color, ColorMode::Auto);
}

} // namespace
