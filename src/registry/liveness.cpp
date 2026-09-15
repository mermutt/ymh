#include "ymh/registry/liveness.hpp"

#include <cerrno>
#include <cstdint>
#include <string>

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace ymh {

WorkspaceLockProbe probeWorkspaceLock(const std::filesystem::path& workspaceRoot) {
    WorkspaceLockProbe probe;
    const std::filesystem::path lock_path = workspaceRoot / ".ymh" / "sessions.lock";
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        return probe;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return probe;
    }
    if (errno != EWOULDBLOCK) {
        ::close(fd);
        return probe;
    }
    probe.held = true;

    char          buffer[4096];
    const ssize_t bytes = ::pread(fd, buffer, sizeof(buffer), 0);
    if (bytes > 0) {
        try {
            const auto diagnostic =
                nlohmann::json::parse(std::string{buffer, static_cast<std::size_t>(bytes)});
            if (diagnostic.contains("pid") && diagnostic.at("pid").is_number()) {
                probe.pid = diagnostic.at("pid").get<std::int32_t>();
            }
            if (diagnostic.contains("boot_id") && diagnostic.at("boot_id").is_string()) {
                probe.bootId = diagnostic.at("boot_id").get<std::string>();
            }
        } catch (const nlohmann::json::exception&) {
            probe.pid.reset();
            probe.bootId.reset();
        }
    }
    ::close(fd);
    return probe;
}

KillHint killHint(std::int32_t pid) {
    if (pid <= 0) {
        return KillHint::Unknown;
    }
    if (::kill(pid, 0) == 0) {
        return KillHint::Alive;
    }
    return errno == ESRCH ? KillHint::Dead : KillHint::Unknown;
}

} // namespace ymh
