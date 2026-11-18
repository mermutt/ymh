#pragma once

// Per-host runtime paths. AF_UNIX sockets are meaningful only on the machine
// that bound them; keeping them under a shared workspace or NFS home makes a
// remote daemon's endpoint look local. The socket lives in a private local
// directory instead, keyed by workspace id.
//
// `YMH_RUNTIME_DIR` overrides the root (tests). Otherwise `$TMPDIR` (or /tmp)
// plus the effective uid. /tmp is deliberate: `$XDG_RUNTIME_DIR` is
// logind-session-scoped and can be removed when the SSH session that spawned
// the daemon exits, leaving a live daemon without a reachable path.

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

#include <sys/stat.h>
#include <unistd.h>

namespace ymh {

namespace detail {

inline bool safe_path_component(std::string_view value) {
    if (value.empty() || value == "." || value == "..") {
        return false;
    }
    for (const char character : value) {
        if (character == '/' || character == '\\' || character == '\0') {
            return false;
        }
    }
    return true;
}

inline std::filesystem::path host_runtime_root() {
    if (const char* override = std::getenv("YMH_RUNTIME_DIR"); override != nullptr && *override != '\0') {
        return std::filesystem::path{override};
    }
    const char* temporary = std::getenv("TMPDIR");
    const std::filesystem::path base =
        (temporary != nullptr && *temporary != '\0') ? std::filesystem::path{temporary}
                                                     : std::filesystem::path{"/tmp"};
    return base / ("ymh-" + std::to_string(static_cast<long long>(::geteuid())));
}

inline void ensure_private_directory(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw std::runtime_error("cannot create runtime directory " + directory.string() + ": " +
                                 error.message());
    }
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);

    struct stat status {};
    if (::lstat(directory.c_str(), &status) != 0) {
        throw std::runtime_error("cannot stat runtime directory " + directory.string() + ": " +
                                 std::strerror(errno));
    }
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::geteuid()) {
        throw std::runtime_error("runtime directory is not a private user directory: " +
                                 directory.string());
    }
}

} // namespace detail

// Creates and verifies the private runtime root.
inline std::filesystem::path ensure_host_runtime_dir() {
    const std::filesystem::path root = detail::host_runtime_root();
    detail::ensure_private_directory(root);
    return root;
}

// `<runtime>/<workspace-id>/host.sock` without creating anything.
inline std::filesystem::path host_socket_path(std::string_view workspace_id) {
    if (!detail::safe_path_component(workspace_id)) {
        throw std::runtime_error("invalid workspace id for runtime path");
    }
    return detail::host_runtime_root() / std::string{workspace_id} / "host.sock";
}

// `<runtime>/<workspace-id>/host.sock`, with the workspace directory created.
inline std::filesystem::path local_host_socket_path(std::string_view workspace_id) {
    const std::filesystem::path root = ensure_host_runtime_dir();
    if (!detail::safe_path_component(workspace_id)) {
        throw std::runtime_error("invalid workspace id for runtime path");
    }
    const std::filesystem::path directory = root / std::string{workspace_id};
    detail::ensure_private_directory(directory);
    return directory / "host.sock";
}

} // namespace ymh
