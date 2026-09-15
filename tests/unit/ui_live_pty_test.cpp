#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>

#include "support/pty_child.hpp"
#include "support/test_env.hpp"

#ifndef YMH_TEST_BINARY
#define YMH_TEST_BINARY "ymh"
#endif

namespace {

using namespace ymh;

bool live_enabled() {
    const char* flag = std::getenv("YMH_LIVE_LLM");
    const char* key = std::getenv("DEEPSEEK_API_KEY");
    return flag != nullptr && std::string{flag} == "1" && key != nullptr && *key != '\0';
}

std::string strip_ansi(const std::string& input) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size(); ++index) {
        if (input[index] == '\x1b' && index + 1 < input.size() && input[index + 1] == '[') {
            index += 2;
            while (index < input.size() &&
                   (input[index] == ';' || (input[index] >= '0' && input[index] <= '9'))) {
                ++index;
            }
            continue;
        }
        output.push_back(input[index]);
    }
    return output;
}

class PtyProcess {
public:
    PtyProcess() = default;
    ~PtyProcess() { terminate(); }

    PtyProcess(const PtyProcess&) = delete;
    PtyProcess& operator=(const PtyProcess&) = delete;

    bool spawn(const std::string& binary, const std::string& cwd) {
        master_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (master_ < 0) {
            return false;
        }
        if (::grantpt(master_) != 0 || ::unlockpt(master_) != 0) {
            return false;
        }
        winsize window{};
        window.ws_row = 24;
        window.ws_col = 100;
        ::ioctl(master_, TIOCSWINSZ, &window);

        const char* slave_name = ::ptsname(master_);
        if (slave_name == nullptr) {
            return false;
        }
        const int slave = ::open(slave_name, O_RDWR | O_NOCTTY);
        if (slave < 0) {
            return false;
        }
        pid_ = ::fork();
        if (pid_ < 0) {
            return false;
        }
        if (pid_ == 0) {
            ::setsid();
            ::ioctl(slave, TIOCSCTTY, 0);
            ::dup2(slave, STDIN_FILENO);
            ::dup2(slave, STDOUT_FILENO);
            ::dup2(slave, STDERR_FILENO);
            if (slave > 2) {
                ::close(slave);
            }
            ::close(master_);
            if (::chdir(cwd.c_str()) != 0) {
                _exit(126);
            }
            ::execl(binary.c_str(), "ymh", static_cast<char*>(nullptr));
            _exit(127);
        }
        ::close(slave);
        return true;
    }

    void write_all(const std::string& data) const {
        if (master_ >= 0) {
            const ssize_t written = ::write(master_, data.data(), data.size());
            (void)written;
        }
    }

    std::string read_available(int timeout_ms) const {
        std::string output;
        if (master_ < 0) {
            return output;
        }
        pollfd descriptor{};
        descriptor.fd = master_;
        descriptor.events = POLLIN;
        if (::poll(&descriptor, 1, timeout_ms) <= 0) {
            return output;
        }
        char buffer[4096];
        for (;;) {
            const ssize_t count = ::read(master_, buffer, sizeof(buffer));
            if (count <= 0) {
                break;
            }
            output.append(buffer, static_cast<std::size_t>(count));
            if (count < static_cast<ssize_t>(sizeof(buffer))) {
                break;
            }
        }
        return output;
    }

    void terminate() {
        if (pid_ > 0) {
            int status = 0;
            if (::waitpid(pid_, &status, WNOHANG) == 0) {
                ::kill(pid_, SIGTERM);
                std::this_thread::sleep_for(std::chrono::milliseconds{200});
                if (::waitpid(pid_, &status, WNOHANG) == 0) {
                    ::kill(pid_, SIGKILL);
                    ::waitpid(pid_, &status, 0);
                }
            }
            pid_ = -1;
        }
        if (master_ >= 0) {
            ::close(master_);
            master_ = -1;
        }
    }

private:
    int   master_ = -1;
    pid_t pid_ = -1;
};

TEST(UiLivePty, StreamsAssistantReply) {
    if (!live_enabled()) {
        GTEST_SKIP() << "opt-in: set YMH_LIVE_LLM=1 and DEEPSEEK_API_KEY to run";
    }

    test::TempWorkspace workspace("ui_live_pty");
    PtyProcess pty;
    ASSERT_TRUE(pty.spawn(YMH_TEST_BINARY, workspace.path().string()));

    // The no-args binary is the M2 supervisor TUI, which auto-creates the first
    // session asynchronously; keystrokes typed before it is active are dropped.
    std::string output;
    const auto startup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{30};
    while (std::chrono::steady_clock::now() < startup_deadline) {
        output += pty.read_available(200);
        if (strip_ansi(output).find("Type a message and press Enter") != std::string::npos) {
            break;
        }
    }

    pty.write_all("Reply with exactly the word pong and do not call any tools.\r");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{90};
    bool saw_reply = false;
    bool answered_permission = false;
    while (std::chrono::steady_clock::now() < deadline) {
        output += pty.read_available(500);
        const std::string plain = strip_ansi(output);
        if (!answered_permission && plain.find("Permission required") != std::string::npos) {
            pty.write_all("y");
            answered_permission = true;
        }
        if (plain.find("pong") != std::string::npos) {
            saw_reply = true;
            break;
        }
    }

    pty.write_all("/exit\r");
    pty.terminate();
    test::stop_hosts_for_root(workspace.path());

    EXPECT_TRUE(saw_reply) << strip_ansi(output);
}

} // namespace
