#include "ymh/mcp/mcp_manager.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <set>
#include <string>
#include <utility>

#include "ymh/config/config.hpp"
#include "ymh/mcp/mcp_tool.hpp"
#include "ymh/mcp/schema_translation.hpp"

namespace ymh {

std::chrono::milliseconds compute_mcp_backoff(std::uint32_t attempt,
                                              const McpConfig& config,
                                              double jitter_roll) {
    const double initial = static_cast<double>(config.reconnect_initial_backoff.count());
    const double maximum = static_cast<double>(config.reconnect_max_backoff.count());
    const double capped = std::min(initial * std::pow(2.0, static_cast<double>(attempt)), maximum);
    const double roll = std::clamp(jitter_roll, 0.0, 1.0);
    const double factor = 1.0 - config.reconnect_jitter + 2.0 * config.reconnect_jitter * roll;
    return std::chrono::milliseconds{static_cast<std::int64_t>(capped * factor)};
}

std::string validate_mcp_server(const McpServerConfig& server, const McpConfig& config,
                                const ToolConfig& tools) {
    if (server.transport == McpTransportKind::HttpSse) {
        if (server.enabled && !config.allow_network_servers) {
            return "http_sse requires allow_network_servers";
        }
        if (server.url.empty()) {
            return "http_sse server '" + server.id.value + "' has no url";
        }
    } else {
        if (server.command.empty()) {
            return "stdio server '" + server.id.value + "' has no command";
        }
        try {
            (void)resolve_mcp_env(server.env);
        } catch (const McpError& error) {
            return "server '" + server.id.value + "': " + error.what();
        }
    }
    if (server.max_result_bytes > tools.tool_result_max_bytes) {
        return "server '" + server.id.value + "' max_result_bytes exceeds tool_result_max_bytes";
    }
    return {};
}

std::string validate_mcp_config(const McpConfig& config, const ToolConfig& tools) {
    std::set<std::string> ids;
    for (const McpServerConfig& server : config.servers) {
        if (!is_valid_mcp_server_id(server.id.value)) {
            return "invalid server id '" + server.id.value + "'";
        }
        if (!ids.insert(server.id.value).second) {
            return "duplicate server id '" + server.id.value + "'";
        }
        const std::string reason = validate_mcp_server(server, config, tools);
        if (!reason.empty()) {
            return reason;
        }
    }
    return {};
}

McpManager::McpManager(McpConfig               config,
                       const ToolConfig&       tools,
                       ExecutionEnvironment&   environment,
                       ResourceGovernor&       governor,
                       ToolRegistry&           registry,
                       EventBus&               bus,
                       Logger&                 logger,
                       McpClockReader          now)
    : config_(std::move(config)),
      tool_config_(tools),
      environment_(environment),
      governor_(governor),
      registry_(registry),
      bus_(bus),
      logger_(logger),
      now_(std::move(now)) {
    factory_ = [](const McpServerConfig& server, McpConfig& mcp_config,
                  ExecutionEnvironment& env, ResourceGovernor& gov, Logger& log) {
        return make_stdio_mcp_client(server, mcp_config, env, gov, log);
    };
    validate();
    slots_.reserve(config_.servers.size());
    for (const McpServerConfig& server : config_.servers) {
        ServerSlot slot;
        slot.config = server;
        slots_.push_back(std::move(slot));
    }
}

McpManager::~McpManager() {
    bool already_shutdown = false;
    {
        const std::lock_guard lock(mutex_);
        already_shutdown = shutdown_;
    }
    if (!already_shutdown) {
        (void)shutdown(std::chrono::milliseconds{200}).get();
    }
}

void McpManager::validate() const {
    const std::string reason = validate_mcp_config(config_, tool_config_);
    if (!reason.empty()) {
        throw ConfigError("mcp: " + reason);
    }
}

std::size_t McpManager::effectiveResultBytes(const McpServerConfig& config) const {
    constexpr std::size_t kEnvelopeHeadroom = 256;
    const std::size_t cap = tool_config_.tool_result_max_bytes > kEnvelopeHeadroom
                                ? tool_config_.tool_result_max_bytes - kEnvelopeHeadroom
                                : tool_config_.tool_result_max_bytes;
    return std::min(config.max_result_bytes, cap);
}

std::unique_ptr<McpClient> McpManager::makeClient(
    const McpServerConfig& config,
    std::chrono::milliseconds handshake_timeout) {
    McpConfig client_config = config_;
    client_config.handshake_timeout = handshake_timeout;
    return factory_(config, client_config, environment_, governor_, logger_);
}

McpManager::ServerSlot* McpManager::findSlotLocked(const McpServerId& id) {
    for (ServerSlot& slot : slots_) {
        if (slot.config.id == id) {
            return &slot;
        }
    }
    return nullptr;
}

Event McpManager::makeStatusEventLocked(const ServerSlot& slot, std::string reason) {
    payload::McpServerStatusChanged status;
    status.server = slot.config.id;
    status.state = slot.state;
    status.tool_count = slot.tool_count;
    status.reason = std::move(reason);

    Event event;
    event.id = EventId{"mcp-status-" + std::to_string(++status_sequence_)};
    event.session_id = SessionId{};
    event.timestamp = std::chrono::system_clock::now();
    event.type = EventType::McpServerStatusChanged;
    event.payload = status;
    return event;
}

Event McpManager::setStateLocked(ServerSlot& slot, McpServerState state, std::string reason) {
    slot.state = state;
    if (!reason.empty()) {
        slot.last_error = reason;
    }
    return makeStatusEventLocked(slot, std::move(reason));
}

Event McpManager::installToolsLocked(ServerSlot& slot, std::vector<McpToolInfo> tools) {
    std::vector<std::unique_ptr<Tool>> built;
    std::vector<std::string>           skipped;
    std::set<std::string>              seen;
    const std::size_t                  result_bytes = effectiveResultBytes(slot.config);

    for (const McpToolInfo& info : tools) {
        if (info.remote_name.value.empty()) {
            skipped.push_back("<empty>");
            continue;
        }
        if (!mcp_tool_allowed(slot.config, info.remote_name.value)) {
            skipped.push_back(info.remote_name.value);
            continue;
        }
        std::vector<std::string> notes;
        std::expected<ToolSchema, McpErrorCode> translated = translate_mcp_schema(
            slot.config.id, info.remote_name, info.input_schema,
            tool_config_.tool_description_max_bytes, &notes);
        if (!translated.has_value()) {
            skipped.push_back(info.remote_name.value);
            continue;
        }
        if (!seen.insert(translated->name.value).second) {
            skipped.push_back(info.remote_name.value);
            continue;
        }
        translated->destructive = info.destructive;
        built.push_back(std::make_unique<McpTool>(slot.client, slot.config,
                                                  std::move(*translated),
                                                  info.remote_name, result_bytes));
    }

    if (!slot.scope.has_value()) {
        throw McpError{McpErrorCode::Internal, "no adapter scope"};
    }
    slot.scope->replace(std::move(built));
    slot.tool_count = slot.scope->size();
    slot.skipped_tools = std::move(skipped);
    return setStateLocked(slot, slot.tool_count == 0 ? McpServerState::Degraded
                                                     : McpServerState::Ready,
                          {});
}

Task<void> McpManager::start(CancellationToken cancel) {
    const McpClock::time_point deadline = now_() + config_.startup_deadline;
    std::size_t started = 0;
    const std::size_t slot_count = slots_.size();

    for (std::size_t index = 0; index < slot_count; ++index) {
        McpServerConfig config;
        bool cap_exhausted = false;
        bool cap_required = false;
        std::optional<Event> cap_event;
        {
            const std::lock_guard lock(mutex_);
            ServerSlot& slot = slots_[index];
            config = slot.config;
            if (!config_.enabled || !slot.config.enabled) {
                slot.state = McpServerState::Disabled;
                continue;
            }
            if (started >= config_.max_servers) {
                cap_exhausted = true;
                cap_required = slot.config.required;
                cap_event = setStateLocked(slot, McpServerState::Failed, "max_servers reached");
            } else {
                ++started;
            }
        }
        if (cap_exhausted) {
            if (cap_event.has_value()) {
                bus_.publish(std::move(*cap_event));
            }
            if (cap_required) {
                throw McpError{McpErrorCode::CapExhausted, "max_servers reached"};
            }
            continue;
        }

        std::optional<ToolRegistry::AdapterScope> scope;
        try {
            scope = registry_.openAdapter("mcp." + config.id.value + ".");
        } catch (const std::exception& error) {
            std::optional<Event> event;
            {
                const std::lock_guard lock(mutex_);
                event = setStateLocked(slots_[index], McpServerState::Failed, error.what());
            }
            bus_.publish(std::move(*event));
            if (config.required) {
                throw McpError{McpErrorCode::ConfigInvalid, error.what()};
            }
            continue;
        }
        {
            const std::lock_guard lock(mutex_);
            slots_[index].scope = std::move(scope);
        }

        std::shared_ptr<McpClient> client;
        try {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now_());
            if (remaining.count() <= 0) {
                throw McpError{McpErrorCode::HandshakeTimeout, "startup deadline exceeded"};
            }
            const auto handshake = std::min(config_.handshake_timeout, remaining);
            client = makeClient(config, handshake);
            const McpServerId id = config.id;
            client->setNotificationHandler(
                [](std::string_view, const nlohmann::json&) {},
                [this, id] {
                    try {
                        refresh(id, {}).get();
                    } catch (const std::exception&) {
                    }
                });
        } catch (const McpError& error) {
            std::optional<Event> event;
            {
                const std::lock_guard lock(mutex_);
                event = setStateLocked(slots_[index], McpServerState::Failed,
                                       std::string{to_string(error.code())});
            }
            bus_.publish(std::move(*event));
            if (config.required) {
                throw;
            }
            continue;
        } catch (const std::exception& error) {
            std::optional<Event> event;
            {
                const std::lock_guard lock(mutex_);
                event = setStateLocked(slots_[index], McpServerState::Failed, error.what());
            }
            bus_.publish(std::move(*event));
            if (config.required) {
                throw McpError{McpErrorCode::Internal, error.what()};
            }
            continue;
        }
        {
            const std::lock_guard lock(mutex_);
            slots_[index].client = client;
        }

        {
            std::optional<Event> event;
            {
                const std::lock_guard lock(mutex_);
                event = setStateLocked(slots_[index], McpServerState::Starting, {});
            }
            bus_.publish(std::move(*event));
        }

        try {
            client->start(cancel).get();
            std::vector<McpToolInfo> tools = client->listTools(cancel).get();
            std::optional<Event> event;
            bool ready = false;
            {
                const std::lock_guard lock(mutex_);
                event = installToolsLocked(slots_[index], std::move(tools));
                ready = slots_[index].state == McpServerState::Ready;
            }
            bus_.publish(std::move(*event));
            if (config.required && !ready) {
                throw McpError{McpErrorCode::HandshakeRejected,
                               "required server has no usable tools"};
            }
        } catch (const McpError& error) {
            std::optional<Event> event;
            {
                const std::lock_guard lock(mutex_);
                event = setStateLocked(slots_[index], McpServerState::Failed,
                                       std::string{to_string(error.code())});
            }
            bus_.publish(std::move(*event));
            if (config.required) {
                throw;
            }
        } catch (const std::exception& error) {
            std::optional<Event> event;
            {
                const std::lock_guard lock(mutex_);
                event = setStateLocked(slots_[index], McpServerState::Failed, error.what());
            }
            bus_.publish(std::move(*event));
            if (config.required) {
                throw McpError{McpErrorCode::Internal, error.what()};
            }
        }
    }
    return Task<void>{};
}

Task<void> McpManager::refresh(McpServerId id, CancellationToken cancel) {
    McpServerConfig config;
    std::shared_ptr<McpClient> client;
    {
        const std::lock_guard lock(mutex_);
        ServerSlot* slot = findSlotLocked(id);
        if (slot == nullptr || slot->client == nullptr) {
            return Task<void>{};
        }
        config = slot->config;
        client = slot->client;
    }
    try {
        const McpServerState current = client->state();
        if (current != McpServerState::Ready && current != McpServerState::Degraded &&
            current != McpServerState::Stopped) {
            {
                const std::lock_guard lock(mutex_);
                ServerSlot* slot = findSlotLocked(id);
                if (slot != nullptr) {
                    slot->reconnect_attempt += 1;
                    slot->next_backoff =
                        compute_mcp_backoff(slot->reconnect_attempt, config_, 0.5);
                }
            }
            client->shutdown(std::chrono::milliseconds{0}).get();
            client->start(cancel).get();
        }
        std::vector<McpToolInfo> tools = client->listTools(cancel).get();
        std::optional<Event> event;
        {
            const std::lock_guard lock(mutex_);
            ServerSlot* slot = findSlotLocked(id);
            if (slot != nullptr) {
                event = installToolsLocked(*slot, std::move(tools));
                slot->reconnect_attempt = 0;
            }
        }
        if (event.has_value()) {
            bus_.publish(std::move(*event));
        }
    } catch (const McpError& error) {
        std::optional<Event> event;
        {
            const std::lock_guard lock(mutex_);
            ServerSlot* slot = findSlotLocked(id);
            if (slot != nullptr) {
                slot->last_error = std::string{to_string(error.code())};
                event = makeStatusEventLocked(*slot, slot->last_error);
            }
        }
        if (event.has_value()) {
            bus_.publish(std::move(*event));
        }
        if (config.required) {
            throw;
        }
    } catch (const std::exception& error) {
        std::optional<Event> event;
        {
            const std::lock_guard lock(mutex_);
            ServerSlot* slot = findSlotLocked(id);
            if (slot != nullptr) {
                slot->last_error = error.what();
                event = makeStatusEventLocked(*slot, slot->last_error);
            }
        }
        if (event.has_value()) {
            bus_.publish(std::move(*event));
        }
    }
    return Task<void>{};
}

Task<void> McpManager::shutdown(std::chrono::milliseconds grace) {
    {
        const std::lock_guard lock(mutex_);
        if (shutdown_) {
            return Task<void>{};
        }
        shutdown_ = true;
    }
    const std::size_t slot_count = slots_.size();
    for (std::size_t index = 0; index < slot_count; ++index) {
        std::shared_ptr<McpClient> client;
        {
            const std::lock_guard lock(mutex_);
            client = slots_[index].client;
        }
        if (client != nullptr) {
            try {
                client->shutdown(grace).get();
            } catch (const std::exception&) {
            }
        }
        std::optional<Event> event;
        {
            const std::lock_guard lock(mutex_);
            ServerSlot& slot = slots_[index];
            slot.scope.reset();
            slot.tool_count = 0;
            event = setStateLocked(slot, McpServerState::Stopped, {});
        }
        bus_.publish(std::move(*event));
    }
    return Task<void>{};
}

std::vector<McpServerStatus> McpManager::statuses() const {
    const std::lock_guard lock(mutex_);
    std::vector<McpServerStatus> result;
    result.reserve(slots_.size());
    for (const ServerSlot& slot : slots_) {
        McpServerStatus status =
            slot.client != nullptr ? slot.client->status() : McpServerStatus{};
        status.id = slot.config.id;
        status.state = slot.state;
        status.tool_count = slot.tool_count;
        status.skipped_tools = slot.skipped_tools;
        if (!slot.last_error.empty()) {
            status.last_error = slot.last_error;
        }
        result.push_back(std::move(status));
    }
    std::sort(result.begin(), result.end(),
              [](const McpServerStatus& left, const McpServerStatus& right) {
                  return left.id < right.id;
              });
    return result;
}

void McpManager::setClientFactory(McpClientFactory factory) {
    if (factory) {
        factory_ = std::move(factory);
    }
}

} // namespace ymh
