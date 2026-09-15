#include "ymh/cli/headless.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <unistd.h>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/context_assembler.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/output.hpp"
#include "ymh/execution/resource_governor.hpp"
#include "ymh/llm/fake_llm.hpp"
#include "ymh/policy/permission_policy.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/session/session_persistence.hpp"
#include "ymh/tools/builtin_tools.hpp"
#include "ymh/tools/tool_registry.hpp"

namespace ymh {
namespace {

Message user_message(std::string text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = std::move(text);
    message.content.push_back(std::move(block));
    return message;
}

std::string first_line(const std::string& text, std::size_t max_bytes) {
    std::string line = text;
    const std::size_t newline = line.find('\n');
    if (newline != std::string::npos) {
        line.resize(newline);
    }
    if (line.size() > max_bytes) {
        line.resize(max_bytes);
        line += "...";
    }
    return line;
}

void render_tool_result(std::ostream& out, const payload::ToolResult& result) {
    constexpr std::size_t kMaxBytes = 4000;
    std::string body = result.output;
    bool truncated = result.truncated;
    if (body.size() > kMaxBytes) {
        body.resize(kMaxBytes);
        truncated = true;
    }

    out << (result.outcome == payload::ToolOutcome::Ok ? "  -> " : "  !! ");
    if (body.empty()) {
        out << "(no output)";
    } else {
        bool first = true;
        std::size_t start = 0;
        while (start <= body.size()) {
            const std::size_t end = body.find('\n', start);
            const std::string_view line{body.data() + start,
                                        (end == std::string::npos ? body.size() : end) - start};
            if (!first) {
                out << "     ";
            }
            out << line;
            first = false;
            if (end == std::string::npos) {
                break;
            }
            out << '\n';
            start = end + 1;
        }
    }
    if (truncated) {
        out << "  ...[truncated]";
    }
    out << '\n';
}

FinishReason parse_finish(std::string_view name) {
    if (name == "stop") {
        return FinishReason::Stop;
    }
    if (name == "length") {
        return FinishReason::Length;
    }
    if (name == "tool_calls" || name == "tool_call") {
        return FinishReason::ToolCalls;
    }
    if (name == "content_filter") {
        return FinishReason::ContentFilter;
    }
    return FinishReason::Other;
}

std::optional<LLMError> parse_error(const nlohmann::json& node) {
    if (!node.is_object()) {
        return std::nullopt;
    }
    LLMError error;
    const std::string code = node.value("code", std::string{"provider_internal"});
    if (code == "auth") {
        error.code = LLMErrorCode::Auth;
    } else if (code == "config_error") {
        error.code = LLMErrorCode::ConfigError;
    } else if (code == "bad_request") {
        error.code = LLMErrorCode::BadRequest;
    } else if (code == "rate_limited") {
        error.code = LLMErrorCode::RateLimited;
    } else if (code == "server_error") {
        error.code = LLMErrorCode::ServerError;
    } else if (code == "network_error") {
        error.code = LLMErrorCode::NetworkError;
    } else if (code == "timeout") {
        error.code = LLMErrorCode::Timeout;
    } else if (code == "context_length_exceeded") {
        error.code = LLMErrorCode::ContextLengthExceeded;
    } else {
        error.code = LLMErrorCode::ProviderInternal;
    }
    error.detail = node.value("detail", std::string{});
    return error;
}

std::optional<FakeScript> parse_fake_script(const nlohmann::json& document) {
    const nlohmann::json* steps = nullptr;
    FakeScript             script;
    if (document.is_array()) {
        steps = &document;
    } else if (document.is_object() && document.contains("steps")) {
        steps = &document["steps"];
        script.chunk_size = document.value("chunk_size", script.chunk_size);
    } else {
        return std::nullopt;
    }
    if (!steps->is_array()) {
        return std::nullopt;
    }

    for (const nlohmann::json& step : *steps) {
        FakeResponseStep response;
        response.text     = step.value("text", std::string{});
        response.reasoning = step.contains("reasoning")
                                 ? std::optional<std::string>{step["reasoning"].get<std::string>()}
                                 : std::nullopt;
        response.finish = parse_finish(step.value("finish", std::string{"stop"}));
        if (step.contains("usage") && step["usage"].is_object()) {
            Usage usage;
            usage.input_tokens  = step["usage"].value("input_tokens", std::int64_t{0});
            usage.output_tokens = step["usage"].value("output_tokens", std::int64_t{0});
            response.usage      = usage;
        }
        if (step.contains("error")) {
            response.error = parse_error(step["error"]);
        }
        if (step.contains("tool_calls") && step["tool_calls"].is_array()) {
            for (const nlohmann::json& call : step["tool_calls"]) {
                FakeToolCallStep tool;
                tool.name      = call.value("name", std::string{});
                tool.arguments = call.value("arguments", nlohmann::json::object());
                if (call.contains("id")) {
                    tool.id = call["id"].get<std::string>();
                }
                response.tool_calls.push_back(std::move(tool));
            }
            if (response.finish == FinishReason::Stop) {
                response.finish = FinishReason::ToolCalls;
            }
        }
        script.steps.push_back(std::move(response));
    }
    return script;
}

std::optional<FakeScript> fake_script_from_env() {
    const std::optional<std::string> path = env_value("YMH_FAKE_LLM_SCRIPT");
    if (!path.has_value()) {
        return std::nullopt;
    }
    std::ifstream input{*path, std::ios::binary};
    if (!input) {
        throw ConfigError("YMH_FAKE_LLM_SCRIPT cannot be opened: " + *path);
    }
    const nlohmann::json document = nlohmann::json::parse(input);
    std::optional<FakeScript> script = parse_fake_script(document);
    if (!script.has_value()) {
        throw ConfigError("YMH_FAKE_LLM_SCRIPT is not a valid FakeLLM script: " + *path);
    }
    return script;
}

} // namespace

HeadlessResult run_headless(const HeadlessOptions& options) {
    HeadlessResult result;
    std::ostream&  out = options.out != nullptr ? *options.out : std::cout;
    std::ostream&  err = options.err != nullptr ? *options.err : std::cerr;

    std::error_code error;
    const std::filesystem::path root = std::filesystem::canonical(options.workspace, error);
    if (error || !std::filesystem::is_directory(root)) {
        err << "ymh: workspace not found: " << options.workspace << '\n';
        result.exit_code = 2;
        return result;
    }

    if (options.task.empty()) {
        err << "ymh: run requires a non-empty task\n";
        result.exit_code = 2;
        return result;
    }

    PersistenceConfig persistence;
    persistence.db_path   = root / ".ymh" / "sessions.db";
    persistence.lock_path = root / ".ymh" / "sessions.lock";
    persistence.boot_id   = BootId{make_boot_id()};

    std::unique_ptr<SessionPersistence> store;
    try {
        store = SessionPersistence::open(persistence);
    } catch (const std::exception& open_error) {
        err << "ymh: cannot open session store: " << open_error.what() << '\n';
        result.exit_code = 2;
        return result;
    }

    EventBus        bus;
    SessionManager  sessions(*store, bus);
    const ToolConfig tool_config;
    LocalEnvironment environment(root, SandboxMode::Workspace, tool_config);
    ResourceGovernor governor;
    ToolRegistry     tools;
    std::vector<ToolRegistry::Registration> registrations;
    for (std::unique_ptr<Tool>& tool : make_builtin_tools(tool_config)) {
        registrations.push_back(tools.add(std::move(tool)));
    }
    tools.freeze();

    RulePermissionPolicy   policy(to_permission_config(options.config));
    const AgentConfig      agent_config = to_agent_config(options.config);
    SessionContextAssembler assembler(tools, agent_config.system_prompt);
    DefaultTokenEstimator  estimator;
    ProviderRegistry       providers = make_default_provider_registry();
    LLMProviderConfig      provider_config = to_provider_config(options.config);

    OutputRing    ring(governor.caps().session_output_ring_bytes);
    RingOutputSink sink(ring);

    AgentServices services;
    services.sessions        = &sessions;
    services.governor        = &governor;
    services.tools           = &tools;
    services.policy          = &policy;
    services.context         = &assembler;
    services.execution       = &environment;
    services.logger          = &category_logger(LogCategory::Tool);
    services.output          = &sink;
    services.estimator       = &estimator;
    services.providers       = &providers;
    services.provider_config = provider_config;

    std::unique_ptr<LLMProvider> injected;
    try {
        if (options.provider_factory) {
            injected = options.provider_factory(provider_config);
        } else if (std::optional<FakeScript> script = fake_script_from_env(); script.has_value()) {
            injected = std::make_unique<FakeLLM>(std::move(*script));
        }
    } catch (const std::exception& factory_error) {
        err << "ymh: provider setup failed: " << factory_error.what() << '\n';
        result.exit_code = 2;
        return result;
    }
    if (injected != nullptr) {
        services.provider = injected.get();
    }

    AgentRegistry registry(services, agent_config);

    AgentId agent_id;
    if (options.resume.has_value()) {
        std::expected<AgentId, AgentError> resumed = registry.resume(*options.resume);
        if (!resumed.has_value()) {
            err << "ymh: cannot resume session " << options.resume->value << ": "
                << resumed.error().detail << '\n';
            result.exit_code = 2;
            return result;
        }
        agent_id = *resumed;
    } else {
        SessionOptions session_options;
        session_options.cwd           = root;
        session_options.serverProfile = "automation";
        session_options.model         = agent_config.model;
        session_options.title         = first_line(options.task, 60);
        std::expected<AgentId, AgentError> created = registry.create(session_options);
        if (!created.has_value()) {
            err << "ymh: cannot create session: " << created.error().detail << '\n';
            result.exit_code = 2;
            return result;
        }
        agent_id = *created;
    }

    Agent& agent   = registry.get(agent_id);
    result.session = agent.session();

    try {
        if (!store->acquireLease(result.session)) {
            err << "ymh: session " << result.session.value
                << " is locked by another writer\n";
            result.exit_code = 2;
            return result;
        }
    } catch (const std::exception& lease_error) {
        err << "ymh: cannot acquire session lease: " << lease_error.what() << '\n';
        result.exit_code = 2;
        return result;
    }

    category_logger(LogCategory::Agent)
        .info("run session=" + result.session.value + " model=" + agent_config.model +
              " provider=" + provider_config.provider);

    std::string terminal;
    Subscription subscription = bus.subscribe([&](const Event& event) {
        if (event.session_id.value != result.session.value) {
            return;
        }
        switch (event.type) {
            case EventType::AssistantChunk: {
                const auto& chunk = event.payload.get<payload::AssistantChunk>();
                if (chunk.kind == payload::AssistantChunkKind::Text) {
                    out << chunk.text;
                    out.flush();
                    result.assistant_text += chunk.text;
                } else if (options.verbose) {
                    err << chunk.text;
                    err.flush();
                }
                break;
            }
            case EventType::ToolCall: {
                const auto& call = event.payload.get<payload::ToolCall>();
                out << "\n  \u25cf " << call.name << '(' << call.arguments.dump() << ")\n";
                break;
            }
            case EventType::ToolResult: {
                render_tool_result(out, event.payload.get<payload::ToolResult>());
                break;
            }
            case EventType::TurnFailed: {
                const auto& failed = event.payload.get<payload::TurnFailed>();
                terminal = "turn/fail";
                err << "\nymh: turn failed [" << failed.code << "]: " << failed.message << '\n';
                break;
            }
            case EventType::TurnCancelled: {
                terminal = "turn/cancel";
                err << "\nymh: turn cancelled\n";
                break;
            }
            case EventType::TurnEnded: {
                terminal = "turn/end";
                break;
            }
            default:
                break;
        }
    });

    std::atomic<bool> finished{false};
    std::thread worker([&]() {
        agent.send(user_message(options.task));
        finished.store(true);
    });

    while (!finished.load()) {
        if (options.cancel_poll && options.cancel_poll()) {
            agent.cancel();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    worker.join();

    out << '\n';
    subscription.unsubscribe();

    result.terminal = terminal;
    if (terminal == "turn/fail") {
        result.exit_code = 1;
    } else if (terminal == "turn/cancel") {
        result.exit_code = 130;
    } else {
        result.exit_code = 0;
    }

    registry.dispose(agent_id);
    try {
        store->releaseLease(result.session);
    } catch (const std::exception&) {
    }
    return result;
}

} // namespace ymh
