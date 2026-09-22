#include <gtest/gtest.h>

#include <string>

#include <ftxui/dom/elements.hpp>
#include <ftxui/screen/screen.hpp>

#include "ymh/ui/render/diff_renderer.hpp"

namespace {

using namespace ymh::ui;

const char* kSampleDiff =
    "diff --git a/src/foo.cpp b/src/foo.cpp\n"
    "index 1111111..2222222 100644\n"
    "--- a/src/foo.cpp\n"
    "+++ b/src/foo.cpp\n"
    "@@ -1,3 +1,4 @@\n"
    " context line\n"
    "-removed line\n"
    "+added line\n"
    "+another line\n";

std::string render_text(const DiffModel& model) {
    const RenderContext context{
        .width = 80, .content_width = 80, .theme = Theme{false}, .compact = false};
    ftxui::Element element = DiffRenderer{}.render(model, context);
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimensions{80, 40});
    ftxui::Render(screen, element);
    return screen.ToString();
}

TEST(DiffRenderer, ParsesFileAndCounts) {
    const DiffModel model = DiffModel::parse(kSampleDiff);
    ASSERT_EQ(model.files.size(), 1u);
    const DiffFile& file = model.files.front();
    EXPECT_EQ(file.path.generic_string(), "src/foo.cpp");
    EXPECT_EQ(file.additions, 2);
    EXPECT_EQ(file.deletions, 1);
    ASSERT_EQ(file.hunks.size(), 1u);
    EXPECT_EQ(file.hunks.front().header, "@@ -1,3 +1,4 @@");
}

TEST(DiffRenderer, ClassifiesLineKinds) {
    const DiffModel model = DiffModel::parse(kSampleDiff);
    const DiffHunk& hunk = model.files.front().hunks.front();
    ASSERT_EQ(hunk.lines.size(), 4u);
    EXPECT_EQ(hunk.lines[0].kind, DiffLineKind::Context);
    EXPECT_EQ(hunk.lines[1].kind, DiffLineKind::Removed);
    EXPECT_EQ(hunk.lines[2].kind, DiffLineKind::Added);
    EXPECT_EQ(hunk.lines[3].kind, DiffLineKind::Added);
}

TEST(DiffRenderer, ParsesMultipleFiles) {
    const std::string diff =
        "diff --git a/a.txt b/a.txt\n"
        "--- a/a.txt\n"
        "+++ b/a.txt\n"
        "@@ -1 +1 @@\n"
        "-old\n"
        "+new\n"
        "diff --git a/b.txt b/b.txt\n"
        "--- a/b.txt\n"
        "+++ b/b.txt\n"
        "@@ -1 +1,2 @@\n"
        " keep\n"
        "+extra\n";
    const DiffModel model = DiffModel::parse(diff);
    ASSERT_EQ(model.files.size(), 2u);
    EXPECT_EQ(model.files[0].path.generic_string(), "a.txt");
    EXPECT_EQ(model.files[1].path.generic_string(), "b.txt");
    EXPECT_EQ(model.files[1].additions, 1);
}

TEST(DiffRenderer, RendersAddedRemovedAndHunkLines) {
    const std::string rendered = render_text(DiffModel::parse(kSampleDiff));
    EXPECT_NE(rendered.find("src/foo.cpp"), std::string::npos);
    EXPECT_NE(rendered.find("@@ -1,3 +1,4 @@"), std::string::npos);
    EXPECT_NE(rendered.find("-removed line"), std::string::npos);
    EXPECT_NE(rendered.find("+added line"), std::string::npos);
}

TEST(DiffRenderer, EmptyDiffRendersPlaceholder) {
    const std::string rendered = render_text(DiffModel::parse(""));
    EXPECT_NE(rendered.find("(no changes)"), std::string::npos);
}

TEST(DiffRenderer, ViewRendersModel) {
    DiffModel model = DiffModel::parse(kSampleDiff);
    DiffView view(model);
    ftxui::Element element = view.Render();
    ftxui::Screen screen = ftxui::Screen::Create(ftxui::Dimensions{80, 40});
    ftxui::Render(screen, element);
    EXPECT_NE(screen.ToString().find("src/foo.cpp"), std::string::npos);
    EXPECT_TRUE(view.OnEvent(ftxui::Event::ArrowDown));
    EXPECT_TRUE(view.OnEvent(ftxui::Event::ArrowUp));
    EXPECT_FALSE(view.OnEvent(ftxui::Event::Character('x')));
}

} // namespace
