#pragma once

// Pure renderers (10 §8.2/§8.4, §20.12). `renderToAnsi` reads the model and
// writes ANSI to a caller buffer; it never performs I/O or advances a timer
// (U3, D16). The golden tests and the PTY assertions share this code path.

#include <string>
#include <string_view>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

#include "ymh/ui/terminal_layer.hpp"
#include "ymh/ui/theme.hpp"
#include "ymh/ui/ui_model.hpp"

namespace ymh::ui {

enum class LayoutMode : std::uint8_t {
    Narrow,
    Normal,
    Wide,
};

// 18 §5.1 (M5/M6): the pure `/context` grid geometry and cell mapping, shared
// by the renderer and its tests.
struct ContextGridGeometry {
    int cols = 0;
    int rows = 0;
};

[[nodiscard]] ContextGridGeometry context_grid_geometry(int width, int height) noexcept;

// Returns the segment owning cell `cell_index` of `cells_total`. `grid_total` is
// derived from the snapshot (window > 0 ? window : used). Degenerate inputs
// (cells_total <= 0, cell_index out of [0, cells_total), grid_total == 0) return
// ContextSegmentKind::FreeSpace; the caller must not draw a grid for them.
[[nodiscard]] ContextSegmentKind context_cell_kind(const ContextSnapshot& snapshot,
                                                   int cells_total, int cell_index) noexcept;

[[nodiscard]] LayoutMode calculate_layout(int width);
[[nodiscard]] ftxui::Element build_ui(const UiModel& model, TerminalSize size,
                                      const Theme& theme);
[[nodiscard]] std::string render_to_ansi(const UiModel& model, TerminalSize size,
                                         const Theme& theme = {});

// 48-D7.2: the one-line tool-argument summary. Parses `arguments_json` and
// prefers command/path/pattern/query/url/name; falls back to the compact JSON
// or the raw string on parse failure. Pure; never throws.
[[nodiscard]] std::string summarize_tool_arguments(std::string_view tool_name,
                                                   std::string_view arguments_json);

// 48-D6.3: the single styling point for the text hierarchy.
[[nodiscard]] ftxui::Element apply_presentation(ftxui::Element element,
                                                Presentation presentation,
                                                const Theme& theme);

// 48-D7.3 (Rev 2): a colored/bold run of one or more UTF-8 glyphs.
struct StyledSpan {
    std::string  text;
    ftxui::Color color{};
    bool         bold = false;

    bool operator==(const StyledSpan&) const = default;
};
using StyledLine = std::vector<StyledSpan>;

// 48-D7.3 (Rev 2): truncates to `max_width` terminal columns, walking whole
// UTF-8 glyphs via `ftxui::string_width` and never splitting one. Appends "…"
// inside the last kept span (or a new `Color::GrayLight` span when nothing was
// kept). Returns the spans unchanged when the line already fits. Pure.
[[nodiscard]] StyledLine truncate_spans(const StyledLine& line, int max_width);

} // namespace ymh::ui
