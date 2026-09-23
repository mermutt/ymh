#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "support/test_env.hpp"
#include "ymh/agent/preset.hpp"
#include "ymh/config/config.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/prompt/runtime_context.hpp"
#include "ymh/prompt/system_prompt.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/skills/skill_catalog.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

class ScopedEnv {
public:
    ScopedEnv(std::string name, std::string value) : name_(std::move(name)) {
        if (const char* previous = std::getenv(name_.c_str()); previous != nullptr) {
            previous_ = previous;
        }
        ::setenv(name_.c_str(), value.c_str(), 1);
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
    std::string                name_;
    std::optional<std::string> previous_;
};

class CapturingLogger final : public Logger {
public:
    void log(LogLevel level, std::string_view message) override {
        if (level == LogLevel::Warn || level == LogLevel::Error) {
            warnings.emplace_back(message);
        }
    }

    std::vector<std::string> warnings;
};

void write_preset(const std::filesystem::path& root, const std::string& id,
                  const std::string& body) {
    const std::filesystem::path dir = root / id;
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "preset.jsonc", std::ios::binary) << body;
}

constexpr const char* kMinimalPreset = R"({
    "id": "minimal",
    "display_name": "Minimal",
    "rows": [
        {"id": "persona", "persona": {
            "prefix": "You are a helpful software engineer assistant.",
            "complete": true,
            "include_runtime_context": false}},
        {"id": "shell", "tools": {"allow": ["shell"]}}
    ]
})";

constexpr const char* kStandardPreset = R"({
    "id": "standard",
    "display_name": "Standard",
    "rows": [
        {"id": "persona", "persona": {
            "prefix": "You are a coding agent powered by the {{model}} model.",
            "suffix": "Your working directory is {{cwd}}."}},
        {"id": "instructions"}
    ]
})";

PresetConfig shipped_config() {
    PresetConfig config;
    config.include_shipped_root = true;
    config.include_user_root    = false;
    return config;
}

PresetConfig root_only_config(const std::filesystem::path& root) {
    PresetConfig config;
    config.root                 = root;
    config.include_shipped_root = false;
    config.include_user_root    = false;
    return config;
}

class FakeAgent final : public Agent {
public:
    FakeAgent(AgentId id, SessionId session) : id_(std::move(id)), session_(std::move(session)) {}

    AgentId   id() const override { return id_; }
    SessionId session() const override { return session_; }

    AgentStatus status() const noexcept override { return AgentStatus::Idle; }
    AgentState  state() const noexcept override { return AgentState::Idle; }
    bool        disposed() const noexcept override { return false; }
    bool        hasPendingWork() const noexcept override { return false; }

    InboxResult send(Message) override { return InboxResult::Accepted; }
    InboxResult followup(Message) override { return InboxResult::Accepted; }
    InboxResult steer(Message) override { return InboxResult::Accepted; }
    InboxResult inject(ContextMessage) override { return InboxResult::Accepted; }

    void cancel() override {}
    void dispose() override {}
    void whenIdle(std::function<void()>) override {}

private:
    AgentId   id_;
    SessionId session_;
};

struct RosterEnv {
    explicit RosterEnv(const std::string& prefix)
        : workspace(prefix),
          env(workspace.path()),
          user_root(make_dir(workspace.path() / "user_skills")),
          skills(SkillCatalogConfig{}, env,
                 std::vector<SkillRoot>{
                     SkillRoot{user_root, SkillSource::User, SkillTrust::Trusted}},
                 logger) {}

    static std::filesystem::path make_dir(const std::filesystem::path& path) {
        std::filesystem::create_directories(path);
        return path;
    }

    TempWorkspace         workspace;
    LocalEnvironment      env;
    NullLogger            logger;
    std::filesystem::path user_root;
    SkillCatalog          skills;
    EventBus              bus;
    MemorySessionStore    store;
    SessionManager        sessions{store, bus};
    SystemPrompt          prompt;
    ToolRegistry          tools;
    std::filesystem::path shipped_root = workspace.path() / "shipped";
};

SessionId make_session(SessionManager& sessions, const std::filesystem::path& cwd) {
    SessionOptions options;
    options.cwd           = cwd;
    options.model         = "test-model";
    options.title         = "test";
    options.serverProfile = "interactive";
    return sessions.createSession(options);
}

ToolSchema tool_schema(std::string name) {
    ToolSchema result;
    result.name         = ToolName{std::move(name)};
    result.version      = ToolVersion{};
    result.description  = "test";
    result.input_schema = nlohmann::json::object();
    return result;
}

std::vector<std::string> tool_names(const PromptAssembly& assembly) {
    std::vector<std::string> names;
    for (const ToolSchema& schema : assembly.tools) {
        names.push_back(schema.name.value);
    }
    return names;
}

std::vector<std::string> context_names(const PromptAssembly& assembly) {
    std::vector<std::string> names;
    for (const AssembledContext& context : assembly.contexts) {
        names.push_back(context.name);
    }
    return names;
}

const AssembledSection* find_section(const PromptAssembly& assembly, const std::string& name) {
    for (const AssembledSection& section : assembly.sections) {
        if (section.name == name) {
            return &section;
        }
    }
    return nullptr;
}

const PresetRow* persona_row(const AgentPreset& preset) {
    for (const PresetRow& row : preset.rows) {
        if (row.persona.has_value()) {
            return &row;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// 52-D11/D13: roster, reserved ids, shipped root
// ---------------------------------------------------------------------------

TEST(Preset52Roster, ShippedRosterAndReservedIds) {
    RosterEnv      env("spec52_roster");
    ScopedEnv      presets_dir{"YMH_PRESETS_DIR", env.shipped_root.string()};
    write_preset(env.shipped_root, "minimal", kMinimalPreset);
    write_preset(env.shipped_root, "standard", kStandardPreset);

    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, shipped_config());

    std::vector<std::string> ids;
    for (const AgentPreset& preset : roster.list()) {
        ids.push_back(preset.id);
    }
    EXPECT_EQ(ids, (std::vector<std::string>{"minimal", "standard"}));

    EXPECT_EQ(reserved_ids(), (std::vector<std::string>{"ptc", "cordis"}));
    EXPECT_THROW((void)roster.resolve("ptc"), PresetUnavailable);
    EXPECT_THROW((void)roster.resolve("cordis"), PresetUnavailable);
    EXPECT_THROW((void)roster.resolve("nope"), PresetNotFound);
}

TEST(Preset52Roster, ShippedRootMissingWarnsOnceAndStandardAbsent) {
    RosterEnv env("spec52_root_missing");
    ScopedEnv presets_dir{"YMH_PRESETS_DIR", (env.workspace.path() / "absent").string()};

    CapturingLogger   logger;
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, shipped_config(),
                             &logger);

    EXPECT_THROW((void)roster.resolve("standard"), PresetNotFound);
    ASSERT_EQ(logger.warnings.size(), 1u);
    EXPECT_NE(logger.warnings.front().find("shipped root"), std::string::npos);
}

// ---------------------------------------------------------------------------
// 52-D14/I9/I10: persona + scoped runtime-context suppression
// ---------------------------------------------------------------------------

TEST(Preset52Persona, MinimalPersonaCompleteSuppressesRuntimeContext) {
    RosterEnv env("spec52_minimal");
    ScopedEnv presets_dir{"YMH_PRESETS_DIR", env.shipped_root.string()};
    write_preset(env.shipped_root, "minimal", kMinimalPreset);
    write_preset(env.shipped_root, "standard", kStandardPreset);

    const ContextHandle runtime_handle =
        register_runtime_context(env.prompt,
                                 RuntimeContextConfig{.cwd = "/tmp/ws", .model = "test-model", .sandbox = {}, .approval = {}, .delegation = {}});
    (void)runtime_handle;

    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, shipped_config());
    const SessionId    sid = make_session(env.sessions, env.workspace.path());

    AgentContext minimal;
    minimal.agent = AgentId{"minimal-agent"};
    roster.mount(minimal, "minimal");
    const PromptAssembly minimal_assembly =
        env.prompt.assemble(AssembleContext{.scope = minimal.scope});
    const AssembledSection* persona = find_section(minimal_assembly, "deployment:persona-prefix");
    ASSERT_NE(persona, nullptr);
    EXPECT_TRUE(persona->complete);
    EXPECT_EQ(persona->text, "You are a helpful software engineer assistant.");
    EXPECT_EQ(context_names(minimal_assembly), (std::vector<std::string>{}));

    AgentContext standard;
    standard.agent = AgentId{"standard-agent"};
    roster.mount(standard, "standard");
    const PromptAssembly standard_assembly =
        env.prompt.assemble(AssembleContext{.scope = standard.scope});
    const AssembledSection* standard_persona =
        find_section(standard_assembly, "deployment:persona-prefix");
    ASSERT_NE(standard_persona, nullptr);
    EXPECT_FALSE(standard_persona->complete);
    EXPECT_EQ(context_names(standard_assembly), (std::vector<std::string>{"runtime-context"}));
}

TEST(Preset52Persona, StandardPersonaEqualsDefault) {
    RosterEnv env("spec52_standard_persona");
    ScopedEnv presets_dir{"YMH_PRESETS_DIR", env.shipped_root.string()};
    write_preset(env.shipped_root, "standard", kStandardPreset);

    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, shipped_config());
    const PresetRow*   row = persona_row(roster.resolve("standard"));
    ASSERT_NE(row, nullptr);
    const PersonaConfig expected = default_persona_config();
    EXPECT_EQ(row->persona->prefix, expected.prefix);
    EXPECT_EQ(row->persona->suffix, expected.suffix);
    EXPECT_EQ(row->persona->complete, expected.complete);
    EXPECT_EQ(row->persona->include_runtime_context, expected.include_runtime_context);
}

// ---------------------------------------------------------------------------
// 52-F15/F13/F17: loader failures
// ---------------------------------------------------------------------------

TEST(Preset52Loader, PersonaAndLegacyPairRejected) {
    RosterEnv env("spec52_persona_legacy");
    write_preset(env.shipped_root, "bad", R"({
        "rows": [{"id":"r1","persona":{"prefix":"A"},"persona_suffix":"B"}]
    })");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_only_config(env.shipped_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(Preset52Loader, DuplicatePersonaRejected) {
    RosterEnv env("spec52_persona_dup");
    write_preset(env.shipped_root, "bad", R"({
        "rows": [
            {"id":"r1","persona":{"prefix":"A"}},
            {"id":"r2","persona":{"prefix":"B"}}
        ]
    })");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_only_config(env.shipped_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(Preset52Loader, PhantomCapabilityRejected) {
    RosterEnv env("spec52_phantom");
    write_preset(env.shipped_root, "bad", R"({
        "rows": [{"id":"r1","capabilities":["workflow"]}]
    })");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_only_config(env.shipped_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(Preset52Loader, UnknownPermissionPresetRejected) {
    RosterEnv env("spec52_unknown_perm");
    write_preset(env.shipped_root, "bad", R"({
        "rows": [{"id":"r1","permission_preset":"does-not-exist"}]
    })");
    AgentPresetRoster roster(
        env.prompt, env.tools, env.skills, env.sessions,
        root_only_config(env.shipped_root),
        nullptr, std::vector<std::string>{"workspace-write", "danger-full-access"});
    EXPECT_THROW((void)roster.list(), ConfigError);
}

// ---------------------------------------------------------------------------
// 52-I11/I14: narrowing only, no profiles section
// ---------------------------------------------------------------------------

TEST(Preset52NoWiden, PermissionPresetCannotWidenSandbox) {
    const PermissionPresetSettings workspace{"workspace", "ask"};
    const PermissionPresetSettings full{"unrestricted", "never"};
    EXPECT_FALSE(permission_preset_narrows(workspace, full));
    EXPECT_EQ(narrow_permission_preset(workspace, full).sandbox, "workspace");
    EXPECT_TRUE(permission_preset_narrows(full, workspace));
    EXPECT_EQ(narrow_permission_preset(full, workspace).sandbox, "workspace");
}

TEST(Preset52NoWiden, ChildToolFilterCannotAddTools) {
    RosterEnv env("spec52_nowiden_tools");
    env.prompt.set_tool_provider([](const AssembleContext&) {
        return std::vector<ToolSchema>{tool_schema("read"), tool_schema("shell"),
                                       tool_schema("grep")};
    });
    write_preset(env.shipped_root, "alpha", R"({
        "rows": [{"id":"r1","tools":{"allow":["read"]}}]
    })");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_only_config(env.shipped_root));

    AgentContext parent;
    parent.agent = AgentId{"parent-agent"};
    roster.mount(parent, "alpha");

    const SessionId sid = make_session(env.sessions, env.workspace.path());
    FakeAgent       parent_agent{AgentId{"parent-agent"}, sid};

    ChildComposition composition;
    composition.tool_filter = ToolRestriction{.allow = {"read", "shell", "grep"}, .deny = {}};
    AgentContext child;
    child.agent = AgentId{"child-agent"};
    roster.apply_child_composition(child, parent_agent, composition);

    const PromptAssembly assembly = env.prompt.assemble(AssembleContext{.scope = child.scope});
    EXPECT_EQ(tool_names(assembly), (std::vector<std::string>{"read"}));
}

TEST(Preset52NoWiden, ProfilesSectionRejected) {
    TempWorkspace workspace("spec52_profiles");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    std::ofstream(global, std::ios::binary)
        << R"({"profiles": {"balanced": {"model": "x"}}})";
    ConfigPaths paths;
    paths.global = global;
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

// ---------------------------------------------------------------------------
// 52-D15/I12: permission preset resolution and session pinning
// ---------------------------------------------------------------------------

TEST(Preset52Permission, DefaultPresetResolution) {
    Config config;
    EXPECT_EQ(default_permission_preset_name(config), "workspace-write");
    EXPECT_EQ(deployment_permission_baseline(config).sandbox, "workspace");

    config.permissions.default_preset = "danger-full-access";
    EXPECT_EQ(default_permission_preset_name(config), "danger-full-access");
    EXPECT_EQ(deployment_permission_baseline(config).sandbox, "unrestricted");

    EXPECT_THROW((void)resolve_permission_preset(config.permissions, "missing"), ConfigError);
    EXPECT_EQ(resolve_permission_preset(config.permissions, std::nullopt).sandbox, "workspace");
}

TEST(Preset52Permission, ConfigKeysParseAndValidate) {
    TempWorkspace               workspace("spec52_perm_config");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    std::ofstream(global, std::ios::binary) << R"({
        "agent": {"sandbox": "read-only"},
        "permissions": {
            "presets": {
                "workspace-write": {"sandbox": "workspace", "approval": "ask"},
                "danger-full-access": {"sandbox": "unrestricted", "approval": "never"},
                "read-only-plus": {"sandbox": "read-only"}
            },
            "default_preset": "read-only-plus"
        }
    })";
    ConfigPaths paths;
    paths.global            = global;
    const Config config     = load_config(paths);
    EXPECT_EQ(config.agent.sandbox, "read-only");
    EXPECT_EQ(config.permissions.presets.at("danger-full-access").approval, "never");
    EXPECT_EQ(config.permissions.default_preset, "read-only-plus");
    EXPECT_EQ(deployment_permission_baseline(config).sandbox, "read-only");

    std::ofstream(global, std::ios::binary) << R"({"agent": {"sandbox": "wide-open"}})";
    EXPECT_THROW((void)load_config(paths), ConfigError);
    std::ofstream(global, std::ios::binary) << R"({"permissions": {"default_preset": "nope"}})";
    EXPECT_THROW((void)load_config(paths), ConfigError);
    std::ofstream(global, std::ios::binary)
        << R"({"permissions": {"presets": {"x": {"approval": "sometimes"}}}})";
    EXPECT_THROW((void)load_config(paths), ConfigError);
}

TEST(Preset52Permission, UnknownPresetKeyNamesThePreset) {
    TempWorkspace               workspace("spec52_perm_diag");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    std::ofstream(global, std::ios::binary)
        << R"({"permissions": {"presets": {"foo": {"bogus": 1}}}})";
    ConfigPaths paths;
    paths.global = global;
    try {
        (void)load_config(paths);
        FAIL() << "an unknown key inside a preset must be rejected";
    } catch (const ConfigError& error) {
        const std::string message = error.what();
        EXPECT_NE(message.find("permissions.presets.foo.bogus"), std::string::npos) << message;
    }
}

TEST(Preset52Permission, EffectiveSandboxModeClampsAndValidates) {
    Config config;
    EXPECT_EQ(effective_sandbox_mode(config), SandboxMode::Workspace);

    config.agent.sandbox = "unrestricted";
    EXPECT_EQ(effective_sandbox_mode(config), SandboxMode::Unrestricted);

    config.permissions.default_preset = "workspace-write";
    EXPECT_EQ(effective_sandbox_mode(config), SandboxMode::Workspace);

    config.permissions.default_preset.clear();
    config.agent.sandbox = "bogus";
    EXPECT_THROW((void)effective_sandbox_mode(config), ConfigError);
}

TEST(Preset52Permission, WorkspaceLayerCannotDefineOrSelectPresets) {
    TempWorkspace               workspace("spec52_perm_layer");
    const std::filesystem::path global = workspace.path() / "global.jsonc";
    const std::filesystem::path local  = workspace.path() / ".ymh" / "config.jsonc";
    std::filesystem::create_directories(local.parent_path());
    std::ofstream(global, std::ios::binary) << R"({"permissions": {"shell": "deny"}})";

    ConfigPaths paths;
    paths.global    = global;
    paths.workspace = local;

    std::ofstream(local, std::ios::binary)
        << R"({"permissions": {"default_preset": "danger-full-access"}})";
    EXPECT_THROW((void)load_config(paths), ConfigError);

    std::ofstream(local, std::ios::binary)
        << R"({"permissions": {"presets": {"evil": {"sandbox": "unrestricted", "approval": "never"}}}})";
    EXPECT_THROW((void)load_config(paths), ConfigError);

    std::ofstream(local, std::ios::binary) << R"({"permissions": {"shell": "deny"}})";
    const Config narrowed = load_config(paths);
    EXPECT_EQ(narrowed.permissions.shell, "deny");
}

TEST(Preset52Permission, SessionPinningSurvivesConfigChangeAndFork) {
    RosterEnv env("spec52_pin");
    SessionOptions options;
    options.cwd               = env.workspace.path();
    options.model             = "test-model";
    options.serverProfile     = "interactive";
    options.permission_preset = "workspace-write";
    const SessionId sid       = env.sessions.createSession(options);

    std::shared_ptr<Session> session = env.sessions.sessionPtr(sid);
    ASSERT_TRUE(session->header().permission_preset.has_value());
    EXPECT_EQ(*session->header().permission_preset, "workspace-write");

    Config changed;
    changed.permissions.default_preset = "danger-full-access";
    EXPECT_EQ(default_permission_preset_name(changed), "danger-full-access");
    EXPECT_EQ(*session->header().permission_preset, "workspace-write");

    const SessionId          forked = env.sessions.forkSession(sid, 0);
    std::shared_ptr<Session> forked_session = env.sessions.sessionPtr(forked);
    ASSERT_TRUE(forked_session->header().permission_preset.has_value());
    EXPECT_EQ(*forked_session->header().permission_preset, "workspace-write");
}

// ---------------------------------------------------------------------------
// 52 review (3B): the preset instruction override
// ---------------------------------------------------------------------------

TEST(Preset52Instructions, RowOverrideIsFolded) {
    RosterEnv env("spec52_instructions");
    ScopedEnv presets_dir{"YMH_PRESETS_DIR", env.shipped_root.string()};
    write_preset(env.shipped_root, "standard", R"({
        "id": "standard",
        "display_name": "Standard",
        "rows": [
            {"id": "instructions", "instructions": {"max_bytes": 4096}},
            {"id": "later", "instructions": {"enabled": false, "max_bytes": 8192}}
        ]
    })");

    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, shipped_config());

    const std::optional<PresetInstructions> folded = roster.instructions_for("standard");
    ASSERT_TRUE(folded.has_value());
    EXPECT_FALSE(folded->enabled);
    ASSERT_TRUE(folded->max_bytes.has_value());
    EXPECT_EQ(*folded->max_bytes, 8192u);

    EXPECT_FALSE(roster.instructions_for("absent").has_value());
    EXPECT_FALSE(roster.instructions_for(std::nullopt).has_value());
}

TEST(Preset52Instructions, ShippedStandardRowIsNotANoOp) {
    RosterEnv env("spec52_shipped_instructions");
    PresetConfig config = root_only_config(std::filesystem::path{YMH_SOURCE_DIR} / "presets");
    config.default_id   = "standard";
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, config);

    const std::optional<PresetInstructions> standard = roster.instructions_for("standard");
    ASSERT_TRUE(standard.has_value()) << "the shipped standard 'instructions' row must bind";
    EXPECT_TRUE(standard->enabled);
    ASSERT_TRUE(standard->max_bytes.has_value());
    EXPECT_EQ(*standard->max_bytes, 65536u);

    EXPECT_FALSE(roster.instructions_for("minimal").has_value());
}

// ---------------------------------------------------------------------------
// 52-D16: capability catalog
// ---------------------------------------------------------------------------

TEST(Preset52Capability, CatalogDispositions) {
    const CapabilitySpec* workflow = find_capability("workflow");
    ASSERT_NE(workflow, nullptr);
    EXPECT_EQ(workflow->disposition, CapabilityDisposition::Out);

    const CapabilitySpec* skills = find_capability("skills");
    ASSERT_NE(skills, nullptr);
    EXPECT_EQ(skills->disposition, CapabilityDisposition::In);

    const CapabilitySpec* plan = find_capability("plan-mode");
    ASSERT_NE(plan, nullptr);
    EXPECT_EQ(plan->disposition, CapabilityDisposition::InNotPresetControlled);
    EXPECT_EQ(find_capability("not-a-capability"), nullptr);
}

} // namespace
