#include "ymh/agent/workspace_runtime.hpp"

#include <chrono>
#include <exception>
#include <system_error>
#include <utility>
#include <vector>

#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/compactor.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/llm_pool.hpp"
#include "ymh/agent/model_selection.hpp"
#include "ymh/agent/plan_mode_controller.hpp"
#include "ymh/agent/preset.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/execution/config.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/pty.hpp"
#include "ymh/execution/resource_governor.hpp"
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

std::vector<std::string> permission_preset_names(const Config& config) {
    std::vector<std::string> names;
    names.reserve(config.permissions.presets.size());
    for (const auto& [name, settings] : config.permissions.presets) {
        (void)settings;
        names.push_back(name);
    }
    return names;
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
                  return ModelSelection{entry->model_id, entry->name, entry->parameters,
                                        entry->profile, entry->endpoint.provider};
              }),
          roster_(std::make_unique<AgentPresetRoster>(
              prompt_, tools_, *skill_catalog_, sessions_, make_preset_config(config),
              &category_logger(LogCategory::Tool), permission_preset_names(config))) {
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
        tools_.freeze();

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
        services_.runtime = &runtime_;
        services_.pool    = &pool_;

        if (adapter_handle_.has_value()) {
            compactor_ = std::make_unique<ContextCompactor>(
                runtime_, pool_, estimator_, to_compaction_policy(config));
            services_.context_compactor = compactor_.get();
        }

        agents_ = std::make_unique<AgentRegistry>(services_, agent_config_);
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
                                           options.attach_permission_gate,
                                           options.attach_permission_resolver, options.executor,
                                           std::move(options.mcp_client_factory),
                                           options.grant_store);
        return std::unique_ptr<WorkspaceRuntime>(new WorkspaceRuntime(std::move(impl)));
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

} // namespace ymh
