#include "ymh/agent/preset.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "ymh/config/config.hpp"
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
    for (std::size_t index = 0; index < rows->size(); ++index) {
        const Json&       row   = (*rows)[index];
        const std::string label = "rows[" + std::to_string(index) + "]";
        if (!row.is_object()) {
            load_fail(file, "invalid type for '" + label + "'");
        }
        reject_unknown(row, file, label,
                       {"id", "group", "disabled", "persona", "tools", "skills", "sections",
                        "config"});

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
            reject_unknown(*persona, file, label + ".persona", {"prefix", "suffix"});
            if (const Json* prefix = member(*persona, "prefix")) {
                if (!prefix->is_string()) {
                    load_fail(file, "invalid type for '" + label + ".persona.prefix'");
                }
                parsed.persona_prefix = prefix->get<std::string>();
            }
            if (const Json* suffix = member(*persona, "suffix")) {
                if (!suffix->is_string()) {
                    load_fail(file, "invalid type for '" + label + ".persona.suffix'");
                }
                parsed.persona_suffix = suffix->get<std::string>();
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
        if (const Json* config = member(row, "config")) {
            parsed.config = config->dump();
        }
        preset.rows.push_back(std::move(parsed));
    }
    return preset;
}

} // namespace

std::filesystem::path default_presets_user_root() {
    return default_global_config_path().parent_path() / "presets";
}

std::filesystem::path default_presets_shipped_root() {
    return {};
}

AgentPresetRoster::AgentPresetRoster(SystemPrompt& prompt, ToolRegistry& tools,
                                     SkillCatalog& skills, SessionManager& sessions,
                                     PresetConfig config)
    : prompt_(prompt),
      tools_(tools),
      skills_(skills),
      sessions_(sessions),
      config_(std::move(config)) {}

std::vector<std::filesystem::path> AgentPresetRoster::roots() const {
    std::vector<std::filesystem::path> result;
    if (config_.include_shipped_root) {
        std::filesystem::path shipped = default_presets_shipped_root();
        if (!shipped.empty()) {
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
    throw PresetNotFound("unknown preset '" + *target + "'");
}

ScopeKey AgentPresetRoster::standing_key_for(std::optional<std::string> id) const {
    return "preset:" + resolve(std::move(id)).id;
}

void AgentPresetRoster::mount(AgentContext& ctx, std::optional<std::string> id) {
    const AgentPreset& preset = resolve(std::move(id));
    const ScopeKey     key    = "preset:" + preset.id;
    standing_.emplace(preset.id, key);
    ctx.scope                 = key;
    leaves_[ctx.agent.value] = ctx;
}

std::string AgentPresetRoster::composed_preset(const AgentContext& ctx) const {
    constexpr std::string_view kStandingPrefix = "preset:";
    if (ctx.scope.rfind(kStandingPrefix, 0) == 0) {
        return ctx.scope.substr(kStandingPrefix.size());
    }
    return {};
}

const PresetConfig& AgentPresetRoster::config() const noexcept {
    return config_;
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
    const AgentPreset&      target  = resolve(preset);
    AgentContext            ctx     = leaf_for(agent.id());
    std::shared_ptr<Session> session = sessions_.sessionPtr(agent.session());
    if (!is_blank(*session)) {
        throw CompositionFixed("session composition is fixed after the first message");
    }
    ctx.scope                 = "preset:" + target.id;
    leaves_[agent.id().value] = ctx;
    session->append(payload::AgentPresetSelected{.agent_preset = target.id});
}

} // namespace ymh
