#include "ymh/registry/process_scan.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace ymh {
namespace {

std::string executable_basename(const std::string& argv0) {
    return std::filesystem::path{argv0}.filename().string();
}

bool flag_value(const std::vector<std::string>& argv, const std::string& flag,
                std::string& value) {
    for (std::size_t index = 0; index + 1 < argv.size(); ++index) {
        if (argv[index] == flag) {
            value = argv[index + 1];
            return true;
        }
    }
    return false;
}

bool all_digits(const std::string& value) {
    return !value.empty() && std::all_of(value.begin(), value.end(), [](char character) {
        return character >= '0' && character <= '9';
    });
}

} // namespace

bool is_ymh_host_process(const std::vector<std::string>& argv) {
    if (argv.empty() || executable_basename(argv[0]) != "ymh") {
        return false;
    }
    if (std::find(argv.begin(), argv.end(), std::string{"--host"}) == argv.end()) {
        return false;
    }
    std::string workspace;
    std::string socket;
    return flag_value(argv, "--workspace", workspace) && flag_value(argv, "--socket", socket) &&
           !workspace.empty() && !socket.empty();
}

std::vector<ScannedHost> scanHosts(
    std::span<const ProcessEntry> table,
    const std::function<std::optional<WorkspaceRecord>(const WorkspaceId&)>& lookup,
    const HostConfirmer& confirm) {
    std::vector<ScannedHost> result;
    for (const ProcessEntry& entry : table) {
        if (!is_ymh_host_process(entry.argv)) {
            continue;
        }
        std::string workspace_value;
        std::string socket_argument;
        (void)flag_value(entry.argv, "--workspace", workspace_value);
        (void)flag_value(entry.argv, "--socket", socket_argument);

        ScannedHost host;
        host.pid = entry.pid;
        host.workspace = WorkspaceId{workspace_value};

        std::filesystem::path root;
        std::filesystem::path socket;
        const auto record = lookup(host.workspace);
        if (record.has_value()) {
            root = record->canonicalPath;
            if (record->host.has_value() && !record->host->socketPath.empty()) {
                socket = record->host->socketPath;
            } else {
                socket = root / ".ymh" / "host.sock";
            }
        } else {
            std::error_code error;
            root = entry.cwd.empty() ? std::filesystem::path{} 
                                     : std::filesystem::canonical(entry.cwd, error);
            if (error) {
                root = entry.cwd;
            }
            socket = root / ".ymh" / "host.sock";
        }

        host.workspaceRoot = root;
        host.socketPath = socket;
        host.confirmed = !root.empty() && confirm(root, socket);
        if (host.confirmed) {
            result.push_back(std::move(host));
        }
    }
    return result;
}

std::vector<ProcessEntry> readProcessTable() {
    std::vector<ProcessEntry> table;
    std::error_code           error;
    std::filesystem::directory_iterator iterator{"/proc", error};
    const std::filesystem::directory_iterator end;
    while (!error && iterator != end) {
        const std::filesystem::path entry = iterator->path();
        const std::string           name = entry.filename().string();
        if (all_digits(name)) {
            ProcessEntry process;
            process.pid = static_cast<std::int32_t>(std::stol(name));

            std::ifstream cmdline{entry / "cmdline", std::ios::binary};
            std::string   token;
            while (std::getline(cmdline, token, '\0')) {
                process.argv.push_back(token);
            }
            if (!process.argv.empty() && process.argv.back().empty()) {
                process.argv.pop_back();
            }

            std::error_code link_error;
            process.cwd = std::filesystem::read_symlink(entry / "cwd", link_error);
            if (link_error) {
                process.cwd.clear();
            }
            table.push_back(std::move(process));
        }
        iterator.increment(error);
    }
    return table;
}

} // namespace ymh
