#include "ymh/mcp/mcp_transport.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ymh/execution/errors.hpp"

namespace ymh {
namespace {

std::string sanitize_server_id(std::string_view id) {
    std::string out;
    out.reserve(id.size());
    for (const char c : id) {
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        out.push_back(safe ? c : '_');
    }
    return out.empty() ? std::string{"server"} : out;
}

} // namespace

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
      log_child_stderr_(mcp_config.log_child_stderr),
      environment_(environment),
      logger_(logger) {}

StdioMcpTransport::~StdioMcpTransport() {
    if (child_ != nullptr) {
        (void)close(std::chrono::milliseconds{200}).get();
    }
    closeCaptureFd();
}

bool StdioMcpTransport::openCaptureFd() {
    closeCaptureFd();
    if (log_child_stderr_) {
        const std::filesystem::path directory = environment_.root() / ".ymh" / "mcp";
        const std::filesystem::path path =
            directory / (sanitize_server_id(config_.id.value) + ".stderr.log");
        std::error_code ec;
        std::filesystem::create_directories(directory, ec);
        const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_APPEND, 0600);
        if (fd < 0) {
            logger_.warn("mcp: cannot open child stderr log '" + path.string() +
                         "': " + std::strerror(errno));
            return false;
        }
        struct stat info {};
        capture_offset_ =
            ::fstat(fd, &info) == 0 ? static_cast<std::uintmax_t>(info.st_size) : 0;
        capture_path_ = path;
        capture_fd_ = fd;
        return true;
    }
    std::error_code temp_ec;
    const std::filesystem::path directory = std::filesystem::temp_directory_path(temp_ec);
    if (temp_ec) {
        logger_.warn("mcp: no temporary directory for a stderr capture: " +
                     temp_ec.message());
        return false;
    }
    int fd = -1;
#ifdef O_TMPFILE
    fd = ::open(directory.c_str(), O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
#endif
    if (fd < 0) {
        std::string tmpl = (directory / "ymh-mcp-XXXXXX").string();
        std::vector<char> buffer(tmpl.begin(), tmpl.end());
        buffer.push_back('\0');
        fd = ::mkstemp(buffer.data());
        if (fd >= 0) {
            capture_path_.clear();
            ::unlink(buffer.data());
        }
    }
    if (fd < 0) {
        logger_.warn("mcp: cannot create a stderr capture for '" + config_.id.value +
                     "': " + std::strerror(errno));
        return false;
    }
    capture_fd_ = fd;
    capture_offset_ = 0;
    capture_path_.clear();
    return true;
}

void StdioMcpTransport::closeCaptureFd() {
    if (capture_fd_ >= 0) {
        ::close(capture_fd_);
        capture_fd_ = -1;
    }
    capture_path_.clear();
    capture_offset_ = 0;
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
    request.environment = resolve_mcp_env(config_.env);
    request.env_mode = ProcessEnvMode::Inherit;
    request.capture_stdout = true;
    request.capture_stderr = false;

    try {
        request.cwd = config_.cwd.empty() ? environment_.root()
                                          : environment_.resolve(config_.cwd.string());
    } catch (const ToolError&) {
        throw McpError{McpErrorCode::ConfigInvalid,
                       "mcp server '" + config_.id.value +
                           "': cwd escapes the workspace root"};
    }

    if (openCaptureFd()) {
        if (log_child_stderr_) {
            request.stderr_path = capture_path_;
        } else {
            request.stderr_fd = capture_fd_;
        }
    }

    try {
        child_ = environment_.process().spawn(request).get();
    } catch (const ToolError& error) {
        throw McpError{McpErrorCode::SpawnFailed,
                       "spawn failed: " + config_.command + ": " + error.what()};
    }
    if (child_ == nullptr) {
        throw McpError{McpErrorCode::SpawnFailed,
                       "spawn failed: " + config_.command + ": spawn returned no child"};
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
        throw McpError{McpErrorCode::TransportClosed,
                       "transport write failed: " + std::string{error.what()}};
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
    if (!child_->reaped() && !child_->tryReap().has_value()) {
        child_->signal(SIGTERM);
        const auto deadline = std::chrono::steady_clock::now() + grace;
        while (!child_->reaped() && std::chrono::steady_clock::now() < deadline) {
            if (child_->tryReap().has_value()) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        if (!child_->reaped()) {
            child_->signal(SIGKILL);
        }
    }
    child_.reset();
    closeCaptureFd();
    return Task<void>{};
}

std::string StdioMcpTransport::failureContext() {
    std::string context;
    if (child_ != nullptr && !child_->reaped()) {
        if (const std::optional<ProcessResult> status = child_->tryReap();
            status.has_value()) {
            if (status->signalled) {
                context = "server killed by signal " + std::to_string(status->signal);
            } else {
                context = "server exited with code " + std::to_string(status->exit_code);
            }
        }
    }
    if (capture_fd_ >= 0) {
        struct stat info {};
        if (::fstat(capture_fd_, &info) == 0) {
            constexpr std::uintmax_t kMaxCaptureScan = 64u * 1024u;
            const auto size = static_cast<std::uintmax_t>(info.st_size);
            std::uintmax_t begin = capture_offset_;
            if (size > begin + kMaxCaptureScan) {
                begin = size - kMaxCaptureScan;
            }
            if (begin < size) {
                std::string raw(static_cast<std::size_t>(size - begin), '\0');
                const ssize_t count =
                    ::pread(capture_fd_, raw.data(), raw.size(), static_cast<off_t>(begin));
                if (count > 0) {
                    raw.resize(static_cast<std::size_t>(count));
                    if (!context.empty()) {
                        context.push_back('\n');
                    }
                    context += raw;
                }
            }
        }
    }
    return bound_mcp_reason(context);
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
