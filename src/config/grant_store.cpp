#include "ymh/policy/permission_policy.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include "ymh/config/jsonc.hpp"

namespace ymh {
namespace {

using Json = nlohmann::json;

constexpr std::string_view kSchema = "ymh.permissions/1";

const Json* member(const Json& object, std::string_view key) {
    const auto it = object.find(std::string{key});
    return it == object.end() ? nullptr : &(*it);
}

void log_skip(const std::filesystem::path& path, std::string_view reason) {
    std::fprintf(stderr, "ymh: grants: ignoring %s: %.*s\n", path.c_str(),
                 static_cast<int>(reason.size()), reason.data());
}

std::string derived_id(const PolicyRule& rule) {
    return "grant:" + rule.tool + ":" + rule.command;
}

// Parses a grants document into `LocalGrant` rules. `nullopt` means the
// document is not a valid grants file (the caller loads {} and re-asks).
std::optional<std::vector<PolicyRule>> parse_grants(
    const Json& document, const std::optional<std::string>& expected_id) {
    if (!document.is_object()) {
        return std::nullopt;
    }
    const Json* schema = member(document, "schema");
    if (schema == nullptr || !schema->is_string() || schema->get<std::string>() != kSchema) {
        return std::nullopt;
    }
    if (expected_id.has_value()) {
        const Json* workspace_id = member(document, "workspace_id");
        if (workspace_id == nullptr || !workspace_id->is_string() ||
            workspace_id->get<std::string>() != *expected_id) {
            return std::nullopt;
        }
    }
    const Json* grants = member(document, "grants");
    if (grants == nullptr) {
        return std::vector<PolicyRule>{};
    }
    if (!grants->is_array()) {
        return std::nullopt;
    }

    std::vector<PolicyRule> rules;
    for (const Json& entry : *grants) {
        if (!entry.is_object()) {
            return std::nullopt;
        }
        for (auto it = entry.begin(); it != entry.end(); ++it) {
            const std::string_view key = it.key();
            if (key != "tool" && key != "command" && key != "match" && key != "effect" &&
                key != "id") {
                return std::nullopt;
            }
        }
        const Json* tool = member(entry, "tool");
        if (tool == nullptr || !tool->is_string() || tool->get<std::string>().empty()) {
            return std::nullopt;
        }
        PolicyRule rule;
        rule.tool = tool->get<std::string>();
        if (const Json* command = member(entry, "command"); command != nullptr) {
            if (!command->is_string()) {
                return std::nullopt;
            }
            rule.command = command->get<std::string>();
        }
        bool literal = true;
        if (const Json* match = member(entry, "match"); match != nullptr) {
            if (!match->is_string()) {
                return std::nullopt;
            }
            const std::string mode = match->get<std::string>();
            if (mode == "literal") {
                literal = true;
            } else if (mode == "glob") {
                literal = false;
            } else {
                return std::nullopt;
            }
        }
        if (!literal) {
            try {
                validate_glob(rule.command);
            } catch (const PolicyConfigError&) {
                return std::nullopt;
            }
        }
        rule.literal = literal;
        rule.effect  = PolicyVerdict::Allow;
        rule.layer   = PolicyRule::Layer::LocalGrant;
        if (const Json* id = member(entry, "id"); id != nullptr && id->is_string()) {
            rule.id = id->get<std::string>();
        }
        if (rule.id.empty()) {
            rule.id = derived_id(rule);
        }
        rules.push_back(std::move(rule));
    }
    return rules;
}

Json serialize_grants(const std::vector<PolicyRule>& rules,
                      const std::optional<std::string>& workspace_id) {
    Json grants = Json::array();
    for (const PolicyRule& rule : rules) {
        Json entry;
        entry["tool"] = rule.tool;
        if (!rule.command.empty()) {
            entry["command"] = rule.command;
        }
        entry["match"]  = rule.literal ? "literal" : "glob";
        entry["effect"] = "allow";
        if (!rule.id.empty()) {
            entry["id"] = rule.id;
        }
        grants.push_back(std::move(entry));
    }
    Json document;
    document["schema"] = std::string{kSchema};
    if (workspace_id.has_value()) {
        document["workspace_id"] = *workspace_id;
    }
    document["grants"] = std::move(grants);
    return document;
}

class FileGrantStore final : public GrantStore {
public:
    FileGrantStore(std::filesystem::path path, std::optional<std::string> expected_id)
        : path_(std::move(path)), expected_id_(std::move(expected_id)) {}

    std::vector<PolicyRule> load() override {
        std::error_code error;
        if (!std::filesystem::exists(path_, error) || error) {
            return {};
        }
        std::ifstream input{path_, std::ios::binary};
        if (!input) {
            return {};
        }
        const std::string text{std::istreambuf_iterator<char>(input),
                               std::istreambuf_iterator<char>()};
        if (input.bad()) {
            return {};
        }
        std::string         parse_error;
        std::optional<Json> document = parse_jsonc_document(text, parse_error);
        if (!document.has_value()) {
            log_skip(path_, parse_error.empty() ? std::string_view{"no document"} : parse_error);
            return {};
        }
        std::optional<std::vector<PolicyRule>> rules = parse_grants(*document, expected_id_);
        if (!rules.has_value()) {
            log_skip(path_, "invalid grants document");
            return {};
        }
        return std::move(*rules);
    }

    bool append(const PolicyRule& grant) override {
        std::error_code error;
        std::filesystem::create_directories(path_.parent_path(), error);

        const int fd = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd < 0) {
            return false;
        }
        if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
            ::close(fd);
            return false;
        }

        std::vector<PolicyRule> rules;
        std::string             text;
        char                    buffer[4096];
        ssize_t                 count = 0;
        bool                    read_ok = true;
        while ((count = ::read(fd, buffer, sizeof(buffer))) > 0) {
            text.append(buffer, static_cast<std::size_t>(count));
        }
        if (count < 0) {
            read_ok = false;
        }
        if (read_ok && !text.empty()) {
            std::string         parse_error;
            std::optional<Json> document = parse_jsonc_document(text, parse_error);
            if (document.has_value()) {
                if (auto parsed = parse_grants(*document, expected_id_); parsed.has_value()) {
                    rules = std::move(*parsed);
                }
            }
        }

        const bool ok = read_ok && [&] {
            rules.erase(std::remove_if(rules.begin(), rules.end(),
                                       [&grant](const PolicyRule& rule) {
                                           return !rule.id.empty() && rule.id == grant.id;
                                       }),
                        rules.end());
            rules.push_back(grant);
            const std::string body = serialize_grants(rules, expected_id_).dump(2) + "\n";
            return write_atomically(body);
        }();

        (void)::flock(fd, LOCK_UN);
        ::close(fd);
        return ok;
    }

private:
    bool write_atomically(const std::string& body) {
        const std::filesystem::path temp =
            path_.string() + ".tmp." + std::to_string(::getpid());
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
        if (::rename(temp.c_str(), path_.c_str()) != 0) {
            (void)::unlink(temp.c_str());
            return false;
        }
        const std::filesystem::path directory =
            path_.parent_path().empty() ? std::filesystem::path{"."} : path_.parent_path();
        const int dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir_fd >= 0) {
            (void)::fsync(dir_fd);
            ::close(dir_fd);
        }
        return true;
    }

    std::filesystem::path        path_;
    std::optional<std::string>   expected_id_;
};

} // namespace

std::unique_ptr<GrantStore> open_file_grant_store(
    std::filesystem::path grants_path, std::optional<std::string> expected_workspace_id) {
    return std::make_unique<FileGrantStore>(std::move(grants_path),
                                            std::move(expected_workspace_id));
}

} // namespace ymh
