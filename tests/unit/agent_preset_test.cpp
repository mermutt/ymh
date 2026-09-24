#include <gtest/gtest.h>

#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
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
#include "ymh/prompt/system_prompt.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/skills/skill_catalog.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

void write_preset(const std::filesystem::path& root, const std::string& id,
                  const std::string& body) {
    const std::filesystem::path dir = root / id;
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "preset.jsonc", std::ios::binary) << body;
}

PresetConfig root_config(const std::filesystem::path& root) {
    PresetConfig config;
    config.root                = root;
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
    void onSettled(std::function<void()>) override {}

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
                 logger) {
        std::error_code error;
        std::filesystem::remove_all(default_presets_user_root(), error);
        std::filesystem::create_directories(presets_root, error);
    }

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
    std::filesystem::path presets_root = workspace.path() / "presets";
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

std::vector<std::string> section_names(const PromptAssembly& assembly) {
    std::vector<std::string> names;
    for (const AssembledSection& section : assembly.sections) {
        names.push_back(section.name);
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

std::vector<std::string> tool_names(const PromptAssembly& assembly) {
    std::vector<std::string> names;
    for (const ToolSchema& schema : assembly.tools) {
        names.push_back(schema.name.value);
    }
    return names;
}

std::string section_text(const PromptAssembly& assembly, const std::string& name) {
    for (const AssembledSection& section : assembly.sections) {
        if (section.name == name) {
            return section.text;
        }
    }
    return {};
}

std::string context_text(const PromptAssembly& assembly, const std::string& name) {
    for (const AssembledContext& context : assembly.contexts) {
        if (context.name == name) {
            return context.text;
        }
    }
    return {};
}

AgentContext mount_agent(AgentPresetRoster& roster, const AgentId& agent,
                         const std::string& preset) {
    AgentContext ctx;
    ctx.agent = agent;
    roster.mount(ctx, preset);
    return ctx;
}

// ---------------------------------------------------------------------------
// Roster: resolve / list / standing key
// ---------------------------------------------------------------------------

TEST(AgentPresetRoster, ResolveDefaultAndUnknown) {
    RosterEnv env("preset_resolve");
    write_preset(env.presets_root, "standard", R"({"display_name":"Standard"})");
    write_preset(env.presets_root, "second", "{}");

    PresetConfig config = root_config(env.presets_root);
    config.default_id   = "standard";
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, config);

    EXPECT_EQ(roster.resolve(std::nullopt).id, "standard");
    EXPECT_EQ(roster.resolve("second").id, "second");
    EXPECT_EQ(roster.resolve("second").display_name, "second");
    EXPECT_EQ(roster.list().size(), 2u);
    EXPECT_THROW((void)roster.resolve("missing"), PresetNotFound);
}

TEST(AgentPresetRoster, ResolveNulloptWithoutDefaultThrows) {
    RosterEnv env("preset_no_default");
    write_preset(env.presets_root, "standard", "{}");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    EXPECT_THROW((void)roster.resolve(std::nullopt), PresetNotFound);
}

TEST(AgentPresetRoster, StandingKeyIsStableAndPresetScoped) {
    RosterEnv env("preset_standing");
    write_preset(env.presets_root, "standard", "{}");
    PresetConfig config = root_config(env.presets_root);
    config.default_id   = "standard";
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, config);

    EXPECT_EQ(roster.standing_key_for("standard"), "preset:standard");
    EXPECT_EQ(roster.standing_key_for("standard"), roster.standing_key_for("standard"));
    EXPECT_EQ(roster.standing_key_for(std::nullopt), "preset:standard");
}

TEST(AgentPresetRoster, ListHonorsUserRootFlag) {
    RosterEnv env("preset_user_root");
    write_preset(default_presets_user_root(), "user-preset", "{}");

    PresetConfig config = root_config(env.presets_root);
    config.include_user_root = true;
    {
        AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, config);
        ASSERT_EQ(roster.list().size(), 1u);
        EXPECT_EQ(roster.list().front().id, "user-preset");
    }
    config.include_user_root = false;
    AgentPresetRoster disabled(env.prompt, env.tools, env.skills, env.sessions, config);
    EXPECT_TRUE(disabled.list().empty());
}

TEST(AgentPresetRoster, DuplicateIdAcrossRootsFailsLoad) {
    RosterEnv env("preset_dup_roots");
    write_preset(env.presets_root, "dup", "{}");
    write_preset(default_presets_user_root(), "dup", "{}");

    PresetConfig config = root_config(env.presets_root);
    config.include_user_root = true;
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, config);
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

// ---------------------------------------------------------------------------
// Loader validation (42-F8)
// ---------------------------------------------------------------------------

TEST(AgentPresetLoader, UnknownTopLevelKeyFails) {
    RosterEnv env("preset_unknown_top");
    write_preset(env.presets_root, "bad", R"({"bogus":1})");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(AgentPresetLoader, UnknownRowKeyFails) {
    RosterEnv env("preset_unknown_row");
    write_preset(env.presets_root, "bad", R"({"rows":[{"id":"a","bogus":1}]})");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(AgentPresetLoader, MissingOrEmptyRowIdFails) {
    RosterEnv env("preset_missing_row_id");
    write_preset(env.presets_root, "bad", R"({"rows":[{"group":"g"}]})");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(AgentPresetLoader, EmptyRowIdFails) {
    RosterEnv env("preset_empty_row_id");
    write_preset(env.presets_root, "bad", R"({"rows":[{"id":""}]})");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(AgentPresetLoader, DuplicateRowIdFails) {
    RosterEnv env("preset_dup_row_id");
    write_preset(env.presets_root, "bad", R"({"rows":[{"id":"a"},{"id":"a"}]})");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(AgentPresetLoader, DuplicateSectionNameAcrossRowsFails) {
    RosterEnv env("preset_dup_section");
    write_preset(env.presets_root, "bad",
                 R"({"rows":[{"id":"a","sections":[{"name":"s"}]},{"id":"b","sections":[{"name":"s"}]}]})");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(AgentPresetLoader, IdMismatchWithDirectoryFails) {
    RosterEnv env("preset_id_mismatch");
    write_preset(env.presets_root, "bad", R"({"id":"other"})");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(AgentPresetLoader, DirectoryWithoutPresetFileIsSkipped) {
    RosterEnv env("preset_skip_dir");
    std::filesystem::create_directories(env.presets_root / "not-a-preset");
    write_preset(env.presets_root, "standard", "{}");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    ASSERT_EQ(roster.list().size(), 1u);
    EXPECT_EQ(roster.list().front().id, "standard");
}

TEST(AgentPresetLoader, EscapingSkillRootFails) {
    RosterEnv env("preset_skill_escape");
    write_preset(env.presets_root, "bad", R"({"rows":[{"id":"a","skills":["../outside"]}]})");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(AgentPresetLoader, NonStringSkillRootFails) {
    RosterEnv env("preset_skill_type");
    write_preset(env.presets_root, "bad", R"({"rows":[{"id":"a","skills":[1]}]})");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    EXPECT_THROW((void)roster.list(), PresetLoadError);
}

TEST(AgentPresetLoader, RowVocabularyIsParsed) {
    RosterEnv env("preset_parse_rows");
    write_preset(env.presets_root, "standard", R"({
        "display_name": "Standard",
        "rows": [{
            "id": "persona",
            "group": "core",
            "disabled": true,
            "persona": {"prefix": "You are", "suffix": "cwd"},
            "tools": {"allow": ["read"], "deny": ["shell"]},
            "skills": ["skills/editing"],
            "sections": [{"name": "extra", "order": 50, "complete": true, "text": "body"}],
            "config": {"k": "v"}
        }]
    })");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));
    const AgentPreset& preset = roster.resolve("standard");
    ASSERT_EQ(preset.rows.size(), 1u);
    const PresetRow& row = preset.rows.front();
    EXPECT_EQ(row.id, "persona");
    EXPECT_EQ(row.group, "core");
    EXPECT_TRUE(row.disabled);
    ASSERT_TRUE(row.persona.has_value());
    EXPECT_EQ(row.persona->prefix, "You are");
    EXPECT_EQ(row.persona->suffix, "cwd");
    ASSERT_TRUE(row.tool_filter.has_value());
    EXPECT_EQ(row.tool_filter->allow, (std::vector<std::string>{"read"}));
    EXPECT_EQ(row.tool_filter->deny, (std::vector<std::string>{"shell"}));
    ASSERT_EQ(row.sections.size(), 1u);
    EXPECT_EQ(row.sections.front().name, "extra");
    EXPECT_EQ(row.sections.front().order, 50);
    EXPECT_TRUE(row.sections.front().complete);
    ASSERT_TRUE(row.config.has_value());
    EXPECT_EQ(nlohmann::json::parse(*row.config).at("k"), "v");
}

// ---------------------------------------------------------------------------
// Blank predicate and the select path
// ---------------------------------------------------------------------------

TEST(AgentPresetBlank, TruthTable) {
    RosterEnv env("preset_blank");
    const SessionId sid = make_session(env.sessions, env.workspace.path());
    auto            session = env.sessions.sessionPtr(sid);

    EXPECT_TRUE(AgentPresetRoster::is_blank(*session));

    session->append(payload::PlanMode{true});
    session->append(payload::TokenUsage{});
    session->append(payload::SessionRenamed{"renamed", payload::RenameOrigin::User});
    EXPECT_TRUE(AgentPresetRoster::is_blank(*session));
}

TEST(AgentPresetBlank, UserMessageMakesNonBlank) {
    RosterEnv env("preset_blank_user");
    const SessionId sid = make_session(env.sessions, env.workspace.path());
    auto            session = env.sessions.sessionPtr(sid);

    payload::UserMessage user;
    user.id = "m1";
    session->append(user);
    EXPECT_FALSE(AgentPresetRoster::is_blank(*session));
}

TEST(AgentPresetBlank, DelegationAndRequestHeaderMakeNonBlank) {
    RosterEnv env("preset_blank_events");
    const SessionId sid = make_session(env.sessions, env.workspace.path());
    auto            session = env.sessions.sessionPtr(sid);

    session->append(payload::SubagentSpawned{.subagent = SessionId{"child"}, .task = "t"});
    EXPECT_FALSE(AgentPresetRoster::is_blank(*session));

    const SessionId other = make_session(env.sessions, env.workspace.path());
    auto            otherSession = env.sessions.sessionPtr(other);
    otherSession->append(payload::LlmRequestHeader{});
    EXPECT_FALSE(AgentPresetRoster::is_blank(*otherSession));
}

TEST(AgentPresetSelect, BlankSessionSwitchesAndAppendsOneEvent) {
    RosterEnv env("preset_select_blank");
    write_preset(env.presets_root, "standard", "{}");
    write_preset(env.presets_root, "second", "{}");
    PresetConfig config = root_config(env.presets_root);
    config.default_id   = "standard";
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, config);

    const SessionId sid = make_session(env.sessions, env.workspace.path());
    FakeAgent       agent{AgentId{"agent-1"}, sid};
    AgentContext    ctx;
    ctx.agent = agent.id();
    roster.mount(ctx, "standard");
    EXPECT_EQ(roster.composed_preset(roster.leaf_for(agent.id())), "standard");

    roster.select(agent, "second");
    EXPECT_EQ(roster.composed_preset(roster.leaf_for(agent.id())), "second");

    auto session = env.sessions.sessionPtr(sid);
    std::size_t selections = 0;
    for (const EventRecord& record : session->ownEvents()) {
        if (record.event.type == EventType::AgentPresetSelected) {
            ++selections;
            EXPECT_EQ(record.event.payload.at("agent_preset"), "second");
        }
    }
    EXPECT_EQ(selections, 1u);
    EXPECT_TRUE(session->deriveMessages().empty());
}

TEST(AgentPresetSelect, NonBlankSessionIsRejectedWithoutAppending) {
    RosterEnv env("preset_select_fixed");
    write_preset(env.presets_root, "standard", "{}");
    write_preset(env.presets_root, "second", "{}");
    PresetConfig config = root_config(env.presets_root);
    config.default_id   = "standard";
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, config);

    const SessionId sid = make_session(env.sessions, env.workspace.path());
    FakeAgent       agent{AgentId{"agent-2"}, sid};
    AgentContext    ctx;
    ctx.agent = agent.id();
    roster.mount(ctx, "standard");

    auto session = env.sessions.sessionPtr(sid);
    payload::UserMessage user;
    user.id = "m1";
    session->append(user);

    EXPECT_THROW(roster.select(agent, "second"), CompositionFixed);
    EXPECT_EQ(roster.composed_preset(roster.leaf_for(agent.id())), "standard");
    for (const EventRecord& record : session->ownEvents()) {
        EXPECT_NE(record.event.type, EventType::AgentPresetSelected);
    }
}

TEST(AgentPresetSelect, UnmountedAgentThrows) {
    RosterEnv env("preset_select_unmounted");
    write_preset(env.presets_root, "standard", "{}");
    write_preset(env.presets_root, "second", "{}");
    PresetConfig config = root_config(env.presets_root);
    config.default_id   = "standard";
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, config);

    const SessionId sid = make_session(env.sessions, env.workspace.path());
    FakeAgent       agent{AgentId{"agent-3"}, sid};
    EXPECT_THROW(roster.select(agent, "second"), UnknownAgent);
    EXPECT_THROW((void)roster.leaf_for(AgentId{"never-mounted"}), UnknownAgent);
}

// ---------------------------------------------------------------------------
// Event codec and the header
// ---------------------------------------------------------------------------

TEST(AgentPresetEvent, PayloadJsonCarriesOnlyPresetKey) {
    payload::AgentPresetSelected value{.agent_preset = "standard"};
    const nlohmann::json        json = value;
    ASSERT_EQ(json.size(), 1u);
    EXPECT_EQ(json.at("agent_preset"), "standard");
    EXPECT_EQ(json.get<payload::AgentPresetSelected>().agent_preset, "standard");

    EXPECT_EQ(wire_name(EventType::AgentPresetSelected), "agent_preset/selected");
    ASSERT_TRUE(parse_event_type("agent_preset/selected").has_value());
    EXPECT_EQ(*parse_event_type("agent_preset/selected"), EventType::AgentPresetSelected);
}

TEST(AgentPresetEvent, EncodeDecodeRoundTrip) {
    TypedEvent<payload::AgentPresetSelected> typed;
    typed.id         = EventId{"evt-1"};
    typed.session_id = SessionId{"s1"};
    typed.timestamp  = std::chrono::system_clock::time_point{std::chrono::milliseconds{1234}};
    typed.payload    = payload::AgentPresetSelected{.agent_preset = "standard"};

    const Event  erased   = encode(typed);
    const auto   restored = decode<payload::AgentPresetSelected>(erased);
    EXPECT_EQ(restored.payload.agent_preset, "standard");
}

TEST(SessionHeaderPreset, JsonRoundTripCarriesPresetAndDepth) {
    SessionHeader header;
    header.id            = make_session_id();
    header.cwd           = std::filesystem::temp_directory_path();
    header.model         = "test";
    header.serverProfile = "interactive";
    header.agent_preset  = "standard";
    header.depth         = 2;

    const nlohmann::json json = header;
    const SessionHeader  back = json.get<SessionHeader>();
    ASSERT_TRUE(back.agent_preset.has_value());
    EXPECT_EQ(*back.agent_preset, "standard");
    EXPECT_EQ(back.depth, 2u);
}

TEST(SessionManagerPreset, CreateSessionCopiesPresetAndDepth) {
    RosterEnv env("preset_create_copy");
    SessionOptions options;
    options.cwd           = env.workspace.path();
    options.model         = "test";
    options.title         = "t";
    options.serverProfile = "interactive";
    options.agent_preset  = "standard";
    options.depth         = 3;

    const SessionId sid = env.sessions.createSession(options);
    auto            session = env.sessions.sessionPtr(sid);
    ASSERT_TRUE(session->header().agent_preset.has_value());
    EXPECT_EQ(*session->header().agent_preset, "standard");
    EXPECT_EQ(session->header().depth, 3u);
}

TEST(PresetHeaderCodec, ReservedKeysRoundTrip) {
    TempWorkspace workspace("preset_codec");
    PersistenceConfig config;
    config.db_path   = workspace.path() / ".ymh" / "sessions.db";
    config.lock_path = workspace.path() / ".ymh" / "sessions.lock";
    config.boot_id   = BootId{"boot-1"};
    auto store = SessionPersistence::open(config);

    SessionHeader header;
    header.id            = make_session_id();
    header.cwd           = std::filesystem::canonical(workspace.path());
    header.model         = "test";
    header.serverProfile = "interactive";
    header.metadata      = R"({"k":"v"})";
    header.agent_preset  = "standard";
    header.depth         = 2;
    store->create(header);

    const auto loaded = store->load(header.id);
    ASSERT_TRUE(loaded.has_value());
    ASSERT_TRUE(loaded->metadata.has_value());
    EXPECT_EQ(*loaded->metadata, R"({"k":"v"})");
    ASSERT_TRUE(loaded->agent_preset.has_value());
    EXPECT_EQ(*loaded->agent_preset, "standard");
    EXPECT_EQ(loaded->depth, 2u);
}

TEST(PresetHeaderCodec, ReservedKeyCollisionFailsLoud) {
    TempWorkspace workspace("preset_codec_collision");
    PersistenceConfig config;
    config.db_path   = workspace.path() / ".ymh" / "sessions.db";
    config.lock_path = workspace.path() / ".ymh" / "sessions.lock";
    config.boot_id   = BootId{"boot-1"};
    auto store = SessionPersistence::open(config);

    SessionHeader header;
    header.id            = make_session_id();
    header.cwd           = std::filesystem::canonical(workspace.path());
    header.model         = "test";
    header.serverProfile = "interactive";
    header.metadata      = R"({"agent_preset":"x"})";
    header.agent_preset  = "standard";
    store->create(header);

    EXPECT_THROW((void)store->load(header.id), CorruptionError);
}

// ---------------------------------------------------------------------------
// Scope registry: the mount rule Wave 3 deferred (42 §3.2, 42-D3)
// ---------------------------------------------------------------------------

TEST(PromptScope, DuplicateAndUnknownParentFailLoud) {
    SystemPrompt prompt;
    ScopeHandle  standing = prompt.scope("", "preset:alpha");
    EXPECT_THROW((void)prompt.scope("", "preset:alpha"), ConfigError);
    EXPECT_THROW((void)prompt.scope("preset:missing", "session:x"), ConfigError);

    PromptSection section;
    section.name = "dup";
    section.text = [](const AssembleContext&) { return std::string{"body"}; };
    standing.add_section(section);
    EXPECT_THROW(standing.add_section(section), ConfigError);
}

TEST(PromptScope, DestroyingAStandingScopeUnwindsItsLeaves) {
    SystemPrompt prompt;
    PromptSection global;
    global.name = "global";
    global.text = [](const AssembleContext&) { return std::string{"G"}; };
    SectionHandle global_handle = prompt.section(global);
    (void)global_handle;

    {
        ScopeHandle standing = prompt.scope("", "preset:alpha");
        PromptSection section;
        section.name = "scoped";
        section.text = [](const AssembleContext&) { return std::string{"S"}; };
        standing.add_section(section);
        ScopeHandle leaf = prompt.scope("preset:alpha", "session:1");
        EXPECT_EQ(section_names(prompt.assemble(AssembleContext{.scope = ScopeKey{"session:1"}})),
                  (std::vector<std::string>{"global", "scoped"}));
    }
    EXPECT_EQ(section_names(prompt.assemble(AssembleContext{.scope = ScopeKey{"session:1"}})),
              (std::vector<std::string>{"global"}));
}

// ---------------------------------------------------------------------------
// The standing mount and the per-session joined scopes (42 §2.2, 42-I1/I2)
// ---------------------------------------------------------------------------

TEST(AgentPresetMount, StandingSectionsAreVisibleToJoinedLeavesOnly) {
    RosterEnv env("preset_mount_join");
    write_preset(env.presets_root, "alpha", R"({
        "rows": [{"id":"r1","sections":[{"name":"alpha:section","order":10,"text":"A"}]}]
    })");
    write_preset(env.presets_root, "beta", R"({
        "rows": [{"id":"r1","sections":[{"name":"beta:section","order":10,"text":"B"}]}]
    })");
    PresetConfig config = root_config(env.presets_root);
    config.default_id   = "alpha";
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, config);

    const AgentContext a1 = mount_agent(roster, AgentId{"agent-a1"}, "alpha");
    const AgentContext a2 = mount_agent(roster, AgentId{"agent-a2"}, "alpha");
    const AgentContext b1 = mount_agent(roster, AgentId{"agent-b1"}, "beta");

    EXPECT_EQ(roster.composed_preset(roster.leaf_for(AgentId{"agent-a1"})), "alpha");
    EXPECT_NE(a1.scope, a2.scope);
    EXPECT_EQ(section_names(env.prompt.assemble(AssembleContext{.scope = a1.scope})),
              (std::vector<std::string>{"alpha:section"}));
    EXPECT_EQ(section_names(env.prompt.assemble(AssembleContext{.scope = a2.scope})),
              (std::vector<std::string>{"alpha:section"}));
    EXPECT_EQ(section_names(env.prompt.assemble(AssembleContext{.scope = b1.scope})),
              (std::vector<std::string>{"beta:section"}));
}

TEST(AgentPresetMount, SecondMountRegistersNothingTwice) {
    RosterEnv env("preset_mount_idempotent");
    write_preset(env.presets_root, "alpha", R"({
        "rows": [{"id":"r1","sections":[{"name":"alpha:section","order":10,"text":"A"}]}]
    })");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));

    AgentContext first = mount_agent(roster, AgentId{"agent-1"}, "alpha");
    const std::vector<std::string> before =
        section_names(env.prompt.assemble(AssembleContext{.scope = first.scope}));

    AgentContext second = mount_agent(roster, AgentId{"agent-2"}, "alpha");
    mount_agent(roster, AgentId{"agent-1"}, "alpha");
    (void)second;

    EXPECT_EQ(section_names(env.prompt.assemble(AssembleContext{.scope = first.scope})), before);
    EXPECT_EQ(before, (std::vector<std::string>{"alpha:section"}));
}

TEST(AgentPresetScope, LeafShadowsStandingAndGlobal) {
    RosterEnv env("preset_scope_shadow");
    PromptSection global;
    global.name  = "shared";
    global.order = 0;
    global.text  = [](const AssembleContext&) { return std::string{"GLOBAL"}; };
    SectionHandle global_handle = env.prompt.section(global);
    (void)global_handle;

    write_preset(env.presets_root, "alpha", R"({
        "rows": [
            {"id":"r1","persona":{"prefix":"PRESET"},"sections":[{"name":"shared","order":10,"text":"PRESET"}]}
        ]
    })");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));

    const AgentContext leaf = mount_agent(roster, AgentId{"agent-shadow"}, "alpha");
    const PromptAssembly assembly = env.prompt.assemble(AssembleContext{.scope = leaf.scope});
    EXPECT_EQ(section_text(assembly, "shared"), "PRESET");
    EXPECT_EQ(section_text(assembly, "deployment:persona-prefix"), "PRESET");

    const SessionId sid = make_session(env.sessions, env.workspace.path());
    FakeAgent       parent{AgentId{"agent-shadow"}, sid};
    ChildComposition composition;
    composition.persona = "CHILD";
    AgentContext child;
    child.agent = AgentId{"agent-shadow-child"};
    roster.apply_child_composition(child, parent, composition);

    const PromptAssembly child_assembly =
        env.prompt.assemble(AssembleContext{.scope = child.scope});
    EXPECT_EQ(section_text(child_assembly, "deployment:persona-prefix"), "CHILD");
}

// ---------------------------------------------------------------------------
// The child composition (42 §3.3, 42-I5/I7, 42-F3/F15)
// ---------------------------------------------------------------------------

TEST(AgentPresetChild, JoinsParentCompositionAndFixesScope) {
    RosterEnv env("preset_child_join");
    env.prompt.set_tool_provider([](const AssembleContext&) {
        return std::vector<ToolSchema>{tool_schema("read"), tool_schema("shell"),
                                       tool_schema("grep")};
    });
    write_preset(env.presets_root, "alpha", R"({
        "rows": [
            {"id":"r1","persona":{"prefix":"PRESET"},"tools":{"allow":["read","shell"]},
             "sections":[{"name":"alpha:section","order":10,"text":"A"}]}
        ]
    })");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));

    const SessionId sid = make_session(env.sessions, env.workspace.path());
    const AgentContext parent = mount_agent(roster, AgentId{"parent-agent"}, "alpha");
    FakeAgent           parent_agent{AgentId{"parent-agent"}, sid};
    (void)parent;

    ChildComposition composition;
    composition.persona     = "CHILD";
    composition.tool_filter = ToolRestriction{.allow = {"read", "shell", "grep"}, .deny = {}};
    AgentContext child;
    child.agent = AgentId{"child-agent"};
    roster.apply_child_composition(child, parent_agent, composition);

    EXPECT_EQ(roster.composed_preset(roster.leaf_for(AgentId{"child-agent"})), "alpha");
    EXPECT_EQ(roster.leaf_for(AgentId{"child-agent"}).scope, child.scope);

    const PromptAssembly assembly = env.prompt.assemble(AssembleContext{.scope = child.scope});
    EXPECT_EQ(section_text(assembly, "alpha:section"), "A");
    EXPECT_EQ(section_text(assembly, "deployment:persona-prefix"), "CHILD");
    EXPECT_EQ(context_names(assembly), (std::vector<std::string>{"subagent:delegation"}));
    EXPECT_EQ(context_text(assembly, "subagent:delegation"),
              std::string{kDelegationScopeStatement});
    EXPECT_EQ(tool_names(assembly), (std::vector<std::string>{"read", "shell"}));
}

TEST(AgentPresetChild, ToolFilterIntersectionDenyWins) {
    RosterEnv env("preset_child_filter");
    env.prompt.set_tool_provider([](const AssembleContext&) {
        return std::vector<ToolSchema>{tool_schema("read"), tool_schema("shell"),
                                       tool_schema("grep")};
    });
    write_preset(env.presets_root, "alpha", R"({
        "rows": [
            {"id":"r1","tools":{"allow":["read","shell"]}},
            {"id":"r2","tools":{"deny":["shell"]}}
        ]
    })");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));

    const SessionId sid = make_session(env.sessions, env.workspace.path());
    mount_agent(roster, AgentId{"parent-agent"}, "alpha");
    FakeAgent parent_agent{AgentId{"parent-agent"}, sid};

    ChildComposition composition;
    composition.tool_filter = ToolRestriction{.allow = {"read", "shell", "grep"}, .deny = {}};
    AgentContext child;
    child.agent = AgentId{"child-agent"};
    roster.apply_child_composition(child, parent_agent, composition);

    const PromptAssembly assembly = env.prompt.assemble(AssembleContext{.scope = child.scope});
    EXPECT_EQ(tool_names(assembly), (std::vector<std::string>{"read"}));
}

TEST(AgentPresetChild, UnmountedParentThrowsAndRegistersNoChild) {
    RosterEnv env("preset_child_unmounted");
    write_preset(env.presets_root, "alpha", "{}");
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions,
                             root_config(env.presets_root));

    const SessionId sid = make_session(env.sessions, env.workspace.path());
    FakeAgent       parent_agent{AgentId{"never-mounted"}, sid};
    AgentContext    child;
    child.agent = AgentId{"child-agent"};

    EXPECT_THROW(roster.apply_child_composition(child, parent_agent, ChildComposition{}),
                 UnknownAgent);
    EXPECT_THROW((void)roster.leaf_for(AgentId{"child-agent"}), UnknownAgent);
}

// ---------------------------------------------------------------------------
// Depth and the blank-session switch over scopes (42 §3.4, 42-I6, 42-F6)
// ---------------------------------------------------------------------------

TEST(AgentPresetDepth, ChildDepthIsParentPlusOneAndMaxDepthRefuses) {
    EXPECT_FALSE(check_delegation_depth(0, 3).has_value());
    EXPECT_FALSE(check_delegation_depth(2, 3).has_value());

    const std::optional<AgentError> at_limit = check_delegation_depth(3, 3);
    ASSERT_TRUE(at_limit.has_value());
    EXPECT_EQ(at_limit->code, AgentErrorCode::DelegationDepthExceeded);

    const std::optional<AgentError> zero = check_delegation_depth(0, 0);
    ASSERT_TRUE(zero.has_value());
    EXPECT_EQ(zero->code, AgentErrorCode::DelegationDepthExceeded);
}

TEST(AgentPresetSelect, SwitchRewiresTheStandingScope) {
    RosterEnv env("preset_select_rewire");
    write_preset(env.presets_root, "alpha", R"({
        "rows": [{"id":"r1","sections":[{"name":"alpha:section","order":10,"text":"A"}]}]
    })");
    write_preset(env.presets_root, "beta", R"({
        "rows": [{"id":"r1","sections":[{"name":"beta:section","order":10,"text":"B"}]}]
    })");
    PresetConfig config = root_config(env.presets_root);
    config.default_id   = "alpha";
    AgentPresetRoster roster(env.prompt, env.tools, env.skills, env.sessions, config);

    const SessionId sid = make_session(env.sessions, env.workspace.path());
    AgentContext    leaf = mount_agent(roster, AgentId{"agent-rewire"}, "alpha");
    FakeAgent       agent{AgentId{"agent-rewire"}, sid};

    EXPECT_EQ(section_names(env.prompt.assemble(AssembleContext{.scope = leaf.scope})),
              (std::vector<std::string>{"alpha:section"}));

    roster.select(agent, "beta");

    const AgentContext live = roster.leaf_for(AgentId{"agent-rewire"});
    EXPECT_EQ(roster.composed_preset(live), "beta");
    EXPECT_EQ(section_names(env.prompt.assemble(AssembleContext{.scope = live.scope})),
              (std::vector<std::string>{"beta:section"}));
}

} // namespace
