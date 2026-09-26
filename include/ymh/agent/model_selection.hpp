#pragma once

// 53-D7: the daemon-side mid-flight model selection seam. A `ModelSelection`
// carries the selected entry's wire id AND its resolved generation parameters,
// profile, and provider, so a switch changes the whole request shape rather than
// only the model id. `ModelSelectionController` mirrors `PlanModeController`
// (25-D2): the durable `session/model` log is the source of truth; an open turn
// queues a change and it is applied at the next step boundary.
//
// `ModelCatalog` is built once at daemon startup from the resolved `Config`
// (`WorkspaceRuntime::Impl` does not retain `Config`) and resolves a
// `llm.models` name or a literal wire id to a full entry.

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ymh/config/config.hpp"
#include "ymh/llm/llm_call_config.hpp"
#include "ymh/llm/llm_request.hpp"
#include "ymh/llm/model_profile.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session.hpp"

namespace ymh {

struct ModelSelection {
    std::string          model;       // wire id (NEVER an llm.models name)
    std::string          model_name;  // llm.models entry name; "" for a literal
    std::string          endpoint;    // 54-D2: endpoint name; "" = anonymous default
    GenerationParameters parameters;  // selected entry's resolved params
    ModelProfile         profile;     // selected entry's profile
    ProviderId           provider;    // selected entry's endpoint provider
};

enum class ModelSetResult : std::uint8_t { Unchanged, Committed, Queued };

class ModelSelectionController {
public:
    using AppendFn = std::function<void(const SessionId&, payload::SessionModelChanged)>;
    using ResolveFn = std::function<std::optional<ModelSelection>(const std::string& wire_id)>;
    // 55-A7/55-H4: rebuilds the route from the persisted (endpoint, profile_id)
    // pair, so a literal wire id shared by two endpoints cannot rebind wrongly.
    using RouteResolveFn = std::function<std::optional<ModelSelection>(
        const std::string& endpoint, const std::string& profile_id, const std::string& wire_id)>;

    ModelSelectionController(AppendFn append, ResolveFn resolve,
                             RouteResolveFn resolve_route = nullptr);

    ModelSetResult set(const Session& session, bool turn_open, ModelSelection selection);

    bool apply_pending_at_step_start(Session& session);

    bool flush_pending_at_turn_end(Session& session) noexcept;

    // Pending wins; else the durable selection (the folded `header_.model` wire
    // id) rebuilt through `resolve_`; nullopt only when the session has no model
    // or it does not resolve.
    [[nodiscard]] std::optional<ModelSelection> effective(const Session& session) const;

    void erase(const SessionId& session) noexcept;

private:
    struct Memo {
        bool           valid = false;
        bool           has   = false;
        ModelSelection selection;
    };

    [[nodiscard]] std::optional<ModelSelection> durable_(const Session& session) const;

    void commit_(const SessionId& id, const ModelSelection& selection);

    // `commit_` body; the caller must hold `commit_mutex_`.
    void commit_locked_(const SessionId& id, const ModelSelection& selection);

    AppendFn                            append_;
    ResolveFn                           resolve_;
    RouteResolveFn                      resolve_route_;
    mutable std::mutex                  mutex_;
    std::mutex                          commit_mutex_;
    std::map<SessionId, ModelSelection> pending_;
    mutable std::map<SessionId, Memo>   memo_;
};

struct ModelCatalogEntry {
    std::string          name;        // llm.models key; "" for the default entry
    std::string          model_id;    // wire id
    ResolvedEndpoint     endpoint;    // carries the ProviderId
    GenerationParameters parameters;  // resolved for THIS entry
    ModelProfile         profile;     // find_model_profile(entry.profile)
};

class ModelCatalog {
public:
    static ModelCatalog build(const Config& config);

    // An entry `name` match wins; else the literal is matched against each
    // entry's `model_id` (sorted order, first match) and yields that entry's
    // endpoint/parameters/profile. No match => nullopt.
    [[nodiscard]] std::optional<ModelCatalogEntry> find(std::string_view name_or_id) const;

    [[nodiscard]] const ModelCatalogEntry& default_entry() const;

    [[nodiscard]] std::vector<ModelCatalogEntry> entries() const;

private:
    std::vector<ModelCatalogEntry> named_;
    ModelCatalogEntry              default_;
};

} // namespace ymh
