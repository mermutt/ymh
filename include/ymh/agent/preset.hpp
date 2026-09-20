#pragma once

// Agent presets (42-agent-presets.md). This slice ships the roster, the
// `preset.jsonc` loader, and the blank-session-only `select` path. The standing
// mount, the joined scope chain, and the child composition are later slices;
// `compose_from`/`apply_child_composition` are declared but not yet defined.

#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "ymh/agent/agent.hpp"

namespace ymh {

class Session;
class SessionManager;
class SystemPrompt;
class ToolRegistry;
class SkillCatalog;

// 42 §3.1: 26 §4.3.8 shared value types.
using GoalId   = std::uint64_t;  // Wave 6; shared type only
using ScopeKey = std::string;

struct ToolRestriction {
    std::vector<std::string> allow;  // empty allow == all not denied
    std::vector<std::string> deny;   // deny wins over allow
};

struct AgentContext {
    AgentId  agent;
    ScopeKey scope;
};

// 42-D17: the 36 `PromptSection` without its `text` closure; the roster binds
// literal text at mount.
struct PromptSectionSpec {
    std::string  name;
    std::int32_t order    = 0;
    bool         complete = false;
    std::string  text;
};

// 42-D17: one preset composition row (42 §2.1.1 pins the file vocabulary).
struct PresetRow {
    std::string                    id;
    std::string                    group;
    bool                           disabled = false;
    std::optional<std::string>     persona_prefix;
    std::optional<std::string>     persona_suffix;
    std::optional<ToolRestriction> tool_filter;
    std::vector<std::string>       skill_roots;
    std::vector<PromptSectionSpec> sections;
    std::optional<std::string>     config;
};

struct AgentPreset {
    std::string            id;
    std::string            display_name;
    std::filesystem::path  source_path;
    std::vector<PresetRow> rows;
};

// 42-D18: the parsed `presets.*` block.
struct PresetConfig {
    std::optional<std::filesystem::path> root;
    std::optional<std::string>           default_id;
    bool                                 include_shipped_root = true;
    bool                                 include_user_root    = true;
    std::uint32_t                        max_depth            = 3;
};

class PresetError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// 42-F1: an unknown id, or `nullopt` with no configured default.
class PresetNotFound : public PresetError {
public:
    using PresetError::PresetError;
};

// 42-F8: any preset-file schema violation; names the file and key.
class PresetLoadError : public PresetError {
public:
    using PresetError::PresetError;
};

// 42-F4: a `select` on a session that is no longer blank.
class CompositionFixed : public PresetError {
public:
    using PresetError::PresetError;
};

// 42-F15: an agent absent from the roster's leaf map.
class UnknownAgent : public PresetError {
public:
    using PresetError::PresetError;
};

// 42 §3.3: defined with the child-composition slice.
struct ChildComposition;

class AgentPresetRoster {
public:
    AgentPresetRoster(SystemPrompt& prompt, ToolRegistry& tools, SkillCatalog& skills,
                      SessionManager& sessions, PresetConfig config);

    AgentPresetRoster(const AgentPresetRoster&) = delete;
    AgentPresetRoster& operator=(const AgentPresetRoster&) = delete;

    // Discovery + resolution. `resolve(nullopt)` returns the configured default.
    [[nodiscard]] std::vector<AgentPreset> list() const;
    [[nodiscard]] const AgentPreset&       resolve(std::optional<std::string> id) const;

    [[nodiscard]] ScopeKey standing_key_for(std::optional<std::string> id) const;

    // Records the agent's live leaf; the joined scope chain lands with the
    // standing-mount slice.
    void mount(AgentContext& ctx, std::optional<std::string> id);

    // Child composition: later slice.
    [[nodiscard]] std::string compose_from(AgentContext& child, AgentContext& parent);

    [[nodiscard]] std::string composed_preset(const AgentContext& ctx) const;

    [[nodiscard]] const PresetConfig& config() const noexcept;

    [[nodiscard]] AgentContext leaf_for(AgentId agent) const;

    // Blank-session-only switch; appends `agent_preset/selected` after commit.
    void select(Agent& agent, const std::string& preset);

    // Child composition: later slice.
    void apply_child_composition(AgentContext& child, Agent& parent,
                                 const ChildComposition& composition);

    [[nodiscard]] static bool is_blank(const Session& session);

private:
    void                                        ensure_loaded() const;
    [[nodiscard]] std::vector<std::filesystem::path> roots() const;

    // The discovered presets (the pinned `resolve` returns a reference, so the
    // roster must own them).
    mutable std::vector<AgentPreset>              presets_;
    mutable bool                                  loaded_ = false;
    std::unordered_map<std::string, ScopeKey>     standing_;
    std::unordered_map<std::string, AgentContext> leaves_;
    SystemPrompt&                                 prompt_;
    ToolRegistry&                                 tools_;
    SkillCatalog&                                 skills_;
    SessionManager&                               sessions_;
    PresetConfig                                  config_;
};

// `$XDG_CONFIG_HOME/ymh/presets` (else `$HOME/.config/ymh/presets`).
[[nodiscard]] std::filesystem::path default_presets_user_root();

// The packaged shipped root; empty in a source build.
[[nodiscard]] std::filesystem::path default_presets_shipped_root();

} // namespace ymh
