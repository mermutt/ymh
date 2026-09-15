#include "ymh/cli/headless.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "ymh/agent/agent.hpp"
#include "ymh/agent/agent_registry.hpp"
#include "ymh/agent/message.hpp"
#include "ymh/agent/workspace_runtime.hpp"
#include "ymh/cli/provider_factory.hpp"
#include "ymh/cli/wiring.hpp"
#include "ymh/core/event_bus.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/session/events.hpp"
#include "ymh/session/session_manager.hpp"
#include "ymh/session/session_persistence.hpp"

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

    WorkspaceRuntimeOptions runtime_options;
    runtime_options.config  = options.config;
    runtime_options.root    = root;
    runtime_options.boot_id = mint_boot_id();
    runtime_options.provider_factory = make_provider_factory(options.provider_factory);

    std::expected<std::unique_ptr<WorkspaceRuntime>, WorkspaceRuntimeError> runtime_result =
        make_workspace_runtime(std::move(runtime_options));
    if (!runtime_result.has_value()) {
        const WorkspaceRuntimeError& runtime_error = runtime_result.error();
        if (runtime_error.code == WorkspaceRuntimeErrorCode::StoreUnavailable) {
            err << "ymh: cannot open session store: " << runtime_error.detail << '\n';
        } else if (runtime_error.code == WorkspaceRuntimeErrorCode::WorkspaceBusy) {
            err << "ymh: workspace is busy: another ymh process holds "
                << (root / ".ymh" / "sessions.lock")
                << "; stop the running daemon or choose a different workspace\n";
        } else if (runtime_error.code == WorkspaceRuntimeErrorCode::ProviderSetupFailed) {
            err << "ymh: provider setup failed: " << runtime_error.detail << '\n';
        } else {
            err << "ymh: cannot start workspace runtime: " << runtime_error.detail << '\n';
        }
        result.exit_code = 2;
        return result;
    }
    std::unique_ptr<WorkspaceRuntime> runtime      = std::move(*runtime_result);
    AgentRegistry&                    registry     = runtime->agents();
    const AgentConfig&                agent_config = runtime->agent_config();

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
        if (!runtime->acquireLease(result.session)) {
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
              " provider=" + runtime->provider_config().provider);

    std::string terminal;
    Subscription subscription = runtime->bus().subscribe([&](const Event& event) {
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
        runtime->releaseLease(result.session);
    } catch (const std::exception&) {
    }
    return result;
}

} // namespace ymh
