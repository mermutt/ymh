#include "ymh/skills/workspace_trust.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include <nlohmann/json.hpp>

namespace ymh {
namespace {

using Json = nlohmann::json;

std::filesystem::path state_dir() {
    if (const char* xdg = std::getenv("XDG_STATE_HOME"); xdg != nullptr && *xdg != '\0') {
        return std::filesystem::path{xdg} / "ymh";
    }
    if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        return std::filesystem::path{home} / ".local" / "state" / "ymh";
    }
    return std::filesystem::path{".local"} / "state" / "ymh";
}

std::string canonical_key(const std::filesystem::path& workspace) {
    std::error_code       ec;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(workspace, ec);
    if (ec) {
        canonical = workspace.lexically_normal();
    }
    // `weakly_canonical` keeps a trailing separator for a path that does not
    // exist, so `/ws` and `/ws/` would otherwise be two different trust keys.
    // A path with no filename component that is not the root is its parent.
    if (!canonical.has_filename() && canonical != canonical.root_path()) {
        canonical = canonical.parent_path();
    }
    return canonical.string();
}

std::vector<std::string> read_trusted(const std::filesystem::path& store_path) {
    std::ifstream input(store_path);
    if (!input) {
        return {};
    }
    Json document = Json::parse(input, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
        return {};
    }
    const auto found = document.find("trusted");
    if (found == document.end() || !found->is_array()) {
        return {};
    }
    std::vector<std::string> trusted;
    for (const Json& entry : *found) {
        if (entry.is_string()) {
            trusted.push_back(entry.get<std::string>());
        }
    }
    return trusted;
}

bool write_trusted(const std::filesystem::path& store_path,
                   const std::vector<std::string>& trusted) {
    std::error_code error;
    if (!store_path.parent_path().empty()) {
        std::filesystem::create_directories(store_path.parent_path(), error);
    }
    Json document;
    document["trusted"] = trusted;
    const std::string body = document.dump(2) + "\n";

    const std::filesystem::path temp =
        store_path.string() + ".tmp." + std::to_string(::getpid());
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    bool        ok      = true;
    std::size_t written = 0;
    while (written < body.size()) {
        const ssize_t bytes = ::write(fd, body.data() + written, body.size() - written);
        if (bytes < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        written += static_cast<std::size_t>(bytes);
    }
    if (ok && ::fsync(fd) != 0) {
        ok = false;
    }
    if (::close(fd) != 0) {
        ok = false;
    }
    if (!ok) {
        (void)::unlink(temp.c_str());
        return false;
    }
    if (::rename(temp.c_str(), store_path.c_str()) != 0) {
        (void)::unlink(temp.c_str());
        return false;
    }
    return true;
}

} // namespace

WorkspaceTrustStore::WorkspaceTrustStore(std::filesystem::path store_path)
    : store_path_(std::move(store_path)) {}

std::filesystem::path WorkspaceTrustStore::default_path() {
    return state_dir() / "trusted_workspaces.json";
}

bool WorkspaceTrustStore::is_trusted(const std::filesystem::path& workspace) const {
    const std::string key = canonical_key(workspace);
    const std::vector<std::string> trusted = read_trusted(store_path_);
    return std::find(trusted.begin(), trusted.end(), key) != trusted.end();
}

bool WorkspaceTrustStore::trust(const std::filesystem::path& workspace) const {
    std::vector<std::string> trusted = read_trusted(store_path_);
    const std::string        key     = canonical_key(workspace);
    if (std::find(trusted.begin(), trusted.end(), key) == trusted.end()) {
        trusted.push_back(key);
    }
    return write_trusted(store_path_, trusted);
}

bool WorkspaceTrustStore::untrust(const std::filesystem::path& workspace) const {
    std::vector<std::string> trusted = read_trusted(store_path_);
    const std::string        key     = canonical_key(workspace);
    trusted.erase(std::remove(trusted.begin(), trusted.end(), key), trusted.end());
    return write_trusted(store_path_, trusted);
}

} // namespace ymh
