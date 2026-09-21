#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/compactor.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/plan_mode_controller.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/prompt/instructions.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/plan_tools.hpp"
#include "ymh/tools/tool_registry.hpp"

#include "test_env.hpp"

namespace ymh::test {

inline PermissionConfig allow_all_permission_config() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Allow;
    return config;
}

inline std::optional<AdapterHandle> register_test_adapter(
    LlmRuntime& runtime, const std::shared_ptr<LLMProvider>& provider) {
    if (provider == nullptr) {
        return std::nullopt;
    }
    std::vector<ProviderId> routes{provider->id()};
    return runtime.register_adapter(std::move(routes), provider);
}

class ProviderRuntime {
public:
    explicit ProviderRuntime(FakeScript script)
        : provider_(std::make_shared<FakeLLM>(std::move(script))) {
        handle_ = runtime_.register_adapter({provider_->id()}, provider_);
    }

    [[nodiscard]] LlmRuntime& runtime() noexcept { return runtime_; }

private:
    std::shared_ptr<LLMProvider> provider_;
    LlmRuntime                   runtime_;
    std::optional<AdapterHandle> handle_;
};

inline AgentServices make_agent_services(SessionManager& sessions,
                                         ResourceGovernor& governor,
                                         ToolRegistry& tools,
                                         PermissionPolicy& policy,
                                         PermissionGate* gate,
                                         ContextAssembler& context,
                                         ExecutionEnvironment& execution,
                                         Logger& logger,
                                         OutputSink& output,
                                         LlmRuntime& runtime,
                                         LLMPool& pool,
                                         TokenEstimator& estimator,
                                         AgentServices::PermissionResolver resolver,
                                         Compactor* compactor,
                                         ContextCompactor* context_compactor = nullptr,
                                         PlanModeController* plan_mode = nullptr,
                                         SystemPrompt* prompt = nullptr,
                                         InstructionLoader* instructions = nullptr) {
    AgentServices services;
    services.sessions            = &sessions;
    services.governor            = &governor;
    services.tools               = &tools;
    services.policy              = &policy;
    services.gate                = gate;
    services.context             = &context;
    services.execution           = &execution;
    services.logger              = &logger;
    services.output              = &output;
    services.runtime             = &runtime;
    services.pool                = &pool;
    services.estimator           = &estimator;
    services.compactor           = compactor;
    services.context_compactor   = context_compactor;
    services.permission_resolver = std::move(resolver);
    services.plan_mode           = plan_mode;
    services.prompt              = prompt;
    services.instructions        = instructions;
    return services;
}

struct AgentEnv {
    AgentEnv(const std::string& prefix,
             std::unique_ptr<LLMProvider> provider,
             AgentConfig config = {},
             PermissionConfig permission = allow_all_permission_config(),
             AgentServices::PermissionResolver resolver = {},
             bool register_builtins = false,
             std::size_t pool_capacity = 4,
             Compactor* compactor = nullptr,
             bool use_permission_gate = false,
             std::optional<CompactionPolicy> compaction = std::nullopt,
             WallClock wall_clock = std::chrono::system_clock::now,
             bool enable_plan_mode = false,
             SystemPrompt* prompt = nullptr,
             InstructionLoader* instructions = nullptr,
             GrantStore* grant_store = nullptr,
             ToolConfig tool_config = {})
        : workspace(prefix),
          sessions(store, bus),
          env(workspace.path(), SandboxMode::Workspace, tool_config),
          provider(std::move(provider)),
          runtime(),
          adapter_handle(register_test_adapter(runtime, this->provider)),
          policy(std::move(permission), grant_store),
          gate(use_permission_gate ? std::make_unique<PermissionGate>(policy, PermissionConfig{})
                                   : nullptr),
          assembler(tools, config.system_prompt),
          pool(pool_capacity),
          context_compactor(compaction.has_value()
                                ? std::make_unique<ContextCompactor>(runtime, pool, estimator,
                                                                     *compaction,
                                                                     std::move(wall_clock))
                                : nullptr),
          plan_mode_controller(enable_plan_mode
                                   ? std::optional<PlanModeController>(std::in_place,
                                         [this](const SessionId& id, payload::PlanMode mode) {
                                             if (auto session = sessions.sessionPtr(id)) {
                                                 session->append(mode);
                                             }
                                         })
                                   : std::nullopt),
          registry(make_agent_services(sessions, governor, tools, policy, gate.get(), assembler, env,
                                        logger, sink, runtime, pool, estimator,
                                        std::move(resolver), compactor, context_compactor.get(),
                                        plan_mode_controller ? &*plan_mode_controller : nullptr,
                                        prompt, instructions),
                   std::move(config)) {
        if (register_builtins) {
            for (std::unique_ptr<Tool>& tool : make_builtin_tools()) {
                keeper.add(std::move(tool));
            }
            if (plan_mode_controller.has_value()) {
                keeper.add(make_exit_plan_mode_tool());
            }
            tools.freeze();
        }
    }

    // X3: owning handle; callers bind `auto agent = env.createAgent();`.
    std::shared_ptr<AgentLoop> createAgent() {
        SessionOptions options;
        options.cwd           = workspace.path();
        options.serverProfile = "interactive";
        options.model         = "fake-model";
        options.title         = "test";
        const std::expected<AgentId, AgentError> created = registry.create(options);
        if (!created.has_value()) {
            throw std::runtime_error("create failed: " + created.error().detail);
        }
        return registry.getShared(*created);
    }

    // X4: owning handle; callers bind `auto session = env.sessionOf(*agent);`.
    std::shared_ptr<Session> sessionOf(const Agent& agent) {
        return sessions.sessionPtr(agent.session());
    }

    TempWorkspace workspace;
    EventBus bus;
    MemorySessionStore store;
    SessionManager sessions;
    LocalEnvironment env;
    ResourceGovernor governor;
    OutputRing ring{1u << 20};
    RingOutputSink sink{ring};
    NullLogger logger;
    ToolRegistry tools;
    RegistrationKeeper keeper{tools};
    std::shared_ptr<LLMProvider> provider;
    LlmRuntime runtime;
    std::optional<AdapterHandle> adapter_handle;
    RulePermissionPolicy policy;
    std::unique_ptr<PermissionGate> gate;
    SessionContextAssembler assembler;
    DefaultTokenEstimator estimator;
    LLMPool pool;
    std::unique_ptr<ContextCompactor> context_compactor;
    std::optional<PlanModeController> plan_mode_controller;
    AgentRegistry registry;
};

} // namespace ymh::test
