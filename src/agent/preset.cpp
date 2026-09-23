#include "ymh/agent/preset.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "ymh/config/config.hpp"
#include "ymh/core/logger.hpp"
#include "ymh/prompt/order.hpp"
#include "ymh/prompt/runtime_context.hpp"
#include "ymh/prompt/system_prompt.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"

namespace ymh {
namespace {

using Json = nlohmann::json;

[[noreturn]] void load_fail(const std::filesystem::path& file, const std::string& detail) {
    throw PresetLoadError("preset " + file.string() + ": " + detail);
}

const Json* member(const Json& object, std::string_view key) {
    const auto it = object.find(std::string{key});
    return it == object.end() ? nullptr : &(*it);
}

std::string qualified(std::string_view context, const std::string& key) {
    return context.empty() ? key : std::string{context} + "." + key;
}

void reject_unknown(const Json& object, const std::filesystem::path& file,
                    std::string_view context, std::initializer_list<std::string_view> allowed) {
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const std::string_view candidate : allowed) {
            if (candidate == it.key()) {
                known = true;
                break;
            }
        }
        if (!known) {
            load_fail(file, "unknown key '" + qualified(context, it.key()) + "'");
        }
    }
}

std::vector<std::string> read_string_array(const Json& value, const std::filesystem::path& file,
                                           const std::string& label) {
    if (!value.is_array()) {
        load_fail(file, "invalid type for '" + label + "'");
    }
    std::vector<std::string> out;
    out.reserve(value.size());
    for (const Json& item : value) {
        if (!item.is_string()) {
            load_fail(file, "invalid type for '" + label + "'");
        }
        out.push_back(item.get<std::string>());
    }
    return out;
}

void reject_escaping_skill_root(const AgentPreset& preset, const std::string& root,
                                const std::filesystem::path& file) {
    const std::filesystem::path raw{root};
    const std::filesystem::path base = preset.source_path.lexically_normal();
    const std::filesystem::path full = (base / raw).lexically_normal();
    if (raw.is_absolute() || full.lexically_relative(base).generic_string().rfind("..", 0) == 0) {
        load_fail(file, "skill root '" + root + "' escapes the preset directory");
    }
}

AgentPreset parse_preset(const std::filesystem::path& dir) {
    const std::filesystem::path file = dir / "preset.jsonc";
    std::ifstream               input(file, std::ios::binary);
    if (!input) {
        load_fail(file, "cannot open");
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();

    Json document;
    try {
        document = Json::parse(buffer.str(), nullptr, /*allow_exceptions=*/true,
                               /*ignore_comments=*/true);
    } catch (const Json::exception& error) {
        load_fail(file, std::string{"parse error: "} + error.what());
    }
    if (!document.is_object()) {
        load_fail(file, "top-level value must be an object");
    }
    reject_unknown(document, file, "", {"id", "display_name", "rows"});

    const std::string dir_name = dir.filename().string();
    AgentPreset       preset;
    preset.id          = dir_name;
    preset.source_path = dir;
    if (const Json* id = member(document, "id")) {
        if (!id->is_string()) {
            load_fail(file, "invalid type for 'id'");
        }
        const std::string value = id->get<std::string>();
        if (value != dir_name) {
            load_fail(file, "id '" + value + "' does not match directory '" + dir_name + "'");
        }
    }
    preset.display_name = preset.id;
    if (const Json* name = member(document, "display_name")) {
        if (!name->is_string()) {
            load_fail(file, "invalid type for 'display_name'");
        }
        preset.display_name = name->get<std::string>();
    }

    const Json* rows = member(document, "rows");
    if (rows == nullptr) {
        return preset;
    }
    if (!rows->is_array()) {
        load_fail(file, "invalid type for 'rows'");
    }

    std::set<std::string> row_ids;
    std::set<std::string> section_names;
    bool                  persona_prefix_seen = false;
    bool                  persona_suffix_seen = false;
    for (std::size_t index = 0; index < rows->size(); ++index) {
        const Json&       row   = (*rows)[index];
        const std::string label = "rows[" + std::to_string(index) + "]";
        if (!row.is_object()) {
            load_fail(file, "invalid type for '" + label + "'");
        }
        reject_unknown(row, file, label,
                       {"id", "group", "disabled", "persona", "persona_prefix", "persona_suffix",
                        "tools", "skills", "sections", "permission_preset", "model",
                        "capabilities", "config"});

        const Json* id = member(row, "id");
        if (id == nullptr || !id->is_string() || id->get<std::string>().empty()) {
            load_fail(file, "missing or empty '" + label + ".id'");
        }
        const std::string row_id = id->get<std::string>();
        if (!row_ids.insert(row_id).second) {
            load_fail(file, "duplicate '" + label + ".id' '" + row_id + "'");
        }

        PresetRow parsed;
        parsed.id = row_id;
        if (const Json* group = member(row, "group")) {
            if (!group->is_string()) {
                load_fail(file, "invalid type for '" + label + ".group'");
            }
            parsed.group = group->get<std::string>();
        }
        if (const Json* disabled = member(row, "disabled")) {
            if (!disabled->is_boolean()) {
                load_fail(file, "invalid type for '" + label + ".disabled'");
            }
            parsed.disabled = disabled->get<bool>();
        }
        if (const Json* persona = member(row, "persona")) {
            if (!persona->is_object()) {
                load_fail(file, "invalid type for '" + label + ".persona'");
            }
            reject_unknown(*persona, file, label + ".persona",
                           {"prefix", "suffix", "complete", "include_runtime_context"});
            PersonaConfig config;
            if (const Json* prefix = member(*persona, "prefix")) {
                if (!prefix->is_string()) {
                    load_fail(file, "invalid type for '" + label + ".persona.prefix'");
                }
                config.prefix = prefix->get<std::string>();
            }
            if (const Json* suffix = member(*persona, "suffix")) {
                if (!suffix->is_string()) {
                    load_fail(file, "invalid type for '" + label + ".persona.suffix'");
                }
                config.suffix = suffix->get<std::string>();
            }
            if (const Json* complete = member(*persona, "complete")) {
                if (!complete->is_boolean()) {
                    load_fail(file, "invalid type for '" + label + ".persona.complete'");
                }
                config.complete = complete->get<bool>();
            }
            if (const Json* include = member(*persona, "include_runtime_context")) {
                if (!include->is_boolean()) {
                    load_fail(file, "invalid type for '" +
                                        label + ".persona.include_runtime_context'");
                }
                config.include_runtime_context = include->get<bool>();
            }
            parsed.persona = std::move(config);
        }
        if (const Json* prefix = member(row, "persona_prefix")) {
            if (!prefix->is_string()) {
                load_fail(file, "invalid type for '" + label + ".persona_prefix'");
            }
            parsed.persona_prefix = prefix->get<std::string>();
        }
        if (const Json* suffix = member(row, "persona_suffix")) {
            if (!suffix->is_string()) {
                load_fail(file, "invalid type for '" + label + ".persona_suffix'");
            }
            parsed.persona_suffix = suffix->get<std::string>();
        }
        if (parsed.persona.has_value() &&
            (parsed.persona_prefix.has_value() || parsed.persona_suffix.has_value())) {
            load_fail(file, "row '" + row_id +
                                "' sets 'persona' together with 'persona_prefix'/'persona_suffix' "
                                "(52-F15)");
        }
        if (!parsed.disabled) {
            const bool defines_prefix =
                (parsed.persona.has_value() && !parsed.persona->prefix.empty()) ||
                (parsed.persona_prefix.has_value() && !parsed.persona_prefix->empty());
            const bool defines_suffix =
                (parsed.persona.has_value() && !parsed.persona->suffix.empty()) ||
                (parsed.persona_suffix.has_value() && !parsed.persona_suffix->empty());
            if (defines_prefix) {
                if (persona_prefix_seen) {
                    load_fail(file, "duplicate 'deployment:persona-prefix' across rows (52-F15)");
                }
                persona_prefix_seen = true;
            }
            if (defines_suffix) {
                if (persona_suffix_seen) {
                    load_fail(file, "duplicate 'deployment:persona-suffix' across rows (52-F15)");
                }
                persona_suffix_seen = true;
            }
        }
        if (const Json* tools = member(row, "tools")) {
            if (!tools->is_object()) {
                load_fail(file, "invalid type for '" + label + ".tools'");
            }
            reject_unknown(*tools, file, label + ".tools", {"allow", "deny"});
            ToolRestriction restriction;
            if (const Json* allow = member(*tools, "allow")) {
                restriction.allow = read_string_array(*allow, file, label + ".tools.allow");
            }
            if (const Json* deny = member(*tools, "deny")) {
                restriction.deny = read_string_array(*deny, file, label + ".tools.deny");
            }
            parsed.tool_filter = std::move(restriction);
        }
        if (const Json* skills = member(row, "skills")) {
            parsed.skill_roots = read_string_array(*skills, file, label + ".skills");
        }
        if (const Json* sections = member(row, "sections")) {
            if (!sections->is_array()) {
                load_fail(file, "invalid type for '" + label + ".sections'");
            }
            for (std::size_t spec_index = 0; spec_index < sections->size(); ++spec_index) {
                const Json&       spec       = (*sections)[spec_index];
                const std::string spec_label = label + ".sections[" + std::to_string(spec_index) + "]";
                if (!spec.is_object()) {
                    load_fail(file, "invalid type for '" + spec_label + "'");
                }
                reject_unknown(spec, file, spec_label, {"name", "order", "complete", "text"});
                const Json* name = member(spec, "name");
                if (name == nullptr || !name->is_string() || name->get<std::string>().empty()) {
                    load_fail(file, "missing or empty '" + spec_label + ".name'");
                }
                const std::string section_name = name->get<std::string>();
                if (!section_names.insert(section_name).second) {
                    load_fail(file, "duplicate section name '" + section_name + "'");
                }
                PromptSectionSpec parsed_section;
                parsed_section.name = section_name;
                if (const Json* order = member(spec, "order")) {
                    if (!order->is_number_integer()) {
                        load_fail(file, "invalid type for '" + spec_label + ".order'");
                    }
                    const std::int64_t value = order->get<std::int64_t>();
                    if (value < std::numeric_limits<std::int32_t>::min() ||
                        value > std::numeric_limits<std::int32_t>::max()) {
                        load_fail(file, "value out of range for '" + spec_label + ".order'");
                    }
                    parsed_section.order = static_cast<std::int32_t>(value);
                }
                if (const Json* complete = member(spec, "complete")) {
                    if (!complete->is_boolean()) {
                        load_fail(file, "invalid type for '" + spec_label + ".complete'");
                    }
                    parsed_section.complete = complete->get<bool>();
                }
                if (const Json* text = member(spec, "text")) {
                    if (!text->is_string()) {
                        load_fail(file, "invalid type for '" + spec_label + ".text'");
                    }
                    parsed_section.text = text->get<std::string>();
                }
                parsed.sections.push_back(std::move(parsed_section));
            }
        }
        if (const Json* permission = member(row, "permission_preset")) {
            if (!permission->is_string() || permission->get<std::string>().empty()) {
                load_fail(file, "invalid type for '" + label + ".permission_preset'");
            }
            parsed.permission_preset = permission->get<std::string>();
        }
        if (const Json* model = member(row, "model")) {
            if (!model->is_string()) {
                load_fail(file, "invalid type for '" + label + ".model'");
            }
            parsed.model = model->get<std::string>();
        }
        if (const Json* capabilities = member(row, "capabilities")) {
            parsed.capabilities = read_string_array(*capabilities, file, label + ".capabilities");
            for (const std::string& capability : parsed.capabilities) {
                const CapabilitySpec* spec = find_capability(capability);
                if (spec == nullptr) {
                    load_fail(file, "row '" + row_id + "' names unknown capability '" + capability +
                                        "' (52-F13)");
                }
                if (spec->disposition == CapabilityDisposition::Out) {
                    load_fail(file, "row '" + row_id + "' names out-of-scope capability '" +
                                        capability + "' (52-F13)");
                }
            }
        }
        if (const Json* config = member(row, "config")) {
            parsed.config = config->dump();
        }
        preset.rows.push_back(std::move(parsed));
    }
    return preset;
}

bool names(const std::vector<std::string>& list, const std::string& name) {
    return std::find(list.begin(), list.end(), name) != list.end();
}

// 42 §2.1/§3.3: deny wins over allow; an empty allow admits every not-denied
// name. Composing two restrictions this way yields their intersection, so a
// child can only narrow (42-I7, 26-I8).
ToolRestriction intersect_restrictions(const ToolRestriction& outer, const ToolRestriction& inner) {
    ToolRestriction merged;
    if (outer.allow.empty()) {
        merged.allow = inner.allow;
    } else if (inner.allow.empty()) {
        merged.allow = outer.allow;
    } else {
        for (const std::string& name : outer.allow) {
            if (names(inner.allow, name)) {
                merged.allow.push_back(name);
            }
        }
    }
    merged.deny = outer.deny;
    for (const std::string& name : inner.deny) {
        if (!names(merged.deny, name)) {
            merged.deny.push_back(name);
        }
    }
    return merged;
}

std::function<std::vector<ToolSchema>(std::vector<ToolSchema>)> make_tool_filter(
    ToolRestriction restriction) {
    return [restriction = std::move(restriction)](std::vector<ToolSchema> tools) {
        std::vector<ToolSchema> kept;
        kept.reserve(tools.size());
        for (ToolSchema& schema : tools) {
            const std::string& name    = schema.name.value;
            const bool         allowed = restriction.allow.empty() || names(restriction.allow, name);
            const bool         denied  = names(restriction.deny, name);
            if (allowed && !denied) {
                kept.push_back(std::move(schema));
            }
        }
        return kept;
    };
}

} // namespace

std::filesystem::path default_presets_user_root() {
    return default_global_config_path().parent_path() / "presets";
}

std::filesystem::path default_presets_shipped_root() {
    if (const char* env = std::getenv("YMH_PRESETS_DIR"); env != nullptr) {
        std::filesystem::path candidate{env};
        std::error_code       error;
        if (candidate.empty() || !std::filesystem::is_directory(candidate, error)) {
            return {};
        }
        return candidate;
    }
    std::error_code error;
#ifdef YMH_PRESETS_DIR
    {
        std::filesystem::path candidate{YMH_PRESETS_DIR};
        if (!candidate.empty() && std::filesystem::is_directory(candidate, error)) {
            return candidate;
        }
    }
#endif
#ifdef YMH_SOURCE_DIR
    {
        std::filesystem::path candidate = std::filesystem::path{YMH_SOURCE_DIR} / "presets";
        if (std::filesystem::is_directory(candidate, error)) {
            return candidate;
        }
    }
#endif
    return {};
}

std::vector<std::string> reserved_ids() {
    return {"ptc", "cordis"};
}

const std::vector<CapabilitySpec>& shipped_capabilities() {
    static const std::vector<CapabilitySpec> capabilities = {
        {"subagents", CapabilityDisposition::In},
        {"skills", CapabilityDisposition::In},
        {"plan-mode", CapabilityDisposition::InNotPresetControlled},
        {"compaction", CapabilityDisposition::InNotPresetControlled},
        {"goals", CapabilityDisposition::InNotPresetControlled},
        {"jobs", CapabilityDisposition::InNotPresetControlled},
        {"commands", CapabilityDisposition::InNotPresetControlled},
        {"workflow", CapabilityDisposition::Out},
        {"ralph", CapabilityDisposition::Out},
        {"ptc", CapabilityDisposition::Out},
        {"run-code", CapabilityDisposition::Out},
        {"plugin", CapabilityDisposition::Out},
        {"cordis", CapabilityDisposition::Out},
        {"remote", CapabilityDisposition::Out},
        {"acp", CapabilityDisposition::Out},
        {"image", CapabilityDisposition::Out},
        {"model-selection", CapabilityDisposition::Out},
    };
    return capabilities;
}

const CapabilitySpec* find_capability(std::string_view name) {
    for (const CapabilitySpec& spec : shipped_capabilities()) {
        if (spec.name == name) {
            return &spec;
        }
    }
    return nullptr;
}

AgentPresetRoster::AgentPresetRoster(SystemPrompt& prompt, ToolRegistry& tools,
                                     SkillCatalog& skills, SessionManager& sessions,
                                     PresetConfig config, Logger* logger,
                                     std::vector<std::string> known_permission_presets)
    : prompt_(prompt),
      tools_(tools),
      skills_(skills),
      sessions_(sessions),
      config_(std::move(config)),
      logger_(logger),
      known_permission_presets_(std::move(known_permission_presets)) {}

std::vector<std::filesystem::path> AgentPresetRoster::roots() const {
    std::vector<std::filesystem::path> result;
    if (config_.include_shipped_root) {
        std::filesystem::path shipped = default_presets_shipped_root();
        if (shipped.empty()) {
            if (logger_ != nullptr) {
                logger_->warn("presets: shipped root is empty; the shipped "
                              "minimal/standard presets are unavailable (52-F16)");
            }
        } else {
            result.push_back(std::move(shipped));
        }
    }
    if (config_.include_user_root) {
        result.push_back(default_presets_user_root());
    }
    if (config_.root.has_value()) {
        result.push_back(*config_.root);
    }
    return result;
}

void AgentPresetRoster::ensure_loaded() const {
    if (loaded_) {
        return;
    }
    std::vector<AgentPreset> discovered;
    std::set<std::string>    seen;
    for (const std::filesystem::path& root : roots()) {
        std::error_code error;
        if (root.empty() || !std::filesystem::is_directory(root, error)) {
            continue;
        }
        std::vector<std::filesystem::path> directories;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(root, error)) {
            if (entry.is_directory(error)) {
                directories.push_back(entry.path());
            }
        }
        if (error) {
            continue;
        }
        std::sort(directories.begin(), directories.end());
        for (const std::filesystem::path& dir : directories) {
            const std::filesystem::path file = dir / "preset.jsonc";
            if (!std::filesystem::is_regular_file(file, error)) {
                continue;
            }
            AgentPreset preset = parse_preset(dir);
            for (const PresetRow& row : preset.rows) {
                for (const std::string& skill_root : row.skill_roots) {
                    reject_escaping_skill_root(preset, skill_root, file);
                }
                if (row.permission_preset.has_value() &&
                    std::find(known_permission_presets_.begin(), known_permission_presets_.end(),
                              *row.permission_preset) == known_permission_presets_.end()) {
                    throw ConfigError("preset " + file.string() + ": unknown permission preset '" +
                                      *row.permission_preset + "' (52-F17)");
                }
            }
            if (!seen.insert(preset.id).second) {
                throw PresetLoadError("duplicate preset id '" + preset.id + "' across roots");
            }
            discovered.push_back(std::move(preset));
        }
    }
    presets_ = std::move(discovered);
    loaded_  = true;
}

std::vector<AgentPreset> AgentPresetRoster::list() const {
    ensure_loaded();
    return presets_;
}

const AgentPreset& AgentPresetRoster::resolve(std::optional<std::string> id) const {
    ensure_loaded();
    std::optional<std::string> target = std::move(id);
    if (!target.has_value()) {
        if (!config_.default_id.has_value()) {
            throw PresetNotFound("no preset id given and no configured default");
        }
        target = config_.default_id;
    }
    for (const AgentPreset& preset : presets_) {
        if (preset.id == *target) {
            return preset;
        }
    }
    const std::vector<std::string> reserved = reserved_ids();
    if (std::find(reserved.begin(), reserved.end(), *target) != reserved.end()) {
        throw PresetUnavailable("preset '" + *target +
                                "' is reserved and not shipped by this build");
    }
    throw PresetNotFound("unknown preset '" + *target + "'");
}

ScopeKey AgentPresetRoster::standing_key_for(std::optional<std::string> id) const {
    return "preset:" + resolve(std::move(id)).id;
}

std::optional<std::string> AgentPresetRoster::permission_preset_for(
    const std::optional<std::string>& id) const {
    if (!id.has_value() || id->empty()) {
        return std::nullopt;
    }
    try {
        std::optional<std::string> binding;
        for (const PresetRow& row : resolve(*id).rows) {
            if (row.disabled) {
                continue;
            }
            if (row.permission_preset.has_value()) {
                binding = row.permission_preset;
            }
        }
        return binding;
    } catch (const PresetError&) {
        return std::nullopt;
    } catch (const ConfigError&) {
        return std::nullopt;
    }
}

AgentPresetRoster::~AgentPresetRoster() = default;

void AgentPresetRoster::ensure_standing(const AgentPreset& preset) {
    if (standing_.count(preset.id) != 0) {
        return;
    }
    const ScopeKey standing = "preset:" + preset.id;
    auto           handle   = std::make_unique<ScopeHandle>(prompt_.scope(ScopeKey{}, standing));
    register_preset_rows(preset, *handle);
    standing_handles_.emplace(preset.id, std::move(handle));
    standing_.emplace(preset.id, standing);
}

void AgentPresetRoster::register_preset_rows(const AgentPreset& preset, ScopeHandle& scope) {
    std::optional<ToolRestriction> filter;
    bool                           persona_prefix = false;
    bool                           persona_suffix = false;
    bool                           suppress_runtime_context = false;
    for (const PresetRow& row : preset.rows) {
        if (row.disabled) {
            continue;
        }
        for (const PromptSectionSpec& spec : row.sections) {
            PromptSection section;
            section.name     = spec.name;
            section.order    = spec.order;
            section.complete = spec.complete;
            section.text     = [text = spec.text](const AssembleContext&) { return text; };
            scope.add_section(std::move(section));
        }
        if (row.persona.has_value()) {
            if (!row.persona->include_runtime_context) {
                suppress_runtime_context = true;
            }
            if (!row.persona->prefix.empty() && !persona_prefix) {
                PromptSection section;
                section.name     = "deployment:persona-prefix";
                section.order    = section_order("deployment:persona-prefix");
                section.complete = row.persona->complete;
                section.text =
                    [text = row.persona->prefix](const AssembleContext&) { return text; };
                scope.add_section(std::move(section));
                persona_prefix = true;
            }
            if (!row.persona->suffix.empty() && !persona_suffix) {
                PromptSection section;
                section.name  = "deployment:persona-suffix";
                section.order = section_order("deployment:persona-suffix");
                section.text =
                    [text = row.persona->suffix](const AssembleContext&) { return text; };
                scope.add_section(std::move(section));
                persona_suffix = true;
            }
        }
        if (row.persona_prefix.has_value() && !persona_prefix) {
            PromptSection section;
            section.name  = "deployment:persona-prefix";
            section.order = section_order("deployment:persona-prefix");
            section.text =
                [text = *row.persona_prefix](const AssembleContext&) { return text; };
            scope.add_section(std::move(section));
            persona_prefix = true;
        }
        if (row.persona_suffix.has_value() && !persona_suffix) {
            PromptSection section;
            section.name  = "deployment:persona-suffix";
            section.order = section_order("deployment:persona-suffix");
            section.text =
                [text = *row.persona_suffix](const AssembleContext&) { return text; };
            scope.add_section(std::move(section));
            persona_suffix = true;
        }
        if (row.tool_filter.has_value()) {
            filter = filter.has_value() ? intersect_restrictions(*filter, *row.tool_filter)
                                        : row.tool_filter;
        }
    }
    if (filter.has_value()) {
        scope.set_tool_filter(make_tool_filter(*filter));
    }
    if (suppress_runtime_context) {
        PromptContext context;
        context.name  = std::string{kRuntimeContextName};
        context.order = 0;
        context.text  = [](const AssembleContext&) { return std::string{}; };
        scope.add_context(std::move(context));
    }
}

void AgentPresetRoster::mount(AgentContext& ctx, std::optional<std::string> id) {
    const AgentPreset& preset = resolve(std::move(id));
    ensure_standing(preset);

    const ScopeKey standing = standing_[preset.id];
    const ScopeKey leaf     = "session:" + ctx.agent.value;
    if (leaf_handles_.count(leaf) == 0) {
        leaf_handles_.emplace(leaf,
                              std::make_unique<ScopeHandle>(prompt_.scope(standing, leaf)));
    }
    ctx.scope                = leaf;
    leaves_[ctx.agent.value] = ctx;
}

std::string AgentPresetRoster::composed_preset(const AgentContext& ctx) const {
    const auto preset_of = [](const ScopeKey& key) -> std::string {
        constexpr std::string_view kPrefix = "preset:";
        if (key.rfind(kPrefix, 0) == 0) {
            return key.substr(kPrefix.size());
        }
        return {};
    };
    if (const std::string direct = preset_of(ctx.scope); !direct.empty()) {
        return direct;
    }
    if (const std::optional<ScopeKey> parent = prompt_.scope_parent(ctx.scope);
        parent.has_value()) {
        return preset_of(*parent);
    }
    return {};
}

const PresetConfig& AgentPresetRoster::config() const noexcept {
    return config_;
}

ScopeKey AgentPresetRoster::standing_for_leaf(const ScopeKey& leaf) const {
    constexpr std::string_view kPrefix = "preset:";
    if (leaf.rfind(kPrefix, 0) == 0) {
        return leaf;
    }
    if (const std::optional<ScopeKey> parent = prompt_.scope_parent(leaf);
        parent.has_value() && parent->rfind(kPrefix, 0) == 0) {
        return *parent;
    }
    return {};
}

std::string AgentPresetRoster::compose_from(AgentContext& child, AgentContext& parent) {
    const std::string preset_id = composed_preset(parent);
    const ScopeKey    standing  = standing_for_leaf(parent.scope);
    const ScopeKey    leaf      = "session:" + child.agent.value;
    if (leaf_handles_.count(leaf) == 0) {
        leaf_handles_.emplace(leaf,
                              std::make_unique<ScopeHandle>(prompt_.scope(standing, leaf)));
    }
    child.scope                = leaf;
    leaves_[child.agent.value] = child;
    return preset_id;
}

void AgentPresetRoster::apply_child_composition(AgentContext& child, Agent& parent,
                                                const ChildComposition& composition) {
    AgentContext parent_ctx = leaf_for(parent.id());
    (void)compose_from(child, parent_ctx);

    const auto handle = leaf_handles_.find(child.scope);
    if (handle == leaf_handles_.end()) {
        throw UnknownAgent("child leaf was not registered: " + child.scope);
    }
    ScopeHandle& leaf = *handle->second;

    PromptContext statement;
    statement.name  = std::string{kDelegationContextName};
    statement.order = context_order(kDelegationContextName);
    statement.text  = [](const AssembleContext&) {
        return std::string{kDelegationScopeStatement};
    };
    leaf.add_context(std::move(statement));

    if (composition.persona.has_value()) {
        PromptSection persona;
        persona.name  = "deployment:persona-prefix";
        persona.order = section_order("deployment:persona-prefix");
        persona.text  = [text = *composition.persona](const AssembleContext&) { return text; };
        leaf.add_section(std::move(persona));
    }

    if (composition.tool_filter.has_value()) {
        leaf.set_tool_filter(make_tool_filter(*composition.tool_filter));
    }
}

AgentContext AgentPresetRoster::leaf_for(AgentId agent) const {
    const auto it = leaves_.find(agent.value);
    if (it == leaves_.end()) {
        throw UnknownAgent("agent is not mounted: " + agent.value);
    }
    return it->second;
}

bool AgentPresetRoster::is_blank(const Session& session) {
    for (const EventRecord& record : session.events()) {
        switch (record.event.type) {
            case EventType::UserMessage:
            case EventType::AssistantMessage:
            case EventType::ToolCall:
            case EventType::ToolResult:
            case EventType::ContextInjected:
            case EventType::ContextCompaction:
            case EventType::SubagentSpawned:
            case EventType::SubagentFanIn:
            case EventType::LlmRequestHeader:
                return false;
            default:
                break;
        }
    }
    return true;
}

void AgentPresetRoster::select(Agent& agent, const std::string& preset) {
    const AgentPreset&       target  = resolve(preset);
    AgentContext             ctx     = leaf_for(agent.id());
    std::shared_ptr<Session> session = sessions_.sessionPtr(agent.session());
    if (!is_blank(*session)) {
        throw CompositionFixed("session composition is fixed after the first message");
    }

    ensure_standing(target);
    const ScopeKey standing = standing_[target.id];
    const ScopeKey leaf     = ctx.scope;
    leaf_handles_.erase(leaf);
    leaf_handles_.emplace(leaf, std::make_unique<ScopeHandle>(prompt_.scope(standing, leaf)));
    ctx.scope                 = leaf;
    leaves_[agent.id().value] = ctx;
    session->append(payload::AgentPresetSelected{.agent_preset = target.id});
}

std::optional<AgentError> check_delegation_depth(std::uint32_t parent_depth,
                                                 std::uint32_t max_depth) {
    const std::uint32_t child_depth = parent_depth + 1;
    if (child_depth > max_depth) {
        return AgentError{AgentErrorCode::DelegationDepthExceeded,
                          "delegation depth " + std::to_string(child_depth) +
                              " exceeds presets.max_depth " + std::to_string(max_depth)};
    }
    return std::nullopt;
}

} // namespace ymh
