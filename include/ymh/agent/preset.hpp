#pragma once

// Agent presets (42-agent-presets.md). This slice ships the roster, the
// `preset.jsonc` loader, and the blank-session-only `select` path. The standing
// mount, the joined scope chain, and the child composition are later slices;
// `compose_from`/`apply_child_composition` are declared but not yet defined.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "ymh/agent/agent.hpp"
#include "ymh/prompt/persona.hpp"

namespace ymh {

class Session;
class SessionManager;
class ScopeHandle;
class SystemPrompt;
class ToolRegistry;
class SkillCatalog;
class Logger;

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

// 52 review (3B): a row's instruction-loader override, mirroring dsh's
// `@deepseek-ai/dsh-agent-instructions` row config (`maxBytes`).
struct PresetInstructions {
    bool                       enabled = true;
    std::optional<std::size_t> max_bytes;
};

// 42-D17: one preset composition row (42 §2.1.1 pins the file vocabulary).
// 52-D10 adds `persona` (reusing PersonaConfig verbatim; PersonaRow was deleted
// in Rev 2), `permission_preset`, `model`, and `capabilities` (§5.7).
struct PresetRow {
    std::string                    id;
    std::string                    group;
    bool                           disabled = false;
    std::optional<PersonaConfig>   persona;
    std::optional<std::string>     persona_prefix;  // legacy (42), still honored
    std::optional<std::string>     persona_suffix;  // legacy (42), still honored
    std::optional<ToolRestriction> tool_filter;
    std::vector<std::string>       skill_roots;
    std::vector<PromptSectionSpec> sections;
    std::optional<std::string>     permission_preset;
    std::optional<std::string>     model;  // reserved (52-OQ-6)
    std::vector<std::string>       capabilities;
    std::optional<PresetInstructions> instructions;
    std::optional<std::string>     config;
};

// 52 §5.7: the in/out agent capability list. `In` capabilities are delivered;
// `InNotPresetControlled` exist in ymh but are host-global and not selected by a
// preset in v1; `Out` capabilities are not shipped, so a preset that names one
// is a load error (52-I13/52-F13).
enum class CapabilityDisposition : std::uint8_t { In, InNotPresetControlled, Out };

struct CapabilitySpec {
    std::string          name;
    CapabilityDisposition disposition = CapabilityDisposition::Out;
};

[[nodiscard]] const std::vector<CapabilitySpec>& shipped_capabilities();
[[nodiscard]] const CapabilitySpec* find_capability(std::string_view name);

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

// 52-D11: a known-but-not-shipped preset id (ptc, cordis). `resolve` throws this
// rather than silently yielding an empty composition (52-F11).
class PresetUnavailable : public PresetError {
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

// 42 §3.3 (26 §4.3.8): the child's own shadowing rows. The join is implied by
// `apply_child_composition`; "child with no join" is unrepresentable (26-F9).
struct ChildComposition {
    std::optional<std::string>     persona;
    std::optional<ToolRestriction> tool_filter;
};

// 42 §3.5: the fixed delegation statement, verbatim from the authoritative
// source (dsh-subagent/lib/index.js:519, quoted at 26-dsh-alignment.md:1099-1101).
inline constexpr std::string_view kDelegationScopeStatement =
    "You are a delegated subagent: your permission scope was fixed when you were "
    "started and cannot be widened from inside this session \u2014 operations that "
    "require approval are rejected automatically. When the task needs access beyond "
    "that scope, do not retry the denied operation; state the limitation in your "
    "reply so the delegating agent can handle it.";

// 42 §3.5 / 36 §2.2: the canonical name for the leaf-scoped delegation context.
inline constexpr std::string_view kDelegationContextName = "subagent:delegation";

// 42 §3.4 (42-I6, 42-D8, 42-D18): the delegation tool's pre-flight. Returns
// nullopt when delegation is allowed; a value refuses it. `max_depth == 0`
// forbids delegation entirely. Depth is `parent_depth + 1`; a fork inherits.
[[nodiscard]] std::optional<AgentError> check_delegation_depth(std::uint32_t parent_depth,
                                                               std::uint32_t max_depth);

class AgentPresetRoster {
public:
    // 52-D13/52-F16: `logger` (optional) receives the one shipped-root warning.
    // 52-F17: `known_permission_presets` are the names a row's `permission_preset`
    // may reference; an unknown name fails the load with a `ConfigError`.
    AgentPresetRoster(SystemPrompt& prompt, ToolRegistry& tools, SkillCatalog& skills,
                      SessionManager& sessions, PresetConfig config,
                      Logger* logger = nullptr,
                      std::vector<std::string> known_permission_presets = {});
    ~AgentPresetRoster();

    AgentPresetRoster(const AgentPresetRoster&) = delete;
    AgentPresetRoster& operator=(const AgentPresetRoster&) = delete;

    // Discovery + resolution. `resolve(nullopt)` returns the configured default.
    [[nodiscard]] std::vector<AgentPreset> list() const;
    [[nodiscard]] const AgentPreset&       resolve(std::optional<std::string> id) const;

    [[nodiscard]] ScopeKey standing_key_for(std::optional<std::string> id) const;

    // 52-D15: the permission preset a preset binds, folded from its enabled rows
    // (last row wins). Returns nullopt for an absent/unknown preset or one with
    // no binding; never throws.
    [[nodiscard]] std::optional<std::string> permission_preset_for(
        const std::optional<std::string>& id) const;

    // 52 review (3B): the instruction-loader override a preset binds, folded
    // from its enabled rows (last row wins). Nullopt when the preset is absent,
    // unknown, or declares no `instructions` row; never throws.
    [[nodiscard]] std::optional<PresetInstructions> instructions_for(
        const std::optional<std::string>& id) const;

    // 42 §3.2/§2.2: mount the preset once (idempotent), register its rows into
    // the standing scope, then join the agent's leaf to that standing mount and
    // record it. A second call for the same id registers nothing twice (42-I1).
    void mount(AgentContext& ctx, std::optional<std::string> id);

    // 42 §3.3: parent the child leaf to the parent's live chain and record it;
    // returns the preset id for the session header. Reads the parent's live leaf,
    // never a header-derived scope (42-F10).
    [[nodiscard]] std::string compose_from(AgentContext& child, AgentContext& parent);

    [[nodiscard]] std::string composed_preset(const AgentContext& ctx) const;

    [[nodiscard]] const PresetConfig& config() const noexcept;

    [[nodiscard]] AgentContext leaf_for(AgentId agent) const;

    // 52 review (3A): the mounted leaf scope for `agent`, or nullopt when the
    // agent has no mounted preset. Non-throwing, so the loop can call it on the
    // assembly path without an exception for the common unmounted case.
    [[nodiscard]] std::optional<ScopeKey> scope_for(AgentId agent) const noexcept;

    // Blank-session-only switch; appends `agent_preset/selected` after commit.
    void select(Agent& agent, const std::string& preset);

    // 42 §3.3: one call — join the parent's live chain, then apply the child's
    // delegation statement, persona shadow, and narrowing tool filter. A child
    // composed without the join is unrepresentable (42-I5, 26-F9). Throws
    // UnknownAgent when the parent is unmounted (42-F15).
    void apply_child_composition(AgentContext& child, Agent& parent,
                                 const ChildComposition& composition);

    [[nodiscard]] static bool is_blank(const Session& session);

private:
    void                                        ensure_loaded() const;
    [[nodiscard]] std::vector<std::filesystem::path> roots() const;

    // Create the standing mount for `preset` on first use (42-I1) and register
    // its rows into it.
    void ensure_standing(const AgentPreset& preset);
    void register_preset_rows(const AgentPreset& preset, ScopeHandle& scope);

    // The standing mount a live leaf is parented to; empty when the leaf has no
    // preset ancestor (the global layer only).
    [[nodiscard]] ScopeKey standing_for_leaf(const ScopeKey& leaf) const;

    // The discovered presets (the pinned `resolve` returns a reference, so the
    // roster must own them).
    mutable std::vector<AgentPreset>              presets_;
    mutable bool                                  loaded_ = false;
    std::unordered_map<std::string, ScopeKey>     standing_;
    std::unordered_map<std::string, AgentContext> leaves_;
    // The RAII scope handles the standing mounts and per-session leaves live
    // under; destroying one unwinds its registrations (42 §3.2).
    std::unordered_map<std::string, std::unique_ptr<ScopeHandle>> standing_handles_;
    std::unordered_map<std::string, std::unique_ptr<ScopeHandle>> leaf_handles_;
    SystemPrompt&                                 prompt_;
    ToolRegistry&                                 tools_;
    SkillCatalog&                                 skills_;
    SessionManager&                               sessions_;
    PresetConfig                                  config_;
    Logger*                                       logger_ = nullptr;
    std::vector<std::string>                      known_permission_presets_;
};

// `$XDG_CONFIG_HOME/ymh/presets` (else `$HOME/.config/ymh/presets`).
[[nodiscard]] std::filesystem::path default_presets_user_root();

// The packaged shipped root; empty in a source build.
[[nodiscard]] std::filesystem::path default_presets_shipped_root();

// 52-D11/M-P1: the compiled-in reserved ids (`ptc`, `cordis`). They are not
// discovered, so `list()` never returns them; `resolve` throws
// `PresetUnavailable` for an undiscovered reserved id (52-I8/52-F11).
[[nodiscard]] std::vector<std::string> reserved_ids();

} // namespace ymh
