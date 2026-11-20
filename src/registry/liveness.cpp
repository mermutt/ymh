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
    return probeWorkspaceLock(workspaceRoot, true);
}

WorkspaceLockProbe probeWorkspaceLock(const std::filesystem::path& workspaceRoot,
                                      bool create_if_missing) {
    WorkspaceLockProbe probe;
    const std::filesystem::path lock_path = workspaceRoot / ".ymh" / "sessions.lock";
    const int flags = create_if_missing ? (O_RDWR | O_CREAT | O_CLOEXEC) : (O_RDONLY | O_CLOEXEC);
    const int fd = ::open(lock_path.c_str(), flags, 0600);
    if (fd < 0) {
        return probe;
    }
    // The read-only probe (doctor, `workspace stop`) must not ask for an
    // exclusive lock on an O_RDONLY descriptor: NFS maps flock onto POSIX
    // record locks, where a write lock on a read-only fd fails with EBADF, and
    // that was silently reported as "lock free" while the writable probe
    // reported "held". A shared lock is enough to detect an exclusive holder
    // and is valid on both open modes.
    const int lock_mode = (create_if_missing ? LOCK_EX : LOCK_SH) | LOCK_NB;
    if (::flock(fd, lock_mode) == 0) {
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
            if (diagnostic.contains("build") && diagnostic.at("build").is_string()) {
                probe.build = diagnostic.at("build").get<std::string>();
            }
            if (diagnostic.contains("config_digest") &&
                diagnostic.at("config_digest").is_string()) {
                probe.configDigest = diagnostic.at("config_digest").get<std::string>();
            }
        } catch (const nlohmann::json::exception&) {
            probe.pid.reset();
            probe.bootId.reset();
            probe.build.reset();
            probe.configDigest.reset();
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
