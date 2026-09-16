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
#include "ymh/execution/environment.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/tool_registry.hpp"

#include "test_env.hpp"

namespace ymh::test {

inline PermissionConfig allow_all_permission_config() {
    PermissionConfig config;
    config.default_verdict = PolicyVerdict::Allow;
    return config;
}

inline AgentServices make_agent_services(SessionManager& sessions,
                                         ResourceGovernor& governor,
                                         ToolRegistry& tools,
                                         PermissionPolicy& policy,
                                         PermissionGate* gate,
                                         ContextAssembler& context,
                                         ExecutionEnvironment& execution,
                                         Logger& logger,
                                         OutputSink& output,
                                         LLMProvider& provider,
                                         LLMPool& pool,
                                         TokenEstimator& estimator,
                                         AgentServices::PermissionResolver resolver,
                                         Compactor* compactor,
                                         ContextCompactor* context_compactor = nullptr) {
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
    services.provider            = &provider;
    services.pool                = &pool;
    services.estimator           = &estimator;
    services.compactor           = compactor;
    services.context_compactor   = context_compactor;
    services.permission_resolver = std::move(resolver);
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
             WallClock wall_clock = std::chrono::system_clock::now)
        : workspace(prefix),
          sessions(store, bus),
          env(workspace.path(), SandboxMode::Workspace, ToolConfig{}),
          provider(std::move(provider)),
          policy(std::move(permission)),
          gate(use_permission_gate ? std::make_unique<PermissionGate>(policy, PermissionConfig{})
                                   : nullptr),
          assembler(tools, config.system_prompt),
          pool(pool_capacity),
          context_compactor(compaction.has_value()
                                ? std::make_unique<ContextCompactor>(*this->provider, pool, estimator,
                                                                     *compaction,
                                                                     std::move(wall_clock))
                                : nullptr),
          registry(make_agent_services(sessions, governor, tools, policy, gate.get(), assembler, env,
                                        logger, sink, *this->provider, pool, estimator,
                                        std::move(resolver), compactor, context_compactor.get()),
                   std::move(config)) {
        if (register_builtins) {
            for (std::unique_ptr<Tool>& tool : make_builtin_tools()) {
                keeper.add(std::move(tool));
            }
            tools.freeze();
        }
    }

    Agent& createAgent() {
        SessionOptions options;
        options.cwd           = workspace.path();
        options.serverProfile = "interactive";
        options.model         = "fake-model";
        options.title         = "test";
        const std::expected<AgentId, AgentError> created = registry.create(options);
        if (!created.has_value()) {
            throw std::runtime_error("create failed: " + created.error().detail);
        }
        return registry.get(*created);
    }

    Session& sessionOf(const Agent& agent) { return sessions.session(agent.session()); }

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
    std::unique_ptr<LLMProvider> provider;
    RulePermissionPolicy policy;
    std::unique_ptr<PermissionGate> gate;
    SessionContextAssembler assembler;
    DefaultTokenEstimator estimator;
    LLMPool pool;
    std::unique_ptr<ContextCompactor> context_compactor;
    AgentRegistry registry;
};

} // namespace ymh::test
