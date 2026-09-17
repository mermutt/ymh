#pragma once

// Hermetic two-process harness for `ymh --host`. It spawns the real binary as
// a daemon child (the executable is injectable and defaults to the test-build
// `YMH_TEST_BINARY`, since gtest's own `/proc/self/exe` is `ymh_tests`), waits
// for the Unix socket, completes a `host.hello` handshake, and always stops the
// daemon explicitly (`host.shutdown` -> SIGTERM -> SIGKILL) so no test leaks a
// daemon or a socket. Socket paths come from a short temp root
// (`support/short_temp.hpp`) to stay under the 108-byte `sun_path` limit.

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "support/child_process.hpp"
#include "support/crash_inject.hpp"
#include "support/short_temp.hpp"
#include "ymh/registry/registry.hpp"
#include "ymh/transport/host_connection.hpp"
#include "ymh/transport/protocol.hpp"

namespace ymh::test {

struct HostHarnessOptions {
    std::filesystem::path              binary;
    std::filesystem::path              workspace_root;
    std::string                        workspace_id;
    std::filesystem::path              socket_path;
    std::filesystem::path              log_sink;
    std::filesystem::path              config_path;
    protocol::ServerProfile            profile{protocol::ServerProfile::Interactive};
    std::vector<std::string>           extra_args;
    std::map<std::string, std::string> env;
    std::chrono::milliseconds          startup_timeout{15'000};
    std::chrono::milliseconds          shutdown_timeout{15'000};
};

inline std::filesystem::path resolve_ymh_binary() {
    if (const char* env = std::getenv("YMH_TEST_BINARY"); env != nullptr && *env != '\0') {
        return std::filesystem::path{env};
    }
#ifdef YMH_TEST_BINARY
    return std::filesystem::path{YMH_TEST_BINARY};
#else
    return std::filesystem::path{"ymh"};
#endif
}

class HostHarness {
public:
    explicit HostHarness(HostHarnessOptions options) : options_(std::move(options)) {
        if (options_.workspace_root.empty()) {
            throw std::invalid_argument("HostHarness: workspace_root is required");
        }
        if (options_.binary.empty()) {
            options_.binary = resolve_ymh_binary();
        }
        if (options_.workspace_id.empty()) {
            options_.workspace_id = generate_uuid_v4();
        }
        if (options_.socket_path.empty()) {
            options_.socket_path = options_.workspace_root / ".ymh" / "host.sock";
        }
        if (options_.log_sink.empty()) {
            options_.log_sink = options_.workspace_root / ".ymh" / "host.log";
        }
    }

    ~HostHarness() { stop(); }

    HostHarness(const HostHarness&) = delete;
    HostHarness& operator=(const HostHarness&) = delete;

    void start() {
        std::filesystem::create_directories(options_.workspace_root / ".ymh");
        // M4: the daemon's global layer is required (21-D12). Write the
        // conventional config under the XDG_CONFIG_HOME this harness exports so
        // `ymh --host` never fails J-F5, independent of test ordering.
        const std::filesystem::path global_config =
            options_.workspace_root / ".config" / "ymh" / "config.jsonc";
        std::filesystem::create_directories(global_config.parent_path());
        if (!std::filesystem::exists(global_config)) {
            std::ofstream(global_config, std::ios::binary) << "{}\n";
        }
        stdout_capture_ = options_.workspace_root / ".ymh" / "host.stdout.log";
        stderr_capture_ = options_.workspace_root / ".ymh" / "host.stderr.log";

        ChildOptions child;
        child.argv             = build_argv(options_);
        child.executable       = options_.binary;
        child.cwd              = options_.workspace_root;
        child.stdout_path      = stdout_capture_;
        child.stderr_path      = stderr_capture_;
        child.env["HOME"]           = options_.workspace_root.string();
        child.env["XDG_STATE_HOME"] = (options_.workspace_root / ".state").string();
        child.env["XDG_CONFIG_HOME"] = (options_.workspace_root / ".config").string();
        child.env["XDG_CACHE_HOME"] = (options_.workspace_root / ".cache").string();
        for (const auto& [key, value] : options_.env) {
            child.env[key] = value;
        }
        child.env_remove = {"YMH_LIVE_LLM"};
        if (options_.env.find("YMH_FAKE_LLM_SCRIPT") == options_.env.end()) {
            child.env_remove.emplace_back("YMH_FAKE_LLM_SCRIPT");
        }

        process_.start(std::move(child));
        stopped_ = false;
    }

    [[nodiscard]] bool wait_ready() { return wait_ready(options_.startup_timeout); }

    [[nodiscard]] bool wait_ready(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;) {
            if (connection_ != nullptr && connection_->isConnected()) {
                return true;
            }
            if (!process_.running()) {
                return false;
            }
            if (std::filesystem::exists(options_.socket_path) && try_connect()) {
                return true;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
    }

    [[nodiscard]] protocol::HostConnection& connect() {
        if (connection_ == nullptr || !connection_->isConnected()) {
            if (!try_connect()) {
                throw std::runtime_error("HostHarness: cannot connect to " +
                                         options_.socket_path.string());
            }
        }
        return *connection_;
    }

    [[nodiscard]] protocol::HostConnection* connection() noexcept { return connection_.get(); }

    void disconnect() {
        if (connection_ != nullptr) {
            connection_->close();
            connection_.reset();
        }
    }

    ExitStatus stop() {
        if (stopped_) {
            return process_.last_status().value_or(ExitStatus{});
        }
        stopped_ = true;
        if (connection_ != nullptr && connection_->isConnected()) {
            try {
                [[maybe_unused]] const nlohmann::json reply = connection_->request(
                    protocol::method::kHostShutdown, {{"reason", "test teardown"}});
            } catch (...) {
            }
        }
        ExitStatus status;
        if (std::optional<ExitStatus> waited = process_.wait_for(options_.shutdown_timeout);
            waited.has_value()) {
            status = *waited;
        } else {
            status = process_.terminate();
        }
        disconnect();
        return status;
    }

    [[nodiscard]] ExitStatus crash(int signal = SIGKILL) {
        stopped_ = true;
        disconnect();
        return crash_now(process_, signal);
    }

    [[nodiscard]] ExitStatus wait_for_exit(std::chrono::milliseconds timeout) {
        return process_.wait_for(timeout).value_or(process_.terminate());
    }

    [[nodiscard]] bool running() { return process_.running(); }
    [[nodiscard]] pid_t pid() const noexcept { return process_.pid(); }
    [[nodiscard]] ChildProcess& process() noexcept { return process_; }

    [[nodiscard]] const std::filesystem::path& socket_path() const noexcept {
        return options_.socket_path;
    }
    [[nodiscard]] const std::filesystem::path& workspace_root() const noexcept {
        return options_.workspace_root;
    }
    [[nodiscard]] const std::string& workspace_id() const noexcept {
        return options_.workspace_id;
    }

    [[nodiscard]] std::string read_log() const {
        std::string text = read_text_file(options_.log_sink);
        text += read_text_file(stdout_capture_);
        text += read_text_file(stderr_capture_);
        return text;
    }

    [[nodiscard]] static std::vector<std::string> build_argv(const HostHarnessOptions& options) {
        std::vector<std::string> argv;
        argv.push_back(options.binary.string());
        argv.push_back("--host");
        argv.push_back("--workspace");
        argv.push_back(options.workspace_id);
        argv.push_back("--root");
        argv.push_back(options.workspace_root.string());
        argv.push_back("--socket");
        argv.push_back(options.socket_path.string());
        if (!options.config_path.empty()) {
            argv.push_back("--config");
            argv.push_back(options.config_path.string());
        }
        argv.insert(argv.end(), options.extra_args.begin(), options.extra_args.end());
        return argv;
    }

    [[nodiscard]] static bool binary_supports_host(const std::filesystem::path& binary) {
        if (const char* force = std::getenv("YMH_TEST_FORCE_HOST");
            force != nullptr && std::string{force} == "1") {
            return true;
        }
        return probe_host_flag(binary, {"--help"}) ||
               probe_host_flag(binary, {"--host", "--help"});
    }

private:
    [[nodiscard]] static bool probe_host_flag(const std::filesystem::path& binary,
                                              const std::vector<std::string>& args) {
        ShortTempRoot probe("ymh-hostprobe");
        const std::filesystem::path out = probe.path() / "help.out";
        const std::filesystem::path err = probe.path() / "help.err";

        ChildOptions child;
        child.argv        = {binary.string()};
        child.argv.insert(child.argv.end(), args.begin(), args.end());
        child.executable  = binary;
        child.stdout_path = out;
        child.stderr_path = err;
        child.env_remove  = {"YMH_FAKE_LLM_SCRIPT"};

        const ExitStatus status = run_child(std::move(child), std::chrono::seconds{10});
        if (!status.ok()) {
            return false;
        }
        const std::string text = read_text_file(out) + read_text_file(err);
        return text.find("--host") != std::string::npos;
    }

    [[nodiscard]] bool try_connect() {
        try {
            auto candidate = std::make_unique<protocol::HostConnection>();
            candidate->connect(options_.socket_path.string());
            [[maybe_unused]] const protocol::HelloResult hello = candidate->handshake(
                options_.profile, protocol::ClientInstanceId{generate_uuid_v4()});
            connection_ = std::move(candidate);
            return true;
        } catch (...) {
            return false;
        }
    }

    HostHarnessOptions                        options_;
    ChildProcess                              process_;
    std::unique_ptr<protocol::HostConnection> connection_;
    std::filesystem::path                     stdout_capture_;
    std::filesystem::path                     stderr_capture_;
    bool                                      stopped_{false};
};

} // namespace ymh::test
