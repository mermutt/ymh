#include "ymh/agent/workspace_runtime.hpp"

#include <algorithm>
#include <chrono>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/compactor.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/llm_pool.hpp"
#include "ymh/agent/model_selection.hpp"
#include "ymh/agent/plan_mode_controller.hpp"
#include "ymh/agent/preset.hpp"
#include "ymh/agent/session_activator.hpp"
#include "ymh/agent/subagent_service.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/execution/config.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/pty.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/jobs/job_registry.hpp"
#include "ymh/jobs/job_wakeup.hpp"
#include "ymh/llm/llm_runtime.hpp"
#include "ymh/llm/provider_registry.hpp"
#include "ymh/mcp/mcp_manager.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/prompt/instructions.hpp"
#include "ymh/prompt/persona.hpp"
#include "ymh/prompt/runtime_context.hpp"
#include "ymh/prompt/system_prompt.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/skills/skill_catalog.hpp"
#include "ymh/skills/skill_tool.hpp"
#include "ymh/skills/workspace_trust.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/plan_tools.hpp"
#include "ymh/tools/subagent_tools.hpp"
#include "ymh/tools/terminal_tool.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace ymh {
namespace {

std::string sandbox_mode_name(SandboxMode mode) {
    switch (mode) {
        case SandboxMode::Workspace:
            return "workspace";
        case SandboxMode::ReadOnly:
            return "read-only";
        case SandboxMode::Unrestricted:
            return "unrestricted";
    }
    return "workspace";
}

std::shared_ptr<SkillCatalog> make_skill_catalog(const Config& config,
                                                 const ExecutionEnvironment& environment,
                                                 Logger& logger) {
    std::vector<SkillRoot> roots = default_skill_roots();
    roots.push_back(SkillRoot{environment.root() / ".ymh" / "skills", SkillSource::Workspace,
                              SkillTrust::Untrusted});
    SkillCatalogConfig catalog_config = to_skill_catalog_config(config);
    catalog_config.workspace_trusted =
        WorkspaceTrustStore{}.is_trusted(environment.root());
    auto catalog =
        std::make_shared<SkillCatalog>(catalog_config, environment, std::move(roots), logger);
    if (catalog->config().enabled) {
        catalog->discover();
    }
    return catalog;
}

AgentConfig make_agent_config(const Config& config,
                              const SkillCatalog& catalog,
                              const PermissionPolicy& policy,
                              bool prompt_path_available) {
    AgentConfig agent = to_agent_config(config);
    if (skill_tool_usable(policy, prompt_path_available)) {
        const std::string& index = catalog.index_section();
        if (!index.empty()) {
            if (!agent.system_prompt.empty() && agent.system_prompt.back() != '\n') {
                agent.system_prompt.push_back('\n');
            }
            agent.system_prompt += index;
        }
    }
    return agent;
}

ToolConfig make_tool_config(const Config& config) {
    ToolConfig tool_config;
    tool_config.tool_timeout = std::chrono::milliseconds{config.tools.timeout_ms};
    return tool_config;
}

PresetConfig make_preset_config(const Config& config) {
    PresetConfig preset;
    preset.root                 = config.presets.root;
    preset.default_id           = config.presets.default_id;
    preset.include_shipped_root = config.presets.include_shipped_root;
    preset.include_user_root    = config.presets.include_user_root;
    preset.max_depth            = config.presets.max_depth;
    return preset;
}

// 55-D6/§4: the live route-catalog seam. Backs `list_subagent_models` and the
// D6 model/effort validation; composes `ModelCatalog` with the configured
// endpoint set.
class WorkspaceRouteCatalog final : public RouteCatalog {
public:
    explicit WorkspaceRouteCatalog(const ModelCatalog& catalog) : catalog_(catalog) {}

    std::vector<std::string> routable_endpoints() const override {
        std::vector<std::string> out{std::string{}};
        for (const ModelCatalogEntry& entry : catalog_.entries()) {
            if (!entry.endpoint.name.empty() &&
                std::find(out.begin(), out.end(), entry.endpoint.name) == out.end()) {
                out.push_back(entry.endpoint.name);
            }
        }
        return out;
    }

    bool is_routable_endpoint(std::string_view endpoint) const override {
        if (endpoint.empty()) {
            return true;
        }
        for (const ModelCatalogEntry& entry : catalog_.entries()) {
            if (std::string_view{entry.endpoint.name} == endpoint) {
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> models_for(std::string_view endpoint) const override {
        std::vector<std::string> out;
        for (const ModelCatalogEntry& entry : catalog_.entries()) {
            if (std::string_view{entry.endpoint.name} == endpoint) {
                out.push_back(entry.name.empty() ? entry.model_id : entry.name);
            }
        }
        return out;
    }

    std::vector<std::string> efforts_for(std::string_view, std::string_view) const override {
        return {};
    }

    bool is_catalog_member(std::string_view endpoint, std::string_view model) const override {
        for (const ModelCatalogEntry& entry : catalog_.entries()) {
            if (std::string_view{entry.endpoint.name} != endpoint) {
                continue;
            }
            if (entry.model_id == model || entry.name == model) {
                return true;
            }
        }
        return false;
    }

private:
    const ModelCatalog& catalog_;
};

// 55-D11: the live background-activation seam. Runs the child's activation on a
// detached worker so it never blocks the caller and never sets
// `HostRuntime::active_session_`.
class ThreadedActivator final : public SessionActivator {
public:
    void set_service(SubagentService* service) { service_ = service; }

    bool submit(const SessionId& child) override {
        if (service_ == nullptr) {
            return false;
        }
        std::thread([this, child] { static_cast<void>(service_->activateChild(child)); }).detach();
        return true;
    }

private:
    SubagentService* service_ = nullptr;
};

std::vector<std::string> permission_preset_names(const Config& config) {
    std::vector<std::string> names;
    names.reserve(config.permissions.presets.size());
    for (const auto& [name, settings] : config.permissions.presets) {
        (void)settings;
        names.push_back(name);
    }
    return names;
}

std::map<std::string, ResolvedEndpoint> make_endpoint_map(const Config& config) {
    std::map<std::string, ResolvedEndpoint> endpoints;
    for (const auto& [name, settings] : config.llm.endpoints) {
        (void)settings;
        endpoints.emplace(name, resolve_endpoint(config, name));
    }
    return endpoints;
}

} // namespace

bool skill_tool_usable(const PermissionPolicy& policy, bool prompt_path_available) {
    PermissionRequest probe;
    probe.tool = "skill";
    const PolicyVerdict verdict = policy.evaluate(probe);
    if (verdict == PolicyVerdict::Deny) {
        return false;
    }
    if (verdict == PolicyVerdict::Allow) {
        return true;
    }
    return prompt_path_available;
}

class WorkspaceRuntime::Impl {
public:
    Impl(Config config,
         std::filesystem::path root,
         std::unique_ptr<SessionStore> store,
         SessionPersistence* persistence,
         LLMProviderConfig provider_config,
         std::unique_ptr<LLMProvider> provider,
         ProviderRegistry providers,
         std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)> provider_factory,
         bool attach_permission_gate,
         bool attach_permission_resolver,
         Executor* executor,
         McpClientFactory mcp_client_factory,
         GrantStore* grant_store)
        : root_(std::move(root)),
          store_(std::move(store)),
          persistence_(persistence),
          tool_config_(make_tool_config(config)),
          governor_(),
          pty_events_(),
          pty_(executor != nullptr
                   ? std::make_unique<LocalPtyService>(*executor, governor_,
                                                       pty_events_, tool_config_)
                   : nullptr),
          environment_(std::make_unique<LocalEnvironment>(
              root_, effective_sandbox_mode(config), tool_config_, pty_.get())),
          skill_catalog_(make_skill_catalog(config, *environment_,
                                            category_logger(LogCategory::Tool))),
          permission_config_(to_permission_config(config)),
          default_permission_preset_(default_permission_preset_name(config)),
          permission_defaults_(config.permissions),
          permission_baseline_(deployment_permission_baseline(config)),
          policy_(permission_config_, grant_store),
          gate_(policy_, permission_config_),
          agent_config_(make_agent_config(config, *skill_catalog_, policy_,
                                          attach_permission_gate ||
                                              attach_permission_resolver)),
          prompt_(config.tools.tool_order),
          assembler_(tools_, agent_config_.system_prompt),
          provider_config_(std::move(provider_config)),
          pool_(governor_.caps().max_llm_concurrency),
          ring_(governor_.caps().session_output_ring_bytes),
          sink_(ring_),
          sessions_(*store_, bus_),
          model_catalog_(ModelCatalog::build(config)),
          plan_mode_([this](const SessionId& id, payload::PlanMode mode) {
              try {
                  if (auto session = sessions_.sessionPtr(id)) {
                      session->append(mode);
                  }
              } catch (const UnknownSession&) {
              }
          }),
          model_selection_(
              [this](const SessionId& id, payload::SessionModelChanged change) {
                  try {
                      if (auto session = sessions_.sessionPtr(id)) {
                          session->append(change);
                      }
                  } catch (const UnknownSession&) {
                  }
              },
              [this](const std::string& wire_id) -> std::optional<ModelSelection> {
                  const std::optional<ModelCatalogEntry> entry = model_catalog_.find(wire_id);
                  if (!entry.has_value()) {
                      return std::nullopt;
                  }
                  return ModelSelection{entry->model_id, entry->name, entry->endpoint.name,
                                        entry->parameters, entry->profile,
                                        entry->endpoint.provider};
              }),
          roster_(std::make_unique<AgentPresetRoster>(
              prompt_, tools_, *skill_catalog_, sessions_, make_preset_config(config),
              &category_logger(LogCategory::Tool), permission_preset_names(config))),
          providers_(std::move(providers)),
          provider_factory_(std::move(provider_factory)),
          endpoints_(make_endpoint_map(config)) {
        for (std::unique_ptr<Tool>& tool : make_builtin_tools(tool_config_)) {
            registrations_.push_back(tools_.add(std::move(tool)));
        }
        registrations_.push_back(tools_.add(make_exit_plan_mode_tool()));
        if (environment_->pty().available()) {
            registrations_.push_back(tools_.add(make_terminal_tool(tool_config_)));
        }
        if (skill_catalog_->config().enabled &&
            skill_tool_usable(policy_, attach_permission_gate ||
                                           attach_permission_resolver)) {
            registrations_.push_back(tools_.add(make_skill_tool(skill_catalog_)));
        }
        mcp_ = std::make_unique<McpManager>(to_mcp_config(config), tool_config_,
                                            *environment_, governor_, tools_, bus_,
                                            category_logger(LogCategory::Mcp));
        if (mcp_client_factory) {
            mcp_->setClientFactory(std::move(mcp_client_factory));
        }
        mcp_->start({}).get();

        assembler_.set_plan_policy_provider(
            [this](const Session& session) -> std::string {
                return plan_mode_.active(session) ? agent_config_.plan_section : std::string{};
            });

        DefaultPromptConfig prompt_config;
        prompt_config.identity = agent_config_.system_prompt;
        prompt_config.persona  = default_persona_config();
        prompt_config.model    = agent_config_.model;
        prompt_config.cwd      = root_.string();
        const bool include_runtime_context = prompt_config.persona.include_runtime_context;
        default_prompt_        = register_default_prompt(prompt_, std::move(prompt_config));
        prompt_.set_tool_provider(
            [this](const AssembleContext&) { return tools_.schemas(); });
        if (include_runtime_context) {
            RuntimeContextConfig runtime;
            runtime.cwd        = root_.string();
            runtime.model      = agent_config_.model;
            runtime.sandbox    = sandbox_mode_name(agent_config_.sandbox);
            runtime.approval   = default_permission_preset_;
            runtime.delegation = config.presets.max_depth == 0
                                     ? std::string{"disabled"}
                                     : "available (max depth " +
                                           std::to_string(config.presets.max_depth) + ")";
            runtime_context_ = register_runtime_context(prompt_, std::move(runtime));
        }
        bool                  instructions_enabled = config.prompt.instructions_enabled;
        InstructionFileConfig instruction_config   = config.prompt.instructions;
        if (const std::optional<PresetInstructions> row =
                roster_->instructions_for(roster_->config().default_id);
            row.has_value()) {
            instructions_enabled = row->enabled;
            if (row->max_bytes.has_value()) {
                instruction_config.max_bytes = *row->max_bytes;
            }
        }
        if (instructions_enabled) {
            instructions_ =
                std::make_unique<InstructionLoader>(instruction_config, *environment_);
        }
        assembler_.set_system_prompt(&prompt_);

        services_.sessions        = &sessions_;
        services_.plan_mode       = &plan_mode_;
        services_.model_selection = &model_selection_;
        services_.governor        = &governor_;
        services_.tools           = &tools_;
        services_.policy          = &policy_;
        services_.gate            = attach_permission_gate ? &gate_ : nullptr;
        services_.context         = &assembler_;
        services_.prompt          = &prompt_;
        services_.instructions    = instructions_.get();
        services_.execution       = environment_.get();
        services_.logger          = &category_logger(LogCategory::Tool);
        services_.output          = &sink_;
        services_.estimator       = &estimator_;
        services_.presets         = roster_.get();

        if (provider != nullptr) {
            std::shared_ptr<LLMProvider> adapter = std::move(provider);
            std::vector<ProviderId>      routes;
            if (!provider_config_.provider.empty()) {
                routes.push_back(provider_config_.provider);
            } else {
                routes.push_back(adapter->id());
            }
            adapter_handle_ = runtime_.register_adapter(std::move(routes), std::move(adapter));
        }
        runtime_.set_route_resolver(
            [this](std::string_view endpoint_name, std::string_view profile_id) {
                return resolve_endpoint_provider(endpoint_name, profile_id);
            });
        services_.runtime = &runtime_;
        services_.pool    = &pool_;

        if (adapter_handle_.has_value()) {
            compactor_ = std::make_unique<ContextCompactor>(
                runtime_, pool_, estimator_, to_compaction_policy(config),
                std::chrono::system_clock::now, &model_catalog_);
            services_.context_compactor = compactor_.get();
        }

        route_catalog_ = std::make_unique<WorkspaceRouteCatalog>(model_catalog_);
        services_.route_catalog = route_catalog_.get();
        agents_ = std::make_unique<AgentRegistry>(services_, agent_config_);

        job_wakeup_ = std::make_unique<JobWakeupPolicy>(job_registry_, bus_,
                                                        JobWakeupConfig{}, *agents_);
        job_wakeup_->start();

        session_activator_ = std::make_unique<ThreadedActivator>();
        subagent_service_  = std::make_unique<SubagentService>(
            *agents_, sessions_, roster_.get(), job_registry_, *job_wakeup_, model_selection_,
            *route_catalog_, runtime_, *session_activator_, bus_);
        session_activator_->set_service(subagent_service_.get());

        DelegationToolConfig one_shot;
        one_shot.provider        = "subagent";
        one_shot.tool_name       = "subagent";
        one_shot.background_mode = DelegationToolConfig::BackgroundMode::OneShot;
        one_shot.model_selection = true;
        registrations_.push_back(tools_.add(make_subagent_tool(
            *subagent_service_, SubagentCallerResolver{}, std::move(one_shot))));

        DelegationToolConfig continuable;
        continuable.provider        = "subagent_continuable";
        continuable.tool_name       = "subagent_continuable";
        continuable.background_mode = DelegationToolConfig::BackgroundMode::Continuable;
        continuable.model_selection = true;
        registrations_.push_back(tools_.add(make_subagent_tool(
            *subagent_service_, SubagentCallerResolver{}, std::move(continuable))));

        registrations_.push_back(
            tools_.add(make_send_message_tool(*subagent_service_, SubagentCallerResolver{})));
        registrations_.push_back(
            tools_.add(make_interrupt_agent_tool(*subagent_service_, SubagentCallerResolver{})));
        registrations_.push_back(
            tools_.add(make_list_agents_tool(*subagent_service_, SubagentCallerResolver{})));
        registrations_.push_back(
            tools_.add(make_list_subagent_models_tool(*route_catalog_)));

        tools_.freeze();
    }

    std::shared_ptr<LLMProvider> resolve_endpoint_provider(std::string_view endpoint_name,
                                                           std::string_view profile_id) {
        std::lock_guard<std::mutex> lock(endpoint_cache_mutex_);
        const std::pair<std::string, std::string> key{std::string{endpoint_name},
                                                      std::string{profile_id}};
        if (const auto cached = endpoint_cache_.find(key); cached != endpoint_cache_.end()) {
            return cached->second;
        }
        const auto endpoint_it = endpoints_.find(key.first);
        if (endpoint_it == endpoints_.end()) {
            return nullptr;
        }
        const ModelProfile* found = find_model_profile(key.second);
        static const ModelProfile kInertProfile{};
        const ModelProfile& profile = (found != nullptr) ? *found : kInertProfile;
        const LLMProviderConfig config = to_provider_config(endpoint_it->second, profile);

        std::unique_ptr<LLMProvider> created;
        if (provider_factory_) {
            created = provider_factory_(config);
        }
        if (created == nullptr) {
            std::expected<std::unique_ptr<LLMProvider>, LLMError> result =
                providers_.create(config);
            if (!result.has_value()) {
                return nullptr;
            }
            created = std::move(*result);
        }
        std::shared_ptr<LLMProvider> provider{std::move(created)};
        AdapterHandle handle =
            runtime_.register_endpoint_route(endpoint_name, profile_id, provider);
        endpoint_handles_.insert_or_assign(key, std::move(handle));
        endpoint_cache_.emplace(key, provider);
        return provider;
    }

    std::filesystem::path              root_;
    std::unique_ptr<SessionStore>      store_;
    SessionPersistence*                persistence_ = nullptr;
    EventBus                           bus_;
    ToolConfig                         tool_config_;
    ResourceGovernor                   governor_;
    NoopPtyEventSink                   pty_events_;
    std::unique_ptr<LocalPtyService>   pty_;
    std::unique_ptr<LocalEnvironment>  environment_;
    std::shared_ptr<SkillCatalog>      skill_catalog_;
    ToolRegistry                       tools_;
    std::vector<ToolRegistry::Registration> registrations_;
    std::unique_ptr<McpManager>        mcp_;
    PermissionConfig                   permission_config_;
    std::string                        default_permission_preset_;
    PermissionDefaults                 permission_defaults_;
    PermissionPresetSettings           permission_baseline_;
    RulePermissionPolicy               policy_;
    PermissionGate                     gate_;
    AgentConfig                        agent_config_;
    SystemPrompt                       prompt_;
    DefaultPromptHandles               default_prompt_;
    ContextHandle                      runtime_context_;
    std::unique_ptr<InstructionLoader> instructions_;
    SessionContextAssembler            assembler_;
    DefaultTokenEstimator              estimator_;
    LLMProviderConfig                  provider_config_;
    LlmRuntime                         runtime_;
    std::optional<AdapterHandle>       adapter_handle_;
    LLMPool                            pool_;
    std::unique_ptr<ContextCompactor>  compactor_;
    OutputRing                         ring_;
    RingOutputSink                     sink_;
    SessionManager                     sessions_;
    ModelCatalog                       model_catalog_;
    PlanModeController                 plan_mode_;
    ModelSelectionController           model_selection_;
    std::unique_ptr<AgentPresetRoster> roster_;
    AgentServices                      services_;
    std::unique_ptr<AgentRegistry>     agents_;
    std::unique_ptr<RouteCatalog>      route_catalog_;
    JobRegistry                        job_registry_;
    std::unique_ptr<JobWakeupPolicy>   job_wakeup_;
    std::unique_ptr<ThreadedActivator> session_activator_;
    std::unique_ptr<SubagentService>   subagent_service_;
    ProviderRegistry                   providers_;
    std::function<std::unique_ptr<LLMProvider>(const LLMProviderConfig&)> provider_factory_;
    std::map<std::string, ResolvedEndpoint> endpoints_;
    std::map<std::pair<std::string, std::string>, std::shared_ptr<LLMProvider>> endpoint_cache_;
    std::map<std::pair<std::string, std::string>, AdapterHandle> endpoint_handles_;
    mutable std::mutex                 endpoint_cache_mutex_;
};

WorkspaceRuntime::WorkspaceRuntime(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

WorkspaceRuntime::~WorkspaceRuntime() = default;

std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError>
WorkspaceRuntime::create(WorkspaceRuntimeOptions options) {
    std::error_code error;
    if (!std::filesystem::is_directory(options.root, error)) {
        return std::unexpected(WorkspaceRuntimeError{
            WorkspaceRuntimeErrorCode::WorkspaceMissing,
            "workspace root is not a directory: " + options.root.string()});
    }

    std::unique_ptr<SessionStore> store;
    SessionPersistence*           persistence = nullptr;
    if (options.store_factory) {
        try {
            store = options.store_factory();
        } catch (const std::exception& factory_error) {
            return std::unexpected(WorkspaceRuntimeError{WorkspaceRuntimeErrorCode::StoreUnavailable,
                                                         factory_error.what()});
        }
        if (store == nullptr) {
            return std::unexpected(
                WorkspaceRuntimeError{WorkspaceRuntimeErrorCode::StoreUnavailable,
                                      "store_factory returned null"});
        }
    } else {
        PersistenceConfig persistence_config;
        persistence_config.db_path   = options.root / ".ymh" / "sessions.db";
        persistence_config.lock_path = options.root / ".ymh" / "sessions.lock";
        persistence_config.boot_id   = options.boot_id;
        try {
            std::unique_ptr<SessionPersistence> opened =
                SessionPersistence::open(persistence_config);
            persistence = opened.get();
            store       = std::move(opened);
        } catch (const StoreOpenError& open_error) {
            const WorkspaceRuntimeErrorCode code =
                open_error.code() == StoreOpenErrorCode::Locked
                    ? WorkspaceRuntimeErrorCode::WorkspaceBusy
                    : WorkspaceRuntimeErrorCode::StoreUnavailable;
            return std::unexpected(WorkspaceRuntimeError{code, open_error.what()});
        } catch (const std::exception& open_error) {
            return std::unexpected(WorkspaceRuntimeError{WorkspaceRuntimeErrorCode::StoreUnavailable,
                                                         open_error.what()});
        }
    }

    ProviderRegistry   providers       = make_default_provider_registry();
    LLMProviderConfig  provider_config = to_provider_config(options.config);
    std::unique_ptr<LLMProvider> provider;
    try {
        if (options.provider_factory) {
            provider = options.provider_factory(provider_config);
        }
        if (provider == nullptr) {
            std::expected<std::unique_ptr<LLMProvider>, LLMError> created =
                providers.create(provider_config);
            if (created.has_value()) {
                provider = std::move(*created);
            }
        }
    } catch (const std::exception& factory_error) {
        return std::unexpected(WorkspaceRuntimeError{WorkspaceRuntimeErrorCode::ProviderSetupFailed,
                                                     factory_error.what()});
    }

    try {
        auto impl = std::make_unique<Impl>(std::move(options.config), std::move(options.root),
                                           std::move(store), persistence,
                                           std::move(provider_config), std::move(provider),
                                           std::move(providers),
                                           std::move(options.provider_factory),
                                           options.attach_permission_gate,
                                           options.attach_permission_resolver, options.executor,
                                           std::move(options.mcp_client_factory),
                                           options.grant_store);
        auto runtime = std::unique_ptr<WorkspaceRuntime>(new WorkspaceRuntime(std::move(impl)));
        runtime->replayUnreportedSettlements();
        return runtime;
    } catch (const std::exception& build_error) {
        return std::unexpected(
            WorkspaceRuntimeError{WorkspaceRuntimeErrorCode::Internal, build_error.what()});
    }
}

std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError>
make_workspace_runtime(WorkspaceRuntimeOptions options) {
    return WorkspaceRuntime::create(std::move(options));
}

const std::filesystem::path& WorkspaceRuntime::root() const noexcept { return impl_->root_; }

ExecutionEnvironment& WorkspaceRuntime::environment() noexcept { return *impl_->environment_; }
ResourceGovernor&     WorkspaceRuntime::governor() noexcept { return impl_->governor_; }
EventBus&             WorkspaceRuntime::bus() noexcept { return impl_->bus_; }
SessionStore&         WorkspaceRuntime::store() noexcept { return *impl_->store_; }
SessionPersistence*   WorkspaceRuntime::persistence() noexcept { return impl_->persistence_; }
bool WorkspaceRuntime::hasDurableStore() const noexcept { return impl_->persistence_ != nullptr; }
SessionManager&       WorkspaceRuntime::sessions() noexcept { return impl_->sessions_; }
AgentRegistry&        WorkspaceRuntime::agents() noexcept { return *impl_->agents_; }
ToolRegistry&         WorkspaceRuntime::tools() noexcept { return impl_->tools_; }
PermissionPolicy&     WorkspaceRuntime::policy() noexcept { return impl_->policy_; }
PermissionGate&       WorkspaceRuntime::gate() noexcept { return impl_->gate_; }
SkillCatalog&         WorkspaceRuntime::skills() noexcept { return *impl_->skill_catalog_; }
const SkillCatalog&   WorkspaceRuntime::skills() const noexcept {
    return *impl_->skill_catalog_;
}
bool                  WorkspaceRuntime::has_provider() const noexcept {
    return impl_->adapter_handle_.has_value();
}
LLMPool&              WorkspaceRuntime::pool() noexcept { return impl_->pool_; }
ContextAssembler&     WorkspaceRuntime::context() noexcept { return impl_->assembler_; }
PlanModeController&   WorkspaceRuntime::plan_mode() noexcept { return impl_->plan_mode_; }
ModelCatalog&         WorkspaceRuntime::model_catalog() noexcept { return impl_->model_catalog_; }
ModelSelectionController& WorkspaceRuntime::model_selection() noexcept {
    return impl_->model_selection_;
}

bool WorkspaceRuntime::is_routable_endpoint(std::string_view name) const {
    if (name.empty()) {
        return true;
    }
    return impl_->endpoints_.find(std::string{name}) != impl_->endpoints_.end();
}

const AgentConfig& WorkspaceRuntime::agent_config() const noexcept { return impl_->agent_config_; }

const std::string& WorkspaceRuntime::default_permission_preset() const noexcept {
    return impl_->default_permission_preset_;
}

std::string WorkspaceRuntime::effective_permission_preset(
    const std::optional<std::string>& agent_preset) const {
    std::optional<std::string> binding;
    if (agent_preset.has_value() && !agent_preset->empty()) {
        binding = impl_->roster_->permission_preset_for(agent_preset);
    }
    return effective_permission_preset_name(impl_->permission_defaults_,
                                            impl_->permission_baseline_,
                                            impl_->default_permission_preset_, binding);
}

const LLMProviderConfig& WorkspaceRuntime::provider_config() const noexcept {
    return impl_->provider_config_;
}

const TokenEstimator& WorkspaceRuntime::estimator() const noexcept { return impl_->estimator_; }

const CompactionPolicy* WorkspaceRuntime::compaction_policy() const noexcept {
    return impl_->compactor_ != nullptr ? &impl_->compactor_->policy() : nullptr;
}

std::vector<McpServerStatus> WorkspaceRuntime::mcp_statuses() const {
    return impl_->mcp_ != nullptr ? impl_->mcp_->statuses()
                                  : std::vector<McpServerStatus>{};
}

McpManager& WorkspaceRuntime::mcp() noexcept { return *impl_->mcp_; }

AgentPresetRoster& WorkspaceRuntime::presets() noexcept { return *impl_->roster_; }

void WorkspaceRuntime::shutdownChildren(std::chrono::milliseconds grace) {
    impl_->environment_->pty().closeAll();
    if (impl_->mcp_ != nullptr) {
        mcp().shutdown(grace).get();
    }
}

bool WorkspaceRuntime::acquireLease(const SessionId& id) {
    if (!hasDurableStore()) {
        return true;
    }
    return impl_->persistence_->acquireLease(id);
}

bool WorkspaceRuntime::releaseLease(const SessionId& id) {
    if (!hasDurableStore()) {
        return false;
    }
    return impl_->persistence_->releaseLease(id);
}

void WorkspaceRuntime::renewLeases() {
    if (!hasDurableStore()) {
        return;
    }
    impl_->persistence_->renewLeases();
}

namespace {

bool has_background_fanin(SessionStore& store, const SessionId& id) {
    try {
        for (const EventRecord& record : store.read(id)) {
            if (record.event.type != EventType::SubagentFanIn) {
                continue;
            }
            if (record.event.payload.get<payload::SubagentFanIn>().notice_expected) {
                return true;
            }
        }
    } catch (const std::exception&) {
    }
    return false;
}

} // namespace

void WorkspaceRuntime::replayUnreportedSettlements(const SessionId& parent) {
    try {
        (void)acquireLease(parent);
    } catch (const std::exception&) {
        // Best effort: the append below throws `LeaseLost` and this pass is a
        // no-op (at-least-once; the next resume/daemon start retries).
    }
    try {
        impl_->subagent_service_->replayUnreportedSettlements(parent);
    } catch (const std::exception&) {
    }
}

void WorkspaceRuntime::replayUnreportedSettlements() {
    std::vector<SessionHeader> headers;
    try {
        headers = impl_->store_->list();
    } catch (const std::exception&) {
        return;
    }
    for (const SessionHeader& header : headers) {
        if (!has_background_fanin(*impl_->store_, header.id)) {
            continue;
        }
        replayUnreportedSettlements(header.id);
    }
}

} // namespace ymh
