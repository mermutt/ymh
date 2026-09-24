// Spec 56 acceptance tests: the `tool:<toolName>` delegation guidance section,
// the visibility seam, and the assemble() tool-resolution reorder.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/agent/subagent_types.hpp"
#include "ymh/config/config.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/prompt/order.hpp"
#include "ymh/prompt/system_prompt.hpp"
#include "ymh/tools/tool.hpp"

namespace {

using namespace ymh;

constexpr std::string_view kContinuable = "subagent_continuable";

constexpr std::string_view kPinnedGuidance =
    "Use subagent_continuable in the background by default. Start independent "
    "delegations together in one assistant message and continue useful work "
    "while they run. Set `run_in_background: false` only when your next action "
    "depends on that subagent's result. When a background run settles, the "
    "runtime sends you a notice containing its outcome and any final assistant "
    "message.";

ToolSchema schema(std::string name) {
    ToolSchema result;
    result.name         = ToolName{std::move(name)};
    result.version      = ToolVersion{};
    result.description  = "test";
    result.input_schema = nlohmann::json::object();
    return result;
}

PromptSection text_section(std::string name, std::int32_t order, std::string text) {
    PromptSection section;
    section.name = std::move(name);
    section.order = order;
    section.text  = [value = std::move(text)](const AssembleContext&) { return value; };
    return section;
}

PromptSection guidance_section(std::string tool_name) {
    PromptSection section;
    section.name  = "tool:" + tool_name;
    section.order = section_order("TOOL_SUBAGENT");
    section.text  = [tool = std::move(tool_name)](const AssembleContext& context) {
        if (!tool_visible(context, tool)) {
            return std::string{};
        }
        return delegation_guidance_text(tool);
    };
    return section;
}

std::vector<ToolSchema> default_tools() {
    return {schema("read_file"), schema(std::string{kContinuable}), schema("write_file")};
}

std::vector<ToolSchema> without(std::vector<ToolSchema> tools, std::string_view name) {
    tools.erase(std::remove_if(tools.begin(), tools.end(),
                               [&](const ToolSchema& entry) { return entry.name.value == name; }),
                tools.end());
    return tools;
}

void set_provider(SystemPrompt& prompt) {
    prompt.set_tool_provider([](const AssembleContext&) { return default_tools(); });
}

std::vector<std::string> section_names(const PromptAssembly& assembly) {
    std::vector<std::string> names;
    for (const AssembledSection& section : assembly.sections) {
        names.push_back(section.name);
    }
    return names;
}

bool has_section(const PromptAssembly& assembly, std::string_view name) {
    const std::vector<std::string> names = section_names(assembly);
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::vector<std::string> tool_names(const PromptAssembly& assembly) {
    std::vector<std::string> names;
    for (const ToolSchema& tool : assembly.tools) {
        names.push_back(tool.name.value);
    }
    return names;
}

// 56-U1
TEST(Spec56, GuidanceTextIsPinnedVerbatim) {
    const std::string text = delegation_guidance_text(kContinuable);
    EXPECT_EQ(text, kPinnedGuidance);
    EXPECT_EQ(text.find('\n'), std::string::npos);

    std::string single_tool = std::string{kPinnedGuidance};
    const std::size_t position = single_tool.find(kContinuable);
    ASSERT_NE(position, std::string::npos);
    single_tool.replace(position, kContinuable.size(), "subagent");
    EXPECT_EQ(delegation_guidance_text("subagent"), single_tool);
}

// 56-U2
TEST(Spec56, GuidanceEmittedWhenToolVisible) {
    SystemPrompt prompt;
    set_provider(prompt);
    SectionHandle before = prompt.section(text_section("aaa-pure", 2700, "before"));
    SectionHandle guidance = prompt.section(guidance_section(std::string{kContinuable}));
    SectionHandle after  = prompt.section(text_section("zzz-pure", 2900, "after"));

    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    EXPECT_EQ(section_order("TOOL_SUBAGENT"), 2800);
    ASSERT_EQ(section_names(assembly),
              (std::vector<std::string>{"aaa-pure", "tool:subagent_continuable", "zzz-pure"}));
    const auto found = std::find_if(assembly.sections.begin(), assembly.sections.end(),
                                    [](const AssembledSection& section) {
                                        return section.name == "tool:subagent_continuable";
                                    });
    ASSERT_NE(found, assembly.sections.end());
    EXPECT_EQ(found->text, delegation_guidance_text(kContinuable));
    EXPECT_TRUE(has_section(assembly, "tool:subagent_continuable"));
}

// 56-U3
TEST(Spec56, GuidanceAbsentWhenToolRestrictedAway) {
    SystemPrompt prompt;
    set_provider(prompt);
    SectionHandle guidance = prompt.section(guidance_section(std::string{kContinuable}));

    ScopeHandle scoped = prompt.scope("", "preset:restricted");
    scoped.set_tool_filter([](std::vector<ToolSchema> tools) {
        return without(std::move(tools), kContinuable);
    });

    const PromptAssembly assembly =
        prompt.assemble(AssembleContext{.scope = std::string{"preset:restricted"}});
    EXPECT_EQ(tool_names(assembly), (std::vector<std::string>{"read_file", "write_file"}));
    EXPECT_FALSE(has_section(assembly, "tool:subagent_continuable"));
    EXPECT_TRUE(assembly.sections.empty());
}

// 56-U4
TEST(Spec56, ToolOrderReordersNeverNarrows) {
    SystemPrompt prompt(std::vector<std::string>{"read_file", "<unlisted-tools>"});
    set_provider(prompt);
    SectionHandle guidance = prompt.section(guidance_section(std::string{kContinuable}));

    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    const std::vector<std::string> names = tool_names(assembly);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "read_file");
    std::vector<std::string> sorted = names;
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(sorted, (std::vector<std::string>{"read_file", "subagent_continuable", "write_file"}));
    EXPECT_TRUE(has_section(assembly, "tool:subagent_continuable"));

    SystemPrompt unknown(std::vector<std::string>{"no_such_tool", "<unlisted-tools>"});
    set_provider(unknown);
    EXPECT_THROW((void)unknown.assemble(AssembleContext{}), ConfigError);

    SystemPrompt duplicate(std::vector<std::string>{"read_file", "read_file", "<unlisted-tools>"});
    set_provider(duplicate);
    EXPECT_THROW((void)duplicate.assemble(AssembleContext{}), ConfigError);

    SystemPrompt no_rest(std::vector<std::string>{"read_file"});
    set_provider(no_rest);
    EXPECT_THROW((void)no_rest.assemble(AssembleContext{}), ConfigError);

    SystemPrompt two_rest(std::vector<std::string>{"<unlisted-tools>", "<unlisted-tools>"});
    set_provider(two_rest);
    EXPECT_THROW((void)two_rest.assemble(AssembleContext{}), ConfigError);
}

// 56-U5
TEST(Spec56, ToolVisibleIsFailClosed) {
    EXPECT_FALSE(tool_visible(AssembleContext{}, "read_file"));

    std::set<std::string> visible{"read_file"};
    AssembleContext       context;
    context.visible_tools = &visible;
    EXPECT_TRUE(tool_visible(context, "read_file"));
    EXPECT_FALSE(tool_visible(context, "write_file"));

    const PromptSection section = guidance_section("read_file");
    EXPECT_EQ(section.text(AssembleContext{}), "");
}

// 56-U6
TEST(Spec56, MultiInstanceSectionsAreIndependent) {
    SystemPrompt prompt;
    prompt.set_tool_provider([](const AssembleContext&) {
        return std::vector<ToolSchema>{schema("a"), schema("b")};
    });
    SectionHandle guidance_a = prompt.section(guidance_section("a"));
    SectionHandle guidance_b = prompt.section(guidance_section("b"));

    const PromptAssembly both = prompt.assemble(AssembleContext{});
    EXPECT_EQ(section_names(both), (std::vector<std::string>{"tool:a", "tool:b"}));

    ScopeHandle scoped = prompt.scope("", "preset:no-a");
    scoped.set_tool_filter(
        [](std::vector<ToolSchema> tools) { return without(std::move(tools), "a"); });
    const PromptAssembly restricted =
        prompt.assemble(AssembleContext{.scope = std::string{"preset:no-a"}});
    EXPECT_EQ(section_names(restricted), (std::vector<std::string>{"tool:b"}));
    EXPECT_EQ(tool_names(restricted), (std::vector<std::string>{"b"}));
}

// 56-U7
TEST(Spec56, RegistrationConditionExcludesNonContinuableAndDisabled) {
    const auto guidance_enabled = [](const DelegationToolConfig& config) {
        return config.enable_run_in_background &&
               config.background_mode == DelegationToolConfig::BackgroundMode::Continuable;
    };

    DelegationToolConfig continuable;
    continuable.background_mode = DelegationToolConfig::BackgroundMode::Continuable;
    EXPECT_TRUE(guidance_enabled(continuable));

    DelegationToolConfig disabled = continuable;
    disabled.enable_run_in_background = false;
    EXPECT_FALSE(guidance_enabled(disabled));

    DelegationToolConfig one_shot;
    one_shot.background_mode = DelegationToolConfig::BackgroundMode::OneShot;
    EXPECT_FALSE(guidance_enabled(one_shot));
}

// 56-U8
TEST(Spec56, DuplicateGuidanceSectionNameThrows) {
    SystemPrompt prompt;
    SectionHandle first = prompt.section(guidance_section("x"));
    EXPECT_THROW((void)prompt.section(guidance_section("x")), ConfigError);
}

// 56-U9
TEST(Spec56, PureSectionsAreUnaffectedByToolResolution) {
    SystemPrompt with_provider;
    set_provider(with_provider);
    SectionHandle pure = with_provider.section(text_section("pure", 100, "unchanged"));
    const PromptAssembly assembled = with_provider.assemble(AssembleContext{});

    SystemPrompt without_provider;
    SectionHandle bare = without_provider.section(text_section("pure", 100, "unchanged"));
    const PromptAssembly bare_assembly = without_provider.assemble(AssembleContext{});

    ASSERT_EQ(assembled.sections.size(), 1u);
    ASSERT_EQ(bare_assembly.sections.size(), 1u);
    EXPECT_EQ(assembled.sections.front().name, "pure");
    EXPECT_EQ(assembled.sections.front().text, "unchanged");
    EXPECT_EQ(assembled.sections.front().text, bare_assembly.sections.front().text);
    EXPECT_EQ(assembled.tools.size(), 3u);
    EXPECT_TRUE(bare_assembly.tools.empty());
}

// 56-U11
TEST(Spec56, GuidanceChangesDigestDeterministically) {
    SystemPrompt prompt;
    set_provider(prompt);
    SectionHandle guidance = prompt.section(guidance_section(std::string{kContinuable}));

    const std::string first  = prompt.render(AssembleContext{});
    const std::string second = prompt.render(AssembleContext{});
    EXPECT_EQ(first, second);

    ScopeHandle scoped = prompt.scope("", "preset:no-continuable");
    scoped.set_tool_filter([](std::vector<ToolSchema> tools) {
        return without(std::move(tools), kContinuable);
    });
    const std::string restricted =
        prompt.render(AssembleContext{.scope = std::string{"preset:no-continuable"}});

    EXPECT_NE(sha256_hex(first), sha256_hex(restricted));
    EXPECT_NE(first, restricted);
}

// 56-U14
TEST(Spec56, VisibilityIsCallScoped) {
    SystemPrompt prompt;
    set_provider(prompt);
    SectionHandle guidance = prompt.section(guidance_section(std::string{kContinuable}));

    AssembleContext caller;
    EXPECT_EQ(caller.visible_tools, nullptr);
    const PromptAssembly assembly = prompt.assemble(caller);
    EXPECT_EQ(caller.visible_tools, nullptr);
    EXPECT_TRUE(has_section(assembly, "tool:subagent_continuable"));

    const PromptSection section = guidance_section(std::string{kContinuable});
    EXPECT_EQ(section.text(caller), "");
}

// 56-U15
TEST(Spec56, GuidanceIsPresentationOnly) {
    SystemPrompt with_guidance;
    set_provider(with_guidance);
    SectionHandle guidance = with_guidance.section(guidance_section(std::string{kContinuable}));

    SystemPrompt without_guidance;
    set_provider(without_guidance);

    const PromptAssembly a = with_guidance.assemble(AssembleContext{});
    const PromptAssembly b = without_guidance.assemble(AssembleContext{});
    EXPECT_EQ(tool_names(a), tool_names(b));
    EXPECT_EQ(a.tools.size(), b.tools.size());
    EXPECT_TRUE(has_section(a, "tool:subagent_continuable"));
    EXPECT_FALSE(has_section(b, "tool:subagent_continuable"));
}

} // namespace
