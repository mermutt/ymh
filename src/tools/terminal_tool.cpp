#include "ymh/tools/terminal_tool.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "ymh/execution/deadline.hpp"
#include "ymh/execution/path_safety.hpp"
#include "ymh/execution/pty.hpp"
#include "ymh/tools/tool_context.hpp"

namespace ymh {
namespace {

constexpr std::size_t kDefaultReadBytes = 64 * 1024;
constexpr auto        kMaxReadWait = std::chrono::milliseconds{30'000};

nlohmann::json string_type() { return nlohmann::json{{"type", "string"}}; }
nlohmann::json integer_type() { return nlohmann::json{{"type", "integer"}}; }
nlohmann::json boolean_type() { return nlohmann::json{{"type", "boolean"}}; }
nlohmann::json string_array_type() {
    return nlohmann::json{{"type", "array"}, {"items", string_type()}};
}

nlohmann::json terminal_schema() {
    return nlohmann::json{
        {"type", "object"},
        {"properties",
         {{"action", string_type()},
          {"command", string_type()},
          {"argv", string_array_type()},
          {"cwd", string_type()},
          {"rows", integer_type()},
          {"cols", integer_type()},
          {"term", string_type()},
          {"terminal_id", string_type()},
          {"input", string_type()},
          {"append_newline", boolean_type()},
          {"wait_ms", integer_type()},
          {"max_bytes", integer_type()},
          {"signal", string_type()}}},
        {"required", nlohmann::json::array({"action"})},
        {"additionalProperties", false}};
}

class TerminalTool final : public Tool {
public:
    explicit TerminalTool(ToolConfig config) : config_(config) {}

    ToolSchema schema() const override {
        return ToolSchema{
            ToolName{"terminal"},
            ToolVersion{1, 0},
            "Drive a persistent pseudo-terminal: open, write, read, resize, close, list.",
            terminal_schema(),
            true};
    }

    Task<ToolResult> execute(const ToolContext& context,
                             const ToolArguments& arguments) override {
        const std::string action = arguments.value.at("action").get<std::string>();
        try {
            if (action == "open") {
                return openAction(context, arguments);
            }
            if (action == "write") {
                return writeAction(context, arguments);
            }
            if (action == "read") {
                return readAction(context, arguments);
            }
            if (action == "resize") {
                return resizeAction(context, arguments);
            }
            if (action == "close") {
                return closeAction(context, arguments);
            }
            if (action == "list") {
                return listAction(context, arguments);
            }
        } catch (const PtyError& error) {
            throw ToolError{to_tool_error_code(error.code()),
                            std::string(to_string(error.code()))};
        }
        throw ToolError{ToolErrorCode::InvalidArguments,
                        "unknown terminal action: " + action};
    }

private:
    ToolResult makeResult(const ToolContext& context) const {
        ToolResult result;
        result.id = context.callId();
        result.name = "terminal";
        return result;
    }

    PtySession* resolveSession(const ToolContext& context,
                               const ToolArguments& arguments) const {
        if (!arguments.value.contains("terminal_id") ||
            !arguments.value["terminal_id"].is_string()) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "terminal_id must be a string"};
        }
        const std::string id_text =
            arguments.value["terminal_id"].get<std::string>();
        std::uint64_t value = 0;
        try {
            value = std::stoull(id_text);
        } catch (const std::exception&) {
            throw PtyError{PtyErrorCode::NotFound, "invalid terminal id"};
        }
        PtySession* session =
            context.execution().pty().find(PtySessionId{value});
        if (session == nullptr || session->session() != context.sessionId()) {
            throw PtyError{PtyErrorCode::NotFound, "unknown terminal id"};
        }
        return session;
    }

    Task<ToolResult> openAction(const ToolContext& context,
                                const ToolArguments& arguments) {
        const bool has_command = arguments.value.contains("command");
        const bool has_argv = arguments.value.contains("argv");
        if (has_command == has_argv) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "open requires exactly one of command or argv"};
        }

        auto confinement = std::make_shared<SpawnConfinement>(
            context.root(), context.execution().mode());

        PtyRequest request;
        if (has_command) {
            request.executable = "/bin/bash";
            request.argv = {"/bin/bash", "--noprofile", "--norc", "-lc",
                            arguments.value["command"].get<std::string>()};
        } else {
            const auto argv = arguments.value["argv"].get<std::vector<std::string>>();
            if (argv.empty()) {
                throw ToolError{ToolErrorCode::InvalidArguments, "empty argv"};
            }
            request.executable = argv.front();
            request.argv = argv;
        }

        const std::filesystem::path& tmp_root = canonical_tmp_root();
        std::error_code              root_error;
        std::filesystem::path        canonical_root =
            std::filesystem::weakly_canonical(context.root(), root_error);
        if (root_error) {
            canonical_root = context.root().lexically_normal();
        }
        const bool workspace_under_tmp =
            !tmp_root.empty() && (canonical_root == tmp_root ||
                                  path_is_under_tmp_root(tmp_root, canonical_root));
        if (arguments.value.contains("cwd")) {
            const std::string cwd_text = arguments.value["cwd"].get<std::string>();
            const std::filesystem::path raw{cwd_text};
            const std::filesystem::path candidate =
                (raw.is_absolute() ? raw : (context.root() / raw)).lexically_normal();
            std::error_code error;
            const std::filesystem::path canonical =
                std::filesystem::weakly_canonical(candidate, error);
            if (!workspace_under_tmp && !tmp_root.empty() && !error &&
                path_is_under_tmp_root(tmp_root, canonical)) {
                request.cwd = canonical;
            } else {
                request.cwd = context.resolve(cwd_text);
            }
        } else {
            request.cwd = context.root();
        }
        // 90-D22/HIGH-1: canonicalize independently of `resolve()` (which
        // skips containment under Unrestricted) and reject an outside or
        // protected cwd before opening the PTY.
        {
            std::error_code error;
            const std::filesystem::path canonical =
                std::filesystem::weakly_canonical(request.cwd, error);
            const std::filesystem::path checked =
                error ? request.cwd.lexically_normal() : canonical;
            const bool temp =
                !workspace_under_tmp && !tmp_root.empty() &&
                path_is_under_tmp_root(tmp_root, checked);
            if (!temp &&
                (!path_is_within(checked, context.root()) ||
                 path_is_protected(context.root(), checked))) {
                throw ToolError{ToolErrorCode::PathEscape,
                                "terminal cwd escapes workspace root"};
            }
            if (temp && !std::filesystem::is_directory(checked, error)) {
                throw ToolError{ToolErrorCode::PathEscape,
                                "terminal temp cwd is not a directory"};
            }
        }
        request.environment = {{"HOME", confinement->home().string()},
                               {"TMPDIR", confinement->tmpdir().string()},
                               {"PWD", request.cwd.string()}};
        if (context.execution().confinementStatus() == ConfinementStatus::Enforced) {
            request.confinement = &confinement->policy();
        }
        if (arguments.value.contains("rows")) {
            request.rows = arguments.value["rows"].get<int>();
        }
        if (arguments.value.contains("cols")) {
            request.cols = arguments.value["cols"].get<int>();
        }
        if (arguments.value.contains("term")) {
            request.term = arguments.value["term"].get<std::string>();
        }
        request.session = context.sessionId();

        std::unique_ptr<PtySession> opened =
            context.execution().pty().open(request, context.cancellation()).get();
        const PtySessionId id = opened->id();
        const int pid = opened->pid();
        confinements_[id.value] = std::move(confinement);

        ToolResult result = makeResult(context);
        result.output = nlohmann::json{{"terminal_id", std::to_string(id.value)},
                                       {"pid", pid},
                                       {"rows", request.rows},
                                       {"cols", request.cols}}
                            .dump();
        return Task<ToolResult>(std::move(result));
    }

    Task<ToolResult> writeAction(const ToolContext& context,
                                 const ToolArguments& arguments) {
        PtySession* session = resolveSession(context, arguments);
        if (!arguments.value.contains("input") ||
            !arguments.value["input"].is_string()) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "input must be a string"};
        }
        std::string input = arguments.value["input"].get<std::string>();
        const bool append_newline = !arguments.value.contains("append_newline") ||
                                    arguments.value["append_newline"].get<bool>();
        if (append_newline) {
            input.push_back('\n');
        }
        session->write(input);

        ToolResult result = makeResult(context);
        result.output = nlohmann::json{{"queued", input.size()}}.dump();
        return Task<ToolResult>(std::move(result));
    }

    Task<ToolResult> readAction(const ToolContext& context,
                                const ToolArguments& arguments) {
        PtySession* session = resolveSession(context, arguments);

        std::chrono::milliseconds wait{0};
        if (arguments.value.contains("wait_ms")) {
            wait = std::chrono::milliseconds{
                arguments.value["wait_ms"].get<long long>()};
            wait = std::min(wait, kMaxReadWait);
            if (wait.count() < 0) {
                wait = std::chrono::milliseconds{0};
            }
        }
        std::size_t max_bytes = kDefaultReadBytes;
        if (arguments.value.contains("max_bytes")) {
            const long long requested = arguments.value["max_bytes"].get<long long>();
            if (requested > 0) {
                max_bytes = std::min<std::size_t>(
                    static_cast<std::size_t>(requested),
                    config_.tool_result_max_bytes);
            }
        }

        const std::optional<std::chrono::milliseconds> budget = clamp_timeout(
            context.has_deadline()
                ? std::optional<std::chrono::steady_clock::time_point>{context.deadline()}
                : std::nullopt,
            wait, std::chrono::steady_clock::now());
        PtyRead read =
            session->read(max_bytes, wait, budget, context.cancellation()).get();
        ToolResult result = makeResult(context);
        if (read.timed_out) {
            throw ToolError{ToolErrorCode::Timeout, "pty read timed out"};
        }
        if (read.cancelled) {
            result.outcome = payload::ToolOutcome::Cancelled;
            return Task<ToolResult>(std::move(result));
        }
        result.output = read.data;
        result.truncated = read.truncated;
        if (read.eof) {
            result.output += "\n[eof]";
        }
        return Task<ToolResult>(std::move(result));
    }

    Task<ToolResult> resizeAction(const ToolContext& context,
                                  const ToolArguments& arguments) {
        PtySession* session = resolveSession(context, arguments);
        if (!arguments.value.contains("rows") ||
            !arguments.value.contains("cols")) {
            throw ToolError{ToolErrorCode::InvalidArguments,
                            "resize requires rows and cols"};
        }
        const int rows = arguments.value["rows"].get<int>();
        const int cols = arguments.value["cols"].get<int>();
        session->resize(rows, cols);

        ToolResult result = makeResult(context);
        result.output = nlohmann::json{{"rows", rows}, {"cols", cols}}.dump();
        return Task<ToolResult>(std::move(result));
    }

    Task<ToolResult> closeAction(const ToolContext& context,
                                 const ToolArguments& arguments) {
        PtySession* session = resolveSession(context, arguments);
        const bool force = arguments.value.contains("signal") &&
                           arguments.value["signal"].is_string() &&
                           arguments.value["signal"].get<std::string>() == "kill";
        if (force) {
            session->kill();
        } else {
            session->terminate();
        }
        const std::optional<std::chrono::milliseconds> budget = clamp_timeout(
            context.has_deadline()
                ? std::optional<std::chrono::steady_clock::time_point>{context.deadline()}
                : std::nullopt,
            std::chrono::milliseconds{0}, std::chrono::steady_clock::now());
        const PtyExit status = session->wait(std::chrono::milliseconds{0}, budget,
                                             context.cancellation())
                                   .get();
        if (status.timed_out) {
            throw ToolError{ToolErrorCode::Timeout, "pty wait timed out"};
        }

        if (arguments.value.contains("terminal_id") &&
            arguments.value["terminal_id"].is_string()) {
            try {
                confinements_.erase(
                    std::stoull(arguments.value["terminal_id"].get<std::string>()));
            } catch (const std::exception&) {
            }
        }

        ToolResult result = makeResult(context);
        result.output = nlohmann::json{{"exit_code", status.exit_code},
                                       {"signalled", status.signalled},
                                       {"signal", status.signal}}
                            .dump();
        return Task<ToolResult>(std::move(result));
    }

    Task<ToolResult> listAction(const ToolContext& context,
                                const ToolArguments& arguments) {
        (void)arguments;
        const std::vector<PtySessionId> ids =
            context.execution().pty().list(context.sessionId());
        nlohmann::json array = nlohmann::json::array();
        for (const PtySessionId id : ids) {
            array.push_back(std::to_string(id.value));
        }
        ToolResult result = makeResult(context);
        result.output = array.dump();
        return Task<ToolResult>(std::move(result));
    }

    ToolConfig config_;
    std::map<std::uint64_t, std::shared_ptr<SpawnConfinement>> confinements_;
};

} // namespace

std::unique_ptr<Tool> make_terminal_tool(ToolConfig config) {
    return std::make_unique<TerminalTool>(config);
}

} // namespace ymh
