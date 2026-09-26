#include "ymh/ui/render/diff_renderer.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/color.hpp>

namespace ymh::ui {
namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

std::string_view strip_trailing_metadata(std::string_view path) {
    const std::size_t tab = path.find('\t');
    return tab == std::string_view::npos ? path : path.substr(0, tab);
}

std::string header_path(std::string_view line, std::string_view prefix) {
    std::string_view rest = line.substr(prefix.size());
    if (rest == "/dev/null") {
        return {};
    }
    if (rest.size() > 2 && (rest[0] == 'a' || rest[0] == 'b') && rest[1] == '/') {
        rest = rest.substr(2);
    }
    return std::string(strip_trailing_metadata(rest));
}

std::string diff_git_path(std::string_view line) {
    const std::string_view rest = line.substr(std::string_view("diff --git ").size());
    const std::size_t space = rest.find(' ');
    if (space == std::string_view::npos) {
        return {};
    }
    std::string_view path = rest.substr(space + 1);
    if (path.size() > 2 && path[0] == 'b' && path[1] == '/') {
        path = path.substr(2);
    }
    return std::string(strip_trailing_metadata(path));
}

void append_line(DiffFile& file, DiffHunk*& hunk, DiffLineKind kind,
                 std::string text) {
    if (kind == DiffLineKind::Added) {
        ++file.additions;
    } else if (kind == DiffLineKind::Removed) {
        ++file.deletions;
    }
    if (hunk == nullptr) {
        if (kind == DiffLineKind::Meta) {
            return;
        }
        file.hunks.push_back(DiffHunk{});
        hunk = &file.hunks.back();
    }
    hunk->lines.push_back(DiffLine{kind, std::move(text)});
}

ftxui::Element line_element(const DiffLine& line, const RenderContext& context) {
    ftxui::Element element = ftxui::text(line.text);
    if (!context.theme.color) {
        return element;
    }
    switch (line.kind) {
        case DiffLineKind::Added:
            return element | ftxui::color(ftxui::Color::Green);
        case DiffLineKind::Removed:
            return element | ftxui::color(ftxui::Color::Red);
        case DiffLineKind::HunkHeader:
            return element | ftxui::color(ftxui::Color::Cyan);
        case DiffLineKind::FileHeader:
            return element | ftxui::color(ftxui::Color::Cyan) | ftxui::bold;
        case DiffLineKind::Meta:
            return element | ftxui::dim;
        case DiffLineKind::Context:
            return element;
    }
    return element;
}

} // namespace

DiffModel DiffModel::parse(std::string_view unified_diff) {
    DiffModel model;
    DiffFile* current = nullptr;
    DiffHunk* hunk = nullptr;
    std::string old_path;

    std::size_t start = 0;
    while (start <= unified_diff.size()) {
        const std::size_t newline = unified_diff.find('\n', start);
        const std::size_t end =
            newline == std::string_view::npos ? unified_diff.size() : newline;
        const std::string line(unified_diff.substr(start, end - start));

        if (starts_with(line, "diff --git ")) {
            DiffFile file;
            file.path = diff_git_path(line);
            model.files.push_back(std::move(file));
            current = &model.files.back();
            hunk = nullptr;
        } else if (starts_with(line, "--- ")) {
            old_path = header_path(line, "--- ");
        } else if (starts_with(line, "+++ ")) {
            if (current == nullptr) {
                model.files.push_back(DiffFile{});
                current = &model.files.back();
                hunk = nullptr;
            }
            std::string path = header_path(line, "+++ ");
            if (path.empty()) {
                path = old_path;
            }
            if (!path.empty()) {
                current->path = path;
            }
        } else if (starts_with(line, "@@")) {
            if (current == nullptr) {
                model.files.push_back(DiffFile{});
                current = &model.files.back();
            }
            current->hunks.push_back(DiffHunk{line, {}});
            hunk = &current->hunks.back();
        } else if (current != nullptr && !line.empty()) {
            if (starts_with(line, "+++") || starts_with(line, "---")) {
                append_line(*current, hunk, DiffLineKind::Meta, line);
            } else if (starts_with(line, "+")) {
                append_line(*current, hunk, DiffLineKind::Added, line);
            } else if (starts_with(line, "-")) {
                append_line(*current, hunk, DiffLineKind::Removed, line);
            } else if (starts_with(line, "\\")) {
                append_line(*current, hunk, DiffLineKind::Meta, line);
            } else if (starts_with(line, " ")) {
                append_line(*current, hunk, DiffLineKind::Context, line);
            } else {
                append_line(*current, hunk, DiffLineKind::Meta, line);
            }
        }

        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    return model;
}

ftxui::Element DiffRenderer::render(const DiffModel& model,
                                    const RenderContext& context) const {
    ftxui::Elements rows;
    for (const DiffFile& file : model.files) {
        std::string header = file.path.generic_string();
        if (header.empty()) {
            header = "(unknown)";
        }
        header += "  +" + std::to_string(file.additions);
        header += " -" + std::to_string(file.deletions);
        rows.push_back(line_element(DiffLine{DiffLineKind::FileHeader, header}, context));
        for (const DiffHunk& hunk : file.hunks) {
            rows.push_back(
                line_element(DiffLine{DiffLineKind::HunkHeader, hunk.header}, context));
            for (const DiffLine& line : hunk.lines) {
                rows.push_back(line_element(line, context));
            }
        }
    }
    if (rows.empty()) {
        return ftxui::text("(no changes)");
    }
    return ftxui::vbox(std::move(rows));
}

DiffView::DiffView(DiffModel& model, RenderContext context)
    : model_(model), context_(context) {}

ftxui::Element DiffView::OnRender() {
    return DiffRenderer{}.render(model_, context_);
}

bool DiffView::OnEvent(ftxui::Event event) {
    if (event == ftxui::Event::ArrowUp) {
        if (scroll_ > 0) {
            --scroll_;
        }
        return true;
    }
    if (event == ftxui::Event::ArrowDown) {
        ++scroll_;
        return true;
    }
    return false;
}

} // namespace ymh::ui
