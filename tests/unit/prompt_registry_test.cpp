#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/cli/wiring.hpp"
#include "ymh/config/config.hpp"
#include "ymh/prompt/order.hpp"
#include "ymh/prompt/persona.hpp"
#include "ymh/prompt/system_prompt.hpp"
#include "ymh/tools/tool.hpp"

namespace {

using namespace ymh;

PromptSection text_section(std::string name, std::int32_t order, std::string text,
                           bool complete = false) {
    PromptSection section;
    section.name     = std::move(name);
    section.order    = order;
    section.text     = [value = std::move(text)](const AssembleContext&) { return value; };
    section.complete = complete;
    return section;
}

ToolSchema schema(std::string name) {
    ToolSchema result;
    result.name        = ToolName{std::move(name)};
    result.version     = ToolVersion{};
    result.description = "test";
    result.input_schema = nlohmann::json::object();
    return result;
}

std::vector<std::string> section_names(const PromptAssembly& assembly) {
    std::vector<std::string> names;
    for (const AssembledSection& section : assembly.sections) {
        names.push_back(section.name);
    }
    return names;
}

TEST(PromptOrder, SectionNamesResolve) {
    EXPECT_EQ(section_order("harness:identity"), -1000);
    EXPECT_EQ(section_order("HARNESS_IDENTITY"), -1000);
    EXPECT_EQ(section_order("deployment:persona-prefix"), 0);
    EXPECT_EQ(section_order("plan:policy"), 500);
    EXPECT_EQ(section_order("tool:git"), 1050);
    EXPECT_EQ(section_order("tool:read"), 1100);
    EXPECT_EQ(section_order("deployment:persona-suffix"), 10200);
    EXPECT_EQ(section_order("harness:identity"),
              static_cast<std::int32_t>(SectionOrder::HarnessIdentity));
}

TEST(PromptOrder, ContextNamesResolve) {
    EXPECT_EQ(context_order("sandbox:policy"), 110);
    EXPECT_EQ(context_order("SANDBOX_POLICY"), 110);
    EXPECT_EQ(context_order("approval:policy"), 115);
    EXPECT_EQ(context_order("subagent:delegation"), 120);
}

TEST(PromptOrder, UnknownNameThrows) {
    EXPECT_THROW((void)section_order("not:a:section"), ConfigError);
    EXPECT_THROW((void)context_order("not:a:context"), ConfigError);
}

TEST(PromptRegistry, SectionsSortByOrderThenName) {
    SystemPrompt prompt;
    SectionHandle late  = prompt.section(text_section("z-late", 100, "late"));
    SectionHandle early = prompt.section(text_section("a-early", -5, "early"));
    SectionHandle tie_b = prompt.section(text_section("b-tie", 10, "bee"));
    SectionHandle tie_a = prompt.section(text_section("a-tie", 10, "aye"));
    (void)late;
    (void)early;
    (void)tie_b;
    (void)tie_a;

    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    EXPECT_EQ(section_names(assembly),
              (std::vector<std::string>{"a-early", "a-tie", "b-tie", "z-late"}));
}

TEST(PromptRegistry, EmptySectionDrops) {
    SystemPrompt prompt;
    SectionHandle kept   = prompt.section(text_section("kept", 0, "body"));
    SectionHandle empty  = prompt.section(text_section("empty", 10, ""));
    (void)kept;
    (void)empty;

    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    EXPECT_EQ(section_names(assembly), (std::vector<std::string>{"kept"}));
}

TEST(PromptRegistry, DuplicateSectionThrows) {
    SystemPrompt prompt;
    SectionHandle first = prompt.section(text_section("dup", 0, "one"));
    (void)first;
    EXPECT_THROW(prompt.section(text_section("dup", 10, "two")), ConfigError);
}

TEST(PromptRegistry, DuplicateContextThrows) {
    SystemPrompt prompt;
    PromptContext context;
    context.name = "ctx";
    context.text = [](const AssembleContext&) { return std::string{"body"}; };
    ContextHandle first = prompt.context(context);
    (void)first;
    EXPECT_THROW(prompt.context(context), ConfigError);
}

TEST(PromptRegistry, HandleDestructorUnregisters) {
    SystemPrompt prompt;
    {
        SectionHandle handle = prompt.section(text_section("temp", 0, "body"));
        (void)handle;
        EXPECT_EQ(section_names(prompt.assemble(AssembleContext{})),
                  (std::vector<std::string>{"temp"}));
    }
    EXPECT_TRUE(prompt.assemble(AssembleContext{}).sections.empty());
}

TEST(PromptRegistry, ContextsJoinAscendingAndDropEmpty) {
    SystemPrompt prompt;
    PromptContext first;
    first.name  = "first";
    first.order = 100;
    first.text  = [](const AssembleContext&) { return std::string{"first-body"}; };
    PromptContext second;
    second.name  = "second";
    second.order = 5;
    second.text  = [](const AssembleContext&) { return std::string{"second-body"}; };
    PromptContext blank;
    blank.name  = "blank";
    blank.order = 50;
    blank.text  = [](const AssembleContext&) { return std::string{}; };
    ContextHandle a = prompt.context(first);
    ContextHandle b = prompt.context(second);
    ContextHandle c = prompt.context(blank);
    (void)a;
    (void)b;
    (void)c;

    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    ASSERT_EQ(assembly.contexts.size(), 2U);
    EXPECT_EQ(assembly.contexts[0].name, "second");
    EXPECT_EQ(assembly.contexts[1].name, "first");
}

TEST(PromptVariables, StrictUnknownThrows) {
    SystemPrompt prompt;
    SectionHandle handle = prompt.section(text_section("s", 0, "hello {{missing}}"));
    (void)handle;
    EXPECT_THROW((void)render_prompt(prompt.assemble(AssembleContext{})), ConfigError);
}

TEST(PromptVariables, UndefinedProviderThrows) {
    SystemPrompt prompt;
    VariableHandle variable = prompt.variable(
        "maybe", [](const AssembleContext&) { return std::optional<std::string>{}; });
    SectionHandle handle = prompt.section(text_section("s", 0, "value {{maybe}}"));
    (void)variable;
    (void)handle;
    EXPECT_THROW((void)render_prompt(prompt.assemble(AssembleContext{})), ConfigError);
}

TEST(PromptVariables, MalformedReferenceThrows) {
    SystemPrompt prompt;
    SectionHandle handle = prompt.section(text_section("s", 0, "value {{}}"));
    (void)handle;
    EXPECT_THROW((void)render_prompt(prompt.assemble(AssembleContext{})), ConfigError);
}

TEST(PromptVariables, LoneOpenBraceIsLiteral) {
    SystemPrompt prompt;
    SectionHandle handle = prompt.section(text_section("s", 0, "prose {{ not a variable"));
    (void)handle;
    EXPECT_EQ(render_prompt(prompt.assemble(AssembleContext{})), "prose {{ not a variable");
}

TEST(PromptVariables, SubstitutedValueIsNotRescanned) {
    SystemPrompt prompt;
    VariableHandle variable = prompt.variable(
        "x", [](const AssembleContext&) { return std::optional<std::string>{"{{y}}"}; });
    SectionHandle handle = prompt.section(text_section("s", 0, "{{x}}"));
    (void)variable;
    (void)handle;
    EXPECT_EQ(render_prompt(prompt.assemble(AssembleContext{})), "{{y}}");
}

TEST(PromptVariables, EmptySectionsDropAndJoinWithBlankLines) {
    SystemPrompt prompt;
    SectionHandle a = prompt.section(text_section("a", 0, "one"));
    SectionHandle b = prompt.section(text_section("b", 1, ""));
    SectionHandle c = prompt.section(text_section("c", 2, "two"));
    (void)a;
    (void)b;
    (void)c;
    EXPECT_EQ(render_prompt(prompt.assemble(AssembleContext{})), "one\n\ntwo");
}

TEST(PromptVariables, RenderEqualsRenderPromptOfAssemble) {
    SystemPrompt prompt;
    SectionHandle handle = prompt.section(text_section("s", 0, "body"));
    (void)handle;
    EXPECT_EQ(prompt.render(AssembleContext{}),
              render_prompt(prompt.assemble(AssembleContext{})));
}

TEST(PromptPersona, PrefixAndSuffixOrders) {
    SystemPrompt prompt;
    PersonaHandles handles = register_persona(prompt, default_persona_config());
    (void)handles;

    VariableHandle model = prompt.variable(
        "model", [](const AssembleContext&) { return std::optional<std::string>{"m1"}; });
    VariableHandle cwd = prompt.variable(
        "cwd", [](const AssembleContext&) { return std::optional<std::string>{"/w"}; });
    (void)model;
    (void)cwd;

    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    ASSERT_EQ(assembly.sections.size(), 2U);
    EXPECT_EQ(assembly.sections[0].name, "deployment:persona-prefix");
    EXPECT_EQ(assembly.sections[1].name, "deployment:persona-suffix");
    EXPECT_EQ(render_prompt(assembly),
              "You are a coding agent powered by the m1 model.\n\nYour working directory is /w.");
}

TEST(PromptPersona, CompleteMakesPrefixTheSoleSection) {
    SystemPrompt prompt;
    SectionHandle identity = prompt.section(text_section("harness:identity", -1000, "IDENTITY"));
    PersonaConfig persona;
    persona.prefix   = "persona only";
    persona.complete = true;
    PersonaHandles handles = register_persona(prompt, persona);
    (void)identity;
    (void)handles;

    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    ASSERT_EQ(assembly.sections.size(), 1U);
    EXPECT_EQ(assembly.sections[0].name, "deployment:persona-prefix");
    EXPECT_EQ(render_prompt(assembly), "persona only");
}

TEST(PromptPersona, EmptyPrefixThrows) {
    SystemPrompt prompt;
    PersonaConfig persona;
    persona.prefix.clear();
    EXPECT_THROW((void)register_persona(prompt, persona), ConfigError);
}

TEST(PromptPersona, MoreThanOneCompleteFailsAssembly) {
    SystemPrompt prompt;
    SectionHandle first  = prompt.section(text_section("one", 0, "one", true));
    SectionHandle second = prompt.section(text_section("two", 1, "two", true));
    (void)first;
    (void)second;
    EXPECT_THROW((void)prompt.assemble(AssembleContext{}), ConfigError);
}

TEST(PromptTools, EmptyToolOrderIsLexicographic) {
    SystemPrompt prompt;
    prompt.set_tool_provider([](const AssembleContext&) {
        return std::vector<ToolSchema>{schema("zeta"), schema("alpha"), schema("mid")};
    });
    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    ASSERT_EQ(assembly.tools.size(), 3U);
    EXPECT_EQ(assembly.tools[0].name.value, "alpha");
    EXPECT_EQ(assembly.tools[1].name.value, "mid");
    EXPECT_EQ(assembly.tools[2].name.value, "zeta");
    EXPECT_TRUE(assembly.tool_order.empty());
}

TEST(PromptTools, RestTokenExpandsRemainingLexicographically) {
    SystemPrompt prompt({"zeta", "<unlisted-tools>", "alpha"});
    prompt.set_tool_provider([](const AssembleContext&) {
        return std::vector<ToolSchema>{schema("zeta"), schema("beta"), schema("alpha"),
                                       schema("mid")};
    });
    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    ASSERT_EQ(assembly.tools.size(), 4U);
    EXPECT_EQ(assembly.tools[0].name.value, "zeta");
    EXPECT_EQ(assembly.tools[1].name.value, "beta");
    EXPECT_EQ(assembly.tools[2].name.value, "mid");
    EXPECT_EQ(assembly.tools[3].name.value, "alpha");
}

TEST(PromptTools, UnknownToolOrderNameThrows) {
    SystemPrompt prompt({"ghost", "<unlisted-tools>"});
    prompt.set_tool_provider([](const AssembleContext&) {
        return std::vector<ToolSchema>{schema("alpha")};
    });
    EXPECT_THROW((void)prompt.assemble(AssembleContext{}), ConfigError);
}

TEST(PromptTools, MissingRestTokenThrows) {
    SystemPrompt prompt({"alpha"});
    prompt.set_tool_provider([](const AssembleContext&) {
        return std::vector<ToolSchema>{schema("alpha")};
    });
    EXPECT_THROW((void)prompt.assemble(AssembleContext{}), ConfigError);
}

TEST(PromptTools, DuplicateRestTokenThrows) {
    SystemPrompt prompt({"<unlisted-tools>", "<unlisted-tools>"});
    prompt.set_tool_provider([](const AssembleContext&) {
        return std::vector<ToolSchema>{schema("alpha")};
    });
    EXPECT_THROW((void)prompt.assemble(AssembleContext{}), ConfigError);
}

TEST(PromptTools, DuplicateNamedEntryThrows) {
    SystemPrompt prompt({"alpha", "alpha", "<unlisted-tools>"});
    prompt.set_tool_provider([](const AssembleContext&) {
        return std::vector<ToolSchema>{schema("alpha")};
    });
    EXPECT_THROW((void)prompt.assemble(AssembleContext{}), ConfigError);
}

TEST(PromptIdentity, DefaultIdentityAndPersonaRender) {
    SystemPrompt prompt;
    DefaultPromptConfig config;
    config.identity = default_system_prompt();
    config.persona  = default_persona_config();
    config.model    = "deepseek-flash";
    config.cwd      = "/workspace";
    DefaultPromptHandles handles = register_default_prompt(prompt, config);
    (void)handles;

    std::string rendered;
    EXPECT_NO_THROW(rendered = prompt.render(AssembleContext{}));
    EXPECT_NE(rendered.find(default_system_prompt()), std::string::npos);
    EXPECT_NE(rendered.find("powered by the deepseek-flash model"), std::string::npos);
    EXPECT_NE(rendered.find("working directory is /workspace"), std::string::npos);
    EXPECT_EQ(rendered, render_prompt(prompt.assemble(AssembleContext{})));
}

TEST(PromptIdentity, ConfigOverrideReplacesIdentityOnly) {
    SystemPrompt prompt;
    DefaultPromptConfig config;
    config.identity = "CUSTOM IDENTITY";
    config.persona  = default_persona_config();
    config.model    = "m";
    config.cwd      = "/w";
    DefaultPromptHandles handles = register_default_prompt(prompt, config);
    (void)handles;

    const PromptAssembly assembly = prompt.assemble(AssembleContext{});
    ASSERT_EQ(assembly.sections.size(), 3U);
    EXPECT_EQ(assembly.sections[0].name, "harness:identity");
    EXPECT_EQ(assembly.sections[0].text, "CUSTOM IDENTITY");
    EXPECT_EQ(assembly.sections[1].name, "deployment:persona-prefix");
    EXPECT_EQ(assembly.sections[2].name, "deployment:persona-suffix");
    EXPECT_EQ(render_prompt(assembly),
              "CUSTOM IDENTITY\n\nYou are a coding agent powered by the m model."
              "\n\nYour working directory is /w.");
}

TEST(PromptIdentity, DuplicateIdentityRegistrationThrows) {
    SystemPrompt prompt;
    DefaultPromptConfig config;
    config.identity = "IDENTITY";
    config.persona  = default_persona_config();
    config.model    = "m";
    config.cwd      = "/w";
    DefaultPromptHandles handles = register_default_prompt(prompt, config);
    (void)handles;
    EXPECT_THROW((void)register_default_prompt(prompt, config), ConfigError);
}

} // namespace
