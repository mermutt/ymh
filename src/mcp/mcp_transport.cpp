#include "ymh/mcp/mcp_transport.hpp"

#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <thread>
#include <utility>

#include "ymh/execution/errors.hpp"

namespace ymh {

std::vector<std::pair<std::string, std::string>> resolve_mcp_env(
    const std::vector<std::string>& entries) {
    std::vector<std::pair<std::string, std::string>> resolved;
    resolved.reserve(entries.size());
    for (const std::string& entry : entries) {
        const std::size_t equals = entry.find('=');
        if (equals == std::string::npos || equals == 0) {
            throw McpError{McpErrorCode::ConfigInvalid, "malformed env entry"};
        }
        std::string key = entry.substr(0, equals);
        const std::string raw = entry.substr(equals + 1);
        std::string value;
        for (std::size_t index = 0; index < raw.size();) {
            if (raw[index] == '$' && index + 2 < raw.size() && raw[index + 1] == '$' &&
                raw[index + 2] == '{') {
                value += "${";
                index += 3;
            } else if (raw[index] == '$' && index + 1 < raw.size() && raw[index + 1] == '{') {
                const std::size_t close = raw.find('}', index + 2);
                if (close == std::string::npos) {
                    throw McpError{McpErrorCode::ConfigInvalid,
                                   "unterminated env reference"};
                }
                const std::string name = raw.substr(index + 2, close - (index + 2));
                const char* from_env = std::getenv(name.c_str());
                if (from_env == nullptr) {
                    throw McpError{McpErrorCode::ConfigInvalid,
                                   "missing environment variable: " + name};
                }
                value += from_env;
                index = close + 1;
            } else {
                value.push_back(raw[index]);
                ++index;
            }
        }
        resolved.emplace_back(std::move(key), std::move(value));
    }
    return resolved;
}

StdioMcpTransport::StdioMcpTransport(const McpServerConfig& config,
                                     McpConfig& mcp_config,
                                     ExecutionEnvironment& environment,
                                     Logger& logger)
    : config_(config),
      max_frame_bytes_(mcp_config.max_frame_bytes),
      environment_(environment),
      logger_(logger) {}

StdioMcpTransport::~StdioMcpTransport() {
    if (child_ != nullptr) {
        (void)close(std::chrono::milliseconds{200}).get();
    }
}

Task<void> StdioMcpTransport::start(CancellationToken cancel) {
    (void)cancel;
    if (child_ != nullptr) {
        return Task<void>{};
    }
    eof_ = false;
    close_notified_ = false;
    buffer_.clear();
    if (config_.transport != McpTransportKind::Stdio) {
        throw McpError{McpErrorCode::ConfigInvalid, "stdio transport requires stdio"};
    }
    if (config_.command.empty()) {
        throw McpError{McpErrorCode::ConfigInvalid, "stdio server has no command"};
    }

    ProcessRequest request;
    request.executable = config_.command;
    request.argv.push_back(config_.command);
    for (const std::string& argument : config_.args) {
        request.argv.push_back(argument);
    }
    request.cwd = config_.cwd.empty() ? environment_.root()
                                      : environment_.resolve(config_.cwd.string());
    request.environment = resolve_mcp_env(config_.env);
    request.capture_stdout = true;
    request.capture_stderr = false;

    try {
        child_ = environment_.process().spawn(request).get();
    } catch (const ToolError& error) {
        throw McpError{McpErrorCode::SpawnFailed, std::string{to_string(error.code())}};
    }
    if (child_ == nullptr) {
        throw McpError{McpErrorCode::SpawnFailed, "spawn returned no child"};
    }
    return Task<void>{};
}

Task<void> StdioMcpTransport::send(const nlohmann::json& message,
                                   CancellationToken cancel) {
    if (child_ == nullptr) {
        throw McpError{McpErrorCode::TransportClosed, "transport is not started"};
    }
    std::string line = message.dump();
    if (line.size() > max_frame_bytes_) {
        throw McpError{McpErrorCode::ResultTooLarge, "outbound frame too large"};
    }
    line.push_back('\n');
    std::lock_guard<std::mutex> lock(send_mutex_);
    try {
        child_->writeStdin(line, cancel).get();
    } catch (const ToolError& error) {
        throw McpError{McpErrorCode::TransportClosed, std::string{to_string(error.code())}};
    }
    return Task<void>{};
}

void StdioMcpTransport::setMessageHandler(std::function<void(nlohmann::json)> handler) {
    message_handler_ = std::move(handler);
}

void StdioMcpTransport::setCloseHandler(
    std::function<void(McpDisconnectReason)> handler) {
    close_handler_ = std::move(handler);
}

Task<void> StdioMcpTransport::close(std::chrono::milliseconds grace) {
    if (child_ == nullptr) {
        return Task<void>{};
    }
    child_->closeStdin();
    child_->signal(SIGTERM);

    const int pid = static_cast<int>(child_->pid());
    const auto deadline = std::chrono::steady_clock::now() + grace;
    bool reaped = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (tryReap(pid).has_value()) {
            reaped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    if (!reaped) {
        child_->signal(SIGKILL);
        (void)reap(pid);
    }
    child_.reset();
    return Task<void>{};
}

std::uint64_t StdioMcpTransport::childPid() const noexcept {
    return child_ == nullptr ? 0 : child_->pid();
}

bool StdioMcpTransport::poll(std::chrono::milliseconds timeout) {
    if (child_ == nullptr || eof_) {
        return false;
    }
    const int fd = childStdoutFd(*child_);
    if (fd < 0) {
        return false;
    }
    pollfd descriptor{fd, POLLIN, 0};
    const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    if (ready == 0) {
        return false;
    }
    if (ready < 0) {
        if (errno == EINTR) {
            return false;
        }
        notifyClose(McpDisconnectReason::TransportError);
        return false;
    }

    std::array<char, 8192> chunk{};
    std::size_t count = 0;
    try {
        count = child_->readStdout(std::span<char>(chunk.data(), chunk.size()), {}).get();
    } catch (const CancellationError&) {
        return false;
    } catch (const ToolError&) {
        notifyClose(McpDisconnectReason::TransportError);
        return false;
    }
    if (count == 0) {
        eof_ = true;
        notifyClose(McpDisconnectReason::ServerEof);
        return false;
    }
    buffer_.append(chunk.data(), count);
    if (buffer_.size() > max_frame_bytes_) {
        notifyClose(McpDisconnectReason::ProtocolError);
        return false;
    }

    std::size_t newline = std::string::npos;
    while ((newline = buffer_.find('\n')) != std::string::npos) {
        std::string line = buffer_.substr(0, newline);
        buffer_.erase(0, newline + 1);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (!line.empty()) {
            dispatchLine(std::move(line));
        }
    }
    return true;
}

void StdioMcpTransport::dispatchLine(std::string line) {
    nlohmann::json message;
    try {
        message = nlohmann::json::parse(line);
    } catch (const std::exception&) {
        logger_.warn("mcp: dropping malformed frame");
        return;
    }
    if (message_handler_) {
        message_handler_(std::move(message));
    }
}

void StdioMcpTransport::notifyClose(McpDisconnectReason reason) {
    if (close_notified_) {
        return;
    }
    close_notified_ = true;
    if (close_handler_) {
        close_handler_(reason);
    }
}

} // namespace ymh
