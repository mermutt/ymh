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
    if (!shutdown_) {
        (void)shutdown(std::chrono::milliseconds{200}).get();
    }
}

void McpManager::validate() const {
    std::set<std::string> ids;
    for (const McpServerConfig& server : config_.servers) {
        if (!is_valid_mcp_server_id(server.id.value)) {
            throw ConfigError("mcp: invalid server id '" + server.id.value + "'");
        }
        if (!ids.insert(server.id.value).second) {
            throw ConfigError("mcp: duplicate server id '" + server.id.value + "'");
        }
        if (server.transport == McpTransportKind::HttpSse) {
            if (server.enabled && !config_.allow_network_servers) {
                throw ConfigError("mcp: http_sse requires allow_network_servers");
            }
            if (server.url.empty()) {
                throw ConfigError("mcp: http_sse server '" + server.id.value +
                                  "' has no url");
            }
        } else {
            if (server.command.empty()) {
                throw ConfigError("mcp: stdio server '" + server.id.value +
                                  "' has no command");
            }
            try {
                (void)resolve_mcp_env(server.env);
            } catch (const McpError& error) {
                throw ConfigError("mcp: server '" + server.id.value + "': " + error.what());
            }
        }
        if (server.max_result_bytes > tool_config_.tool_result_max_bytes) {
            throw ConfigError("mcp: server '" + server.id.value +
                              "' max_result_bytes exceeds tool_result_max_bytes");
        }
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

McpManager::ServerSlot* McpManager::findSlot(const McpServerId& id) {
    for (ServerSlot& slot : slots_) {
        if (slot.config.id == id) {
            return &slot;
        }
    }
    return nullptr;
}

void McpManager::emitStatus(const ServerSlot& slot, std::string reason) {
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
    bus_.publish(std::move(event));
}

void McpManager::setState(ServerSlot& slot, McpServerState state, std::string reason) {
    slot.state = state;
    if (!reason.empty()) {
        slot.last_error = reason;
    }
    emitStatus(slot, std::move(reason));
}

void McpManager::installTools(ServerSlot& slot, std::vector<McpToolInfo> tools) {
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
    setState(slot, slot.tool_count == 0 ? McpServerState::Degraded
                                        : McpServerState::Ready,
             {});
}

Task<void> McpManager::start(CancellationToken cancel) {
    const McpClock::time_point deadline = now_() + config_.startup_deadline;
    std::size_t started = 0;

    for (ServerSlot& slot : slots_) {
        if (!config_.enabled || !slot.config.enabled) {
            slot.state = McpServerState::Disabled;
            continue;
        }
        if (started >= config_.max_servers) {
            setState(slot, McpServerState::Failed, "max_servers reached");
            if (slot.config.required) {
                throw McpError{McpErrorCode::CapExhausted, "max_servers reached"};
            }
            continue;
        }
        ++started;

        try {
            slot.scope = registry_.openAdapter("mcp." + slot.config.id.value + ".");
        } catch (const std::exception& error) {
            setState(slot, McpServerState::Failed, error.what());
            if (slot.config.required) {
                throw McpError{McpErrorCode::ConfigInvalid, error.what()};
            }
            continue;
        }

        try {
            const auto remaining =
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now_());
            if (remaining.count() <= 0) {
                throw McpError{McpErrorCode::HandshakeTimeout, "startup deadline exceeded"};
            }
            const auto handshake = std::min(config_.handshake_timeout, remaining);
            slot.client = makeClient(slot.config, handshake);
            const McpServerId id = slot.config.id;
            slot.client->setNotificationHandler(
                [](std::string_view, const nlohmann::json&) {},
                [this, id] {
                    try {
                        refresh(id, {}).get();
                    } catch (const std::exception&) {
                    }
                });
        } catch (const McpError& error) {
            setState(slot, McpServerState::Failed, std::string{to_string(error.code())});
            if (slot.config.required) {
                throw;
            }
            continue;
        } catch (const std::exception& error) {
            setState(slot, McpServerState::Failed, error.what());
            if (slot.config.required) {
                throw McpError{McpErrorCode::Internal, error.what()};
            }
            continue;
        }

        try {
            setState(slot, McpServerState::Starting, {});
            slot.client->start(cancel).get();
            std::vector<McpToolInfo> tools = slot.client->listTools(cancel).get();
            installTools(slot, std::move(tools));
            if (slot.config.required && slot.state != McpServerState::Ready) {
                throw McpError{McpErrorCode::HandshakeRejected,
                               "required server has no usable tools"};
            }
        } catch (const McpError& error) {
            setState(slot, McpServerState::Failed, std::string{to_string(error.code())});
            if (slot.config.required) {
                throw;
            }
        } catch (const std::exception& error) {
            setState(slot, McpServerState::Failed, error.what());
            if (slot.config.required) {
                throw McpError{McpErrorCode::Internal, error.what()};
            }
        }
    }
    return Task<void>{};
}

Task<void> McpManager::refresh(McpServerId id, CancellationToken cancel) {
    ServerSlot* slot = findSlot(id);
    if (slot == nullptr || slot->client == nullptr) {
        return Task<void>{};
    }
    try {
        const McpServerState current = slot->client->state();
        if (current != McpServerState::Ready && current != McpServerState::Degraded &&
            current != McpServerState::Stopped) {
            slot->reconnect_attempt += 1;
            slot->next_backoff = compute_mcp_backoff(slot->reconnect_attempt, config_, 0.5);
            slot->client->shutdown(std::chrono::milliseconds{0}).get();
            slot->client->start(cancel).get();
        }
        std::vector<McpToolInfo> tools = slot->client->listTools(cancel).get();
        installTools(*slot, std::move(tools));
        slot->reconnect_attempt = 0;
    } catch (const McpError& error) {
        slot->last_error = std::string{to_string(error.code())};
        emitStatus(*slot, slot->last_error);
        if (slot->config.required) {
            throw;
        }
    } catch (const std::exception& error) {
        slot->last_error = error.what();
        emitStatus(*slot, slot->last_error);
    }
    return Task<void>{};
}

Task<void> McpManager::shutdown(std::chrono::milliseconds grace) {
    if (shutdown_) {
        return Task<void>{};
    }
    shutdown_ = true;
    for (ServerSlot& slot : slots_) {
        if (slot.client != nullptr) {
            try {
                slot.client->shutdown(grace).get();
            } catch (const std::exception&) {
            }
        }
        slot.scope.reset();
        slot.tool_count = 0;
        setState(slot, McpServerState::Stopped, {});
    }
    return Task<void>{};
}

std::vector<McpServerStatus> McpManager::statuses() const {
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
