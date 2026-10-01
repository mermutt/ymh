#include "ymh/mcp/mcp_client.hpp"

#include "ymh/core/version.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace ymh {
namespace {

std::string rpc_error_text(const nlohmann::json& error) {
    if (!error.is_object()) {
        return "malformed error";
    }
    std::string text;
    if (error.contains("code") && error["code"].is_number_integer()) {
        text += std::to_string(error["code"].get<std::int64_t>());
    }
    const std::string message =
        error.contains("message") && error["message"].is_string()
            ? error["message"].get<std::string>()
            : std::string{};
    if (!message.empty()) {
        if (!text.empty()) {
            text += " ";
        }
        text += message;
    }
    return text.empty() ? "unspecified error" : text;
}

} // namespace

bool is_supported_mcp_revision(std::string_view revision) noexcept {
    return revision == "2025-06-18" || revision == "2025-03-26";
}

std::string_view newest_mcp_revision() noexcept { return "2025-06-18"; }

McpClient::CallSlot::CallSlot(std::function<void()> release) noexcept
    : release_(std::move(release)) {}

McpClient::CallSlot::CallSlot(CallSlot&& other) noexcept
    : release_(std::move(other.release_)) {
    other.release_ = {};
}

McpClient::CallSlot& McpClient::CallSlot::operator=(CallSlot&& other) noexcept {
    if (this != &other) {
        if (release_) {
            release_();
        }
        release_ = std::move(other.release_);
        other.release_ = {};
    }
    return *this;
}

McpClient::CallSlot::~CallSlot() {
    if (release_) {
        release_();
    }
}

class DefaultMcpClient final : public McpClient {
public:
    DefaultMcpClient(McpServerConfig config,
                     McpConfig mcp_config,
                     std::unique_ptr<McpTransport> transport,
                     Logger& logger,
                     McpClockReader now)
        : id_(config.id),
          config_(std::move(config)),
          mcp_config_(std::move(mcp_config)),
          transport_(std::move(transport)),
          logger_(logger),
          now_(std::move(now)) {
        status_.id = id_;
        pollable_ = dynamic_cast<McpPollableTransport*>(transport_.get());
        diagnostic_ = dynamic_cast<McpDiagnosticTransport*>(transport_.get());
        transport_->setMessageHandler(
            [this](nlohmann::json message) { handleMessage(std::move(message)); });
        transport_->setCloseHandler(
            [this](McpDisconnectReason reason) { onClose(reason); });
    }

    const McpServerId& id() const noexcept override { return id_; }

    McpServerState state() const noexcept override { return state_.load(); }

    Task<void> start(CancellationToken cancel) override {
        try {
            return startImpl(cancel);
        } catch (const McpError& error) {
            throw McpError{error.code(), std::string{error.what()} + failure_suffix()};
        }
    }

    Task<std::vector<McpToolInfo>> listTools(CancellationToken cancel) override {
        try {
            return listToolsImpl(cancel);
        } catch (const McpError& error) {
            throw McpError{error.code(), std::string{error.what()} + failure_suffix()};
        }
    }

    Task<void> startImpl(CancellationToken cancel) {
        if (state_.load() == McpServerState::Ready) {
            return Task<void>{};
        }
        state_.store(McpServerState::Starting);
        setStatusState(McpServerState::Starting);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = false;
        }

        transport_->start(cancel).get();

        nlohmann::json params = {
            {"protocolVersion",
             config_.protocol_version.empty() ? std::string{newest_mcp_revision()}
                                              : config_.protocol_version},
            {"capabilities", nlohmann::json::object()},
            {"clientInfo", {{"name", "ymh"}, {"version", YMH_VERSION}}},
        };

        nlohmann::json response;
        try {
            response = sendRequest("initialize", std::move(params),
                                   mcp_config_.handshake_timeout, cancel);
        } catch (const McpError& error) {
            fail();
            if (error.code() == McpErrorCode::CallTimeout) {
                throw McpError{McpErrorCode::HandshakeTimeout,
                               "initialize timed out (handshake_timeout=" +
                                   std::to_string(mcp_config_.handshake_timeout.count()) +
                                   "ms)"};
            }
            throw;
        }
        if (response.contains("error")) {
            fail();
            throw McpError{McpErrorCode::HandshakeRejected,
                           "initialize was rejected: " + rpc_error_text(response["error"])};
        }
        if (!response.contains("result") || !response["result"].is_object()) {
            fail();
            throw McpError{McpErrorCode::ProtocolViolation,
                           "initialize had no result: " +
                               mcp_truncate_utf8(response.dump(), 200)};
        }
        const nlohmann::json& result = response["result"];
        const std::string revision =
            result.contains("protocolVersion") && result["protocolVersion"].is_string()
                ? result["protocolVersion"].get<std::string>()
                : std::string{};
        if (!is_supported_mcp_revision(revision)) {
            fail();
            throw McpError{McpErrorCode::HandshakeRejected,
                           "unsupported protocol revision: " +
                               (revision.empty() ? std::string{"<absent>"} : revision)};
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            protocol_version_ = revision;
            status_.protocol_version = revision;
            if (result.contains("serverInfo") && result["serverInfo"].is_object()) {
                const nlohmann::json& info = result["serverInfo"];
                if (info.contains("name") && info["name"].is_string()) {
                    status_.server_name = info["name"].get<std::string>();
                }
                if (info.contains("version") && info["version"].is_string()) {
                    status_.server_version = info["version"].get<std::string>();
                }
            }
        }

        nlohmann::json initialized = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/initialized"},
        };
        transport_->send(initialized, cancel).get();

        state_.store(McpServerState::Ready);
        setStatusState(McpServerState::Ready);
        return Task<void>{};
    }

    Task<std::vector<McpToolInfo>> listToolsImpl(CancellationToken cancel) {
        const McpServerState current = state_.load();
        if (current != McpServerState::Ready && current != McpServerState::Degraded) {
            throw McpError{McpErrorCode::TransportClosed, "tools/list before ready"};
        }
        std::vector<McpToolInfo> tools;
        std::optional<std::string> cursor;
        for (std::size_t page = 0; page < mcp_config_.list_max_pages; ++page) {
            nlohmann::json params = nlohmann::json::object();
            if (cursor.has_value()) {
                params["cursor"] = *cursor;
            }
            nlohmann::json response =
                sendRequest("tools/list", std::move(params), mcp_config_.list_timeout, cancel);
            if (response.contains("error")) {
                throw McpError{McpErrorCode::RpcError,
                               "tools/list error: " + rpc_error_text(response["error"])};
            }
            if (!response.contains("result") || !response["result"].is_object()) {
                throw McpError{McpErrorCode::ProtocolViolation,
                               "tools/list had no result: " +
                                   mcp_truncate_utf8(response.dump(), 200)};
            }
            const nlohmann::json& result = response["result"];
            if (result.contains("tools") && result["tools"].is_array()) {
                for (const nlohmann::json& entry : result["tools"]) {
                    if (!entry.is_object()) {
                        continue;
                    }
                    McpToolInfo info;
                    info.remote_name.value = entry.value("name", std::string{});
                    info.description = entry.value("description", std::string{});
                    if (entry.contains("inputSchema")) {
                        info.input_schema = entry["inputSchema"];
                    }
                    if (entry.contains("annotations") && entry["annotations"].is_object()) {
                        info.destructive =
                            entry["annotations"].value("destructiveHint", false);
                        if (entry["annotations"].contains("title") &&
                            entry["annotations"]["title"].is_string()) {
                            info.title =
                                entry["annotations"]["title"].get<std::string>();
                        }
                    }
                    tools.push_back(std::move(info));
                }
            }
            cursor.reset();
            if (result.contains("nextCursor") && result["nextCursor"].is_string()) {
                cursor = result["nextCursor"].get<std::string>();
            }
            if (!cursor.has_value()) {
                break;
            }
        }
        return Task<std::vector<McpToolInfo>>(std::move(tools));
    }

    Task<McpCallResult> callTool(std::string_view remote_tool,
                                 const nlohmann::json& arguments,
                                 const McpCallOptions& options,
                                 CancellationToken cancel) override {
        if (state_.load() != McpServerState::Ready) {
            throw McpError{McpErrorCode::TransportClosed, "server is not ready"};
        }
        nlohmann::json params = {
            {"name", std::string(remote_tool)},
            {"arguments", arguments},
        };
        const std::chrono::milliseconds configured =
            options.timeout.count() > 0 ? options.timeout : config_.call_timeout;
        std::chrono::milliseconds timeout = configured;
        if (options.deadline.has_value()) {
            if (options.deadline->count() == 0) {
                throw McpError{McpErrorCode::CallTimeout, "deadline expired"};
            }
            timeout = std::min(configured, *options.deadline);
        }
        nlohmann::json response =
            sendRequest("tools/call", std::move(params), timeout, cancel);
        if (response.contains("error")) {
            throw McpError{McpErrorCode::RpcError, "tools/call returned an error"};
        }
        if (!response.contains("result") || !response["result"].is_object()) {
            throw McpError{McpErrorCode::ProtocolViolation, "tools/call had no result"};
        }
        const nlohmann::json& result = response["result"];
        McpCallResult call;
        call.content = result.value("content", nlohmann::json::array());
        call.is_error = result.value("isError", false);
        if (result.contains("structuredContent")) {
            call.structured_content = result["structuredContent"];
        }
        return Task<McpCallResult>(std::move(call));
    }

    std::optional<CallSlot> tryAcquireCallSlot() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inflight_ >= mcp_config_.max_inflight_calls_per_server) {
            return std::nullopt;
        }
        ++inflight_;
        return CallSlot([this] { releaseSlot(); });
    }

    Task<bool> ping() override {
        try {
            nlohmann::json response =
                sendRequest("ping", nlohmann::json::object(), mcp_config_.list_timeout, {});
            return Task<bool>{!response.contains("error")};
        } catch (const McpError&) {
            return Task<bool>{false};
        }
    }

    Task<void> shutdown(std::chrono::milliseconds grace) override {
        if (state_.load() == McpServerState::Stopped) {
            return Task<void>{};
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            for (auto& [id, pending] : pending_) {
                (void)id;
                pending.done = true;
                pending.closed = true;
            }
        }
        try {
            transport_->close(grace).get();
        } catch (const std::exception&) {
        }
        state_.store(McpServerState::Stopped);
        setStatusState(McpServerState::Stopped);
        return Task<void>{};
    }

    void setNotificationHandler(
        std::function<void(std::string_view, const nlohmann::json&)> handler,
        std::function<void()> on_tools_changed) override {
        notification_ = std::move(handler);
        tools_changed_ = std::move(on_tools_changed);
    }

    McpServerStatus status() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        McpServerStatus snapshot = status_;
        snapshot.state = state_.load();
        return snapshot;
    }

private:
    struct Pending {
        bool           done = false;
        bool           closed = false;
        nlohmann::json message;
    };

    void fail() {
        state_.store(McpServerState::Failed);
        setStatusState(McpServerState::Failed);
    }

    void setStatusState(McpServerState state) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.state = state;
    }

    void releaseSlot() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (inflight_ > 0) {
            --inflight_;
        }
    }

    void onClose(McpDisconnectReason reason) {
        state_.store(McpServerState::Disconnected);
        setStatusState(McpServerState::Disconnected);
        std::lock_guard<std::mutex> lock(mutex_);
        disconnect_reason_ = reason;
        for (auto& [id, pending] : pending_) {
            (void)id;
            pending.done = true;
            pending.closed = true;
        }
    }

    std::string disconnect_suffix() const {
        if (!disconnect_reason_.has_value()) {
            return {};
        }
        return " (" + std::string{mcp_disconnect_token(*disconnect_reason_)} + ")";
    }

    std::string failure_suffix() {
        if (diagnostic_ == nullptr) {
            return {};
        }
        const std::string context = diagnostic_->failureContext();
        if (context.empty()) {
            return {};
        }
        std::string out = "\nrecent server output:";
        std::size_t start = 0;
        for (std::size_t index = 0; index <= context.size(); ++index) {
            if (index != context.size() && context[index] != '\n') {
                continue;
            }
            out += "\n  " + context.substr(start, index - start);
            start = index + 1;
        }
        return out;
    }

    void sendCancelled(std::int64_t id) noexcept {
        nlohmann::json notification = {
            {"jsonrpc", "2.0"},
            {"method", "notifications/cancelled"},
            {"params", {{"requestId", id}, {"reason", "cancelled"}}},
        };
        try {
            transport_->send(notification, {}).get();
        } catch (const std::exception&) {
        }
    }

    nlohmann::json sendRequest(std::string_view method,
                               nlohmann::json params,
                               std::chrono::milliseconds timeout,
                               CancellationToken cancel) {
        const std::int64_t id = next_id_++;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                throw McpError{McpErrorCode::TransportClosed, "client is stopped"};
            }
            pending_[id] = Pending{};
        }

        nlohmann::json message = {
            {"jsonrpc", "2.0"},
            {"id", id},
            {"method", std::string(method)},
        };
        if (!params.is_null()) {
            message["params"] = std::move(params);
        }
        transport_->send(message, cancel).get();

        const McpClock::time_point deadline = now_() + timeout;
        for (;;) {
            if (cancel.cancelled()) {
                sendCancelled(id);
                erasePending(id);
                throw McpError{McpErrorCode::Cancelled, "request cancelled"};
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                const auto found = pending_.find(id);
                if (found == pending_.end()) {
                    throw McpError{McpErrorCode::TransportClosed,
                                   "request was abandoned" + disconnect_suffix()};
                }
                if (found->second.done) {
                    if (found->second.closed) {
                        pending_.erase(found);
                        throw McpError{McpErrorCode::TransportClosed,
                                       "transport closed" + disconnect_suffix()};
                    }
                    nlohmann::json response = std::move(found->second.message);
                    pending_.erase(found);
                    return response;
                }
            }
            if (now_() >= deadline) {
                sendCancelled(id);
                erasePending(id);
                throw McpError{McpErrorCode::CallTimeout, "request timed out"};
            }
            if (pollable_ == nullptr) {
                erasePending(id);
                throw McpError{McpErrorCode::ProtocolViolation,
                               "transport produced no response"};
            }
            const auto remaining = deadline - now_();
            const auto slice = std::min(
                std::chrono::duration_cast<std::chrono::milliseconds>(remaining),
                std::chrono::milliseconds{25});
            pollable_->poll(slice);
        }
    }

    void erasePending(std::int64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(id);
    }

    void handleMessage(nlohmann::json message) {
        if (!message.is_object()) {
            logger_.warn("mcp: dropping non-object frame");
            return;
        }
        if (message.contains("method") && message["method"].is_string()) {
            const std::string method = message["method"].get<std::string>();
            const nlohmann::json params =
                message.value("params", nlohmann::json::object());
            const bool is_request = message.contains("id") && !message["id"].is_null();
            if (is_request) {
                nlohmann::json error = {
                    {"jsonrpc", "2.0"},
                    {"id", message["id"]},
                    {"error", {{"code", -32601}, {"message", "Method not found"}}},
                };
                try {
                    transport_->send(error, {}).get();
                } catch (const std::exception&) {
                }
                logger_.warn("mcp: rejected server-initiated request");
                return;
            }
            if (method == "notifications/tools/list_changed") {
                if (tools_changed_) {
                    tools_changed_();
                }
                return;
            }
            if (method == "notifications/message") {
                logger_.debug("mcp: server notification/message (redacted)");
                return;
            }
            if (notification_) {
                notification_(method, params);
            }
            return;
        }
        if (!message.contains("id") || !message["id"].is_number_integer()) {
            logger_.warn("mcp: dropping frame without id or method");
            return;
        }
        const std::int64_t id = message["id"].get<std::int64_t>();
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = pending_.find(id);
        if (found == pending_.end()) {
            logger_.warn("mcp: dropping response with unknown id");
            return;
        }
        if (!message.contains("result") && !message.contains("error")) {
            logger_.warn("mcp: dropping response with neither result nor error");
            return;
        }
        found->second.done = true;
        found->second.message = std::move(message);
    }

    McpServerId                        id_;
    McpServerConfig                    config_;
    McpConfig                          mcp_config_;
    std::unique_ptr<McpTransport>      transport_;
    McpPollableTransport*              pollable_ = nullptr;
    McpDiagnosticTransport*            diagnostic_ = nullptr;
    Logger&                            logger_;
    McpClockReader                     now_;
    std::atomic<McpServerState>        state_{McpServerState::Disabled};

    mutable std::mutex                 mutex_;
    std::optional<McpDisconnectReason> disconnect_reason_;
    McpServerStatus                    status_;
    std::map<std::int64_t, Pending>    pending_;
    std::int64_t                       next_id_{1};
    std::size_t                        inflight_ = 0;
    bool                               stopped_ = false;
    std::string                        protocol_version_;

    std::function<void(std::string_view, const nlohmann::json&)> notification_;
    std::function<void()>                                        tools_changed_;
};

std::unique_ptr<McpClient> make_stdio_mcp_client(const McpServerConfig& config,
                                                 McpConfig& mcp_config,
                                                 ExecutionEnvironment& environment,
                                                 ResourceGovernor& governor,
                                                 Logger& logger) {
    (void)governor;
    auto transport =
        std::make_unique<StdioMcpTransport>(config, mcp_config, environment, logger);
    return make_mcp_client_with_transport(config, mcp_config, std::move(transport), logger);
}

std::unique_ptr<McpClient> make_mcp_client_with_transport(
    const McpServerConfig& config,
    McpConfig& mcp_config,
    std::unique_ptr<McpTransport> transport,
    Logger& logger,
    McpClockReader now) {
    return std::make_unique<DefaultMcpClient>(config, mcp_config, std::move(transport),
                                              logger, std::move(now));
}

} // namespace ymh
