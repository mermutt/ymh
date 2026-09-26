#pragma once

// First-class diff presentation (00 §24, 10 §8.2). `DiffModel::parse` turns a
// unified diff (git diff, tool edit preview, generated patch) into a structured
// file/hunk/line model; `DiffRenderer` paints added/removed/hunk lines. `DiffView`
// is the design-pinned component wrapper and adds no I/O of its own.

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <ftxui/component/component_base.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "ymh/ui/render/render_context.hpp"

namespace ymh::ui {

enum class DiffLineKind : std::uint8_t {
    Context,
    Added,
    Removed,
    HunkHeader,
    FileHeader,
    Meta,
};

struct DiffLine {
    DiffLineKind kind = DiffLineKind::Context;
    std::string  text;
};

struct DiffHunk {
    std::string           header;
    std::vector<DiffLine> lines;
};

struct DiffFile {
    std::filesystem::path path;
    std::vector<DiffHunk> hunks;
    int                   additions = 0;
    int                   deletions = 0;
};

class DiffModel {
public:
    std::vector<DiffFile> files;

    [[nodiscard]] static DiffModel parse(std::string_view unified_diff);
};

class DiffRenderer {
public:
    [[nodiscard]] ftxui::Element render(const DiffModel& model,
                                        const RenderContext& context) const;
};

class DiffView : public ftxui::ComponentBase {
public:
    explicit DiffView(DiffModel& model, RenderContext context = {});

    [[nodiscard]] ftxui::Element OnRender() override;
    bool                          OnEvent(ftxui::Event event) override;

private:
    DiffModel&    model_;
    RenderContext context_;
    int           scroll_ = 0;
    int           selectedFile_ = 0;
};

} // namespace ymh::ui
