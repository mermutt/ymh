#include "ymh/agent/model_selection.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ymh {
namespace {

bool same_params(const GenerationParameters& a, const GenerationParameters& b) {
    return a.temperature == b.temperature && a.top_p == b.top_p && a.top_k == b.top_k &&
           a.max_output_tokens == b.max_output_tokens && a.stop == b.stop &&
           a.tool_choice == b.tool_choice && a.reasoning_effort == b.reasoning_effort &&
           a.seed == b.seed;
}

bool selection_equals(const ModelSelection& a, const ModelSelection& b) {
    return a.model == b.model && a.model_name == b.model_name && a.endpoint == b.endpoint &&
           a.provider == b.provider && a.profile.id == b.profile.id &&
           same_params(a.parameters, b.parameters);
}

int ascii_compare_ci(std::string_view a, std::string_view b) {
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto la = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(a[i])));
        const auto lb = static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(b[i])));
        if (la != lb) {
            return la < lb ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    return a.size() < b.size() ? -1 : 1;
}

ModelProfile profile_for(const std::string& id) {
    if (const ModelProfile* profile = find_model_profile(id); profile != nullptr) {
        return *profile;
    }
    return ModelProfile{};
}

// 53-D7/H4: mirrors `to_agent_config`'s parameter resolution for one selection.
GenerationParameters params_for(const std::optional<std::uint32_t>& max_tokens,
                                const std::optional<std::string>&   reasoning_effort,
                                const std::optional<double>&        temperature,
                                const std::optional<double>&        top_p,
                                const std::optional<std::uint32_t>& top_k,
                                const std::optional<std::string>&   tool_choice,
                                const std::vector<std::string>&     stop,
                                const std::optional<std::uint32_t>& seed,
                                const ModelProfile&                 profile,
                                const std::optional<std::string>&   agent_reasoning_effort) {
    const bool has_profile = !profile.id.empty();
    GenerationParameters out;
    if (agent_reasoning_effort.has_value()) {
        out.reasoning_effort = agent_reasoning_effort;
    } else if (reasoning_effort.has_value()) {
        out.reasoning_effort = reasoning_effort;
    }
    if (max_tokens.has_value()) {
        out.max_output_tokens = max_tokens;
    }
    if (temperature.has_value()) {
        out.temperature = temperature;
    } else if (has_profile) {
        out.temperature = profile.temperature;
    }
    if (top_p.has_value()) {
        out.top_p = top_p;
    } else if (has_profile) {
        out.top_p = profile.top_p;
    }
    if (top_k.has_value()) {
        out.top_k = top_k;
    } else if (has_profile) {
        out.top_k = profile.top_k;
    }
    if (tool_choice.has_value()) {
        out.tool_choice = tool_choice;
    }
    if (!stop.empty()) {
        out.stop = stop;
    }
    if (seed.has_value()) {
        out.seed = seed;
    }
    return out;
}

ResolvedEndpoint endpoint_for(const Config& config, const std::string& name) {
    const auto it = config.llm.endpoints.find(name);
    if (it == config.llm.endpoints.end()) {
        return ResolvedEndpoint{};
    }
    const EndpointSettings& settings = it->second;
    ResolvedEndpoint        resolved;
    resolved.name            = name;
    resolved.provider        = settings.provider;
    resolved.base_url        = settings.base_url;
    resolved.api_key_env     = settings.api_key_env;
    resolved.api_key         = settings.api_key;
    resolved.headers         = settings.headers;
    resolved.connect_timeout = settings.connect_timeout;
    resolved.idle_timeout    = settings.idle_timeout;
    resolved.request_timeout = settings.request_timeout;
    resolved.retry           = settings.retry;
    resolved.max_concurrency = settings.max_concurrency;
    return resolved;
}

ModelCatalogEntry default_catalog_entry(const Config& config) {
    const ResolvedModel resolved = resolve_model(config);
    ModelCatalogEntry   entry;
    entry.name       = resolved.model_name;
    entry.model_id   = resolved.model_id;
    entry.endpoint   = resolved.endpoint;
    entry.profile    = resolved.profile;
    entry.parameters = params_for(resolved.max_tokens, resolved.reasoning_effort,
                                  resolved.temperature, resolved.top_p, resolved.top_k,
                                  resolved.tool_choice, resolved.stop, resolved.seed,
                                  resolved.profile, config.agent.reasoning_effort);
    return entry;
}

ModelCatalogEntry named_catalog_entry(const Config& config, const std::string& name,
                                      const ModelSettings& model) {
    ModelCatalogEntry entry;
    entry.name     = name;
    entry.model_id = model.model;
    entry.endpoint = endpoint_for(config, model.endpoint);
    entry.profile  = profile_for(model.profile);
    entry.parameters =
        params_for(model.max_tokens, model.reasoning_effort, model.temperature, model.top_p,
                   model.top_k, model.tool_choice, model.stop, model.seed, entry.profile,
                   config.agent.reasoning_effort);
    return entry;
}

bool catalog_entry_less(const ModelCatalogEntry& a, const ModelCatalogEntry& b) {
    const int cmp = ascii_compare_ci(a.name, b.name);
    if (cmp != 0) {
        return cmp < 0;
    }
    return a.model_id < b.model_id;
}

} // namespace

ModelSelectionController::ModelSelectionController(AppendFn append, ResolveFn resolve)
    : append_(std::move(append)), resolve_(std::move(resolve)) {}

std::optional<ModelSelection> ModelSelectionController::durable_(const Session& session) const {
    const SessionId id = session.id();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  it = memo_.find(id);
        if (it != memo_.end() && it->second.valid) {
            if (!it->second.has) {
                return std::nullopt;
            }
            return it->second.selection;
        }
    }
    std::optional<ModelSelection> resolved;
    const SessionHeader           header = session.header();
    // 54-D8: prefer the `llm.models` entry name (unambiguous) when the durable
    // header carries it; fall back to the wire id for pre-54 sessions.
    const std::string wire_id =
        (header.model_name.has_value() && !header.model_name->empty()) ? *header.model_name
                                                                      : header.model;
    if (!wire_id.empty() && resolve_ != nullptr) {
        resolved = resolve_(wire_id);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    Memo&                       entry = memo_[id];
    if (!entry.valid) {
        entry.valid = true;
        entry.has   = resolved.has_value();
        if (resolved.has_value()) {
            entry.selection = *resolved;
        }
    }
    if (!entry.has) {
        return std::nullopt;
    }
    return entry.selection;
}

std::optional<ModelSelection> ModelSelectionController::effective(const Session& session) const {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  it = pending_.find(session.id());
        if (it != pending_.end()) {
            return it->second;
        }
    }
    return durable_(session);
}

ModelSetResult ModelSelectionController::set(const Session& session, bool turn_open,
                                             ModelSelection selection) {
    const SessionId id = session.id();
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    const std::optional<ModelSelection> logged = durable_(session);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  pending = pending_.find(id);

        if (logged.has_value() && selection_equals(*logged, selection)) {
            if (pending != pending_.end()) {
                pending_.erase(pending);
            }
            return ModelSetResult::Unchanged;
        }

        if (turn_open) {
            pending_[id] = std::move(selection);
            return ModelSetResult::Queued;
        }

        pending_.erase(id);
    }
    commit_locked_(id, selection);
    return ModelSetResult::Committed;
}

bool ModelSelectionController::apply_pending_at_step_start(Session& session) {
    const SessionId id = session.id();
    std::optional<ModelSelection> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  it = pending_.find(id);
        if (it != pending_.end()) {
            pending = std::move(it->second);
            pending_.erase(it);
        }
    }
    if (pending.has_value()) {
        commit_(id, *pending);
        return true;
    }
    return effective(session).has_value();
}

bool ModelSelectionController::flush_pending_at_turn_end(Session& session) noexcept {
    const SessionId id = session.id();
    std::optional<ModelSelection> pending;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto                  it = pending_.find(id);
        if (it != pending_.end()) {
            pending = std::move(it->second);
            pending_.erase(it);
        }
    }
    if (pending.has_value()) {
        try {
            commit_(id, *pending);
        } catch (...) {
            // A failed flush drops the pending selection; the durable log stands.
        }
    }
    return effective(session).has_value();
}

void ModelSelectionController::erase(const SessionId& session) noexcept {
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(session);
    memo_.erase(session);
}

void ModelSelectionController::commit_(const SessionId& id, const ModelSelection& selection) {
    std::lock_guard<std::mutex> commit_lock(commit_mutex_);
    commit_locked_(id, selection);
}

void ModelSelectionController::commit_locked_(const SessionId& id,
                                              const ModelSelection& selection) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        memo_[id].valid = false;
    }
    append_(id, payload::SessionModelChanged{selection.model, selection.model_name});
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Memo&                       entry = memo_[id];
        entry.valid                       = true;
        entry.has                         = true;
        entry.selection                   = selection;
    }
}

ModelCatalog ModelCatalog::build(const Config& config) {
    ModelCatalog catalog;
    catalog.default_ = default_catalog_entry(config);
    for (const auto& [name, model] : config.llm.models) {
        catalog.named_.push_back(named_catalog_entry(config, name, model));
    }
    std::sort(catalog.named_.begin(), catalog.named_.end(), catalog_entry_less);
    return catalog;
}

std::optional<ModelCatalogEntry> ModelCatalog::find(std::string_view name_or_id) const {
    if (name_or_id.empty()) {
        return std::nullopt;
    }
    for (const ModelCatalogEntry& entry : named_) {
        if (entry.name == name_or_id) {
            return entry;
        }
    }
    if (default_.name == name_or_id) {
        return default_;
    }
    for (const ModelCatalogEntry& entry : named_) {
        if (entry.model_id == name_or_id) {
            return entry;
        }
    }
    if (default_.model_id == name_or_id) {
        return default_;
    }
    return std::nullopt;
}

const ModelCatalogEntry& ModelCatalog::default_entry() const { return default_; }

std::vector<ModelCatalogEntry> ModelCatalog::entries() const {
    std::vector<ModelCatalogEntry> out;
    out.reserve(named_.size() + 1);
    out.push_back(default_);
    out.insert(out.end(), named_.begin(), named_.end());
    std::sort(out.begin(), out.end(), catalog_entry_less);
    return out;
}

} // namespace ymh
