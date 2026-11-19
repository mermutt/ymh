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

std::optional<std::string> read_file_text(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::exists(path, error) || error) {
        return std::nullopt;
    }
    std::ifstream input{path, std::ios::binary};
    if (!input) {
        return std::nullopt;
    }
    std::string text{std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>()};
    if (input.bad()) {
        return std::nullopt;
    }
    return text;
}

// The envelope shared by grants and roots: schema tag plus, when known, the
// workspace id guard.
bool valid_envelope(const Json& document, const std::optional<std::string>& expected_id) {
    if (!document.is_object()) {
        return false;
    }
    const Json* schema = member(document, "schema");
    if (schema == nullptr || !schema->is_string() || schema->get<std::string>() != kSchema) {
        return false;
    }
    if (expected_id.has_value()) {
        const Json* workspace_id = member(document, "workspace_id");
        if (workspace_id == nullptr || !workspace_id->is_string() ||
            workspace_id->get<std::string>() != *expected_id) {
            return false;
        }
    }
    return true;
}

// Parses the optional `roots` array. `nullopt` means the value is malformed
// (fail-safe: the caller ignores the stored roots); an absent key is empty.
std::optional<std::vector<std::filesystem::path>> parse_roots_value(const Json& document) {
    const Json* roots = member(document, "roots");
    if (roots == nullptr) {
        return std::vector<std::filesystem::path>{};
    }
    if (!roots->is_array()) {
        return std::nullopt;
    }
    std::vector<std::filesystem::path> paths;
    paths.reserve(roots->size());
    for (const Json& entry : *roots) {
        if (!entry.is_string()) {
            return std::nullopt;
        }
        paths.emplace_back(entry.get<std::string>());
    }
    return paths;
}

// Parses a grants document into `LocalGrant` rules. `nullopt` means the
// document is not a valid grants file (the caller loads {} and re-asks).
std::optional<std::vector<PolicyRule>> parse_grants(
    const Json& document, const std::optional<std::string>& expected_id) {
    if (!valid_envelope(document, expected_id)) {
        return std::nullopt;
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
                key != "id" && key != "root") {
                return std::nullopt;
            }
        }
        const Json* tool = member(entry, "tool");
        if (tool == nullptr || !tool->is_string() || tool->get<std::string>().empty()) {
            return std::nullopt;
        }
        PolicyRule rule;
        rule.tool = tool->get<std::string>();
        // 90-A15/H1: no wildcard durable grant.
        if (rule.tool.find_first_of("*?[") != std::string::npos) {
            return std::nullopt;
        }
        if (const Json* command = member(entry, "command"); command != nullptr) {
            if (!command->is_string()) {
                return std::nullopt;
            }
            rule.command = command->get<std::string>();
        }
        // 90-A15: a shell/terminal grant must be bounded; a path-tool grant
        // has no command dimension (the shipped `Always` shape is empty).
        if (rule.tool == "shell" || rule.tool == "terminal") {
            if (rule.command.empty()) {
                return std::nullopt;
            }
        } else if (tool_has_path_dimension(rule.tool)) {
            if (!rule.command.empty()) {
                return std::nullopt;
            }
            rule.target_class = TargetClass::InRoot;
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
        if (!literal && (rule.tool == "shell" || rule.tool == "terminal")) {
            bool has_literal = false;
            for (char c : rule.command) {
                if (c != '*' && c != '?') {
                    has_literal = true;
                    break;
                }
            }
            if (!has_literal) {
                return std::nullopt;   // 90-A15/N16: match-all shell grant
            }
        }
        rule.effect  = PolicyVerdict::Allow;
        rule.layer   = PolicyRule::Layer::LocalGrant;
        if (const Json* id = member(entry, "id"); id != nullptr && id->is_string()) {
            rule.id = id->get<std::string>();
        }
        // Multi-root anchoring: only a string is a valid root; anything else
        // makes the whole document invalid (fail-safe re-ask).
        if (const Json* root = member(entry, "root"); root != nullptr) {
            if (!root->is_string()) {
                return std::nullopt;
            }
            rule.root = root->get<std::string>();
        }
        if (rule.id.empty()) {
            rule.id = derived_id(rule);
        }
        rules.push_back(std::move(rule));
    }
    return rules;
}

Json serialize_document(const std::vector<PolicyRule>&            rules,
                        const std::vector<std::filesystem::path>& roots,
                        const std::optional<std::string>&          workspace_id) {
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
        // Multi-root anchoring survives a restart with the grant.
        if (!rule.root.empty()) {
            entry["root"] = rule.root;
        }
        grants.push_back(std::move(entry));
    }
    Json root_array = Json::array();
    for (const std::filesystem::path& root : roots) {
        root_array.push_back(root.string());
    }
    Json document;
    document["schema"] = std::string{kSchema};
    if (workspace_id.has_value()) {
        document["workspace_id"] = *workspace_id;
    }
    document["grants"] = std::move(grants);
    document["roots"]  = std::move(root_array);
    return document;
}

class FileGrantStore final : public GrantStore {
public:
    FileGrantStore(std::filesystem::path path, std::optional<std::string> expected_id)
        : path_(std::move(path)), expected_id_(std::move(expected_id)) {}

    std::vector<PolicyRule> load() override {
        const std::optional<std::string> text = read_file_text(path_);
        if (!text.has_value()) {
            return {};
        }
        std::string         parse_error;
        std::optional<Json> document = parse_jsonc_document(*text, parse_error);
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

    std::vector<std::filesystem::path> load_roots() override {
        const std::optional<std::string> text = read_file_text(path_);
        if (!text.has_value()) {
            return {};
        }
        std::string         parse_error;
        std::optional<Json> document = parse_jsonc_document(*text, parse_error);
        if (!document.has_value() || !valid_envelope(*document, expected_id_)) {
            return {};
        }
        const std::optional<std::vector<std::filesystem::path>> roots =
            parse_roots_value(*document);
        return roots.has_value() ? *roots : std::vector<std::filesystem::path>{};
    }

    bool append(const PolicyRule& grant) override {
        return mutate([&grant](std::vector<PolicyRule>& rules,
                               std::vector<std::filesystem::path>& roots) {
            (void)roots;
            rules.erase(std::remove_if(rules.begin(), rules.end(),
                                       [&grant](const PolicyRule& rule) {
                                           return !rule.id.empty() && rule.id == grant.id;
                                       }),
                        rules.end());
            rules.push_back(grant);
        });
    }

    bool append_root(const std::filesystem::path& root) override {
        return mutate([&root](std::vector<PolicyRule>& rules,
                              std::vector<std::filesystem::path>& roots) {
            (void)rules;
            const std::string key = root.lexically_normal().string();
            for (const std::filesystem::path& existing : roots) {
                if (existing.lexically_normal().string() == key) {
                    return;
                }
            }
            roots.push_back(root);
        });
    }

private:
    // Opens, locks, reloads, applies `fn`, and writes both arrays back with the
    // same atomic temp/fsync/rename pattern as before. A missing or invalid
    // document starts from empty (exactly like the historical `append`).
    template <typename Fn>
    bool mutate(Fn fn) {
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

        std::vector<PolicyRule>            rules;
        std::vector<std::filesystem::path> roots;
        std::string                        text;
        char                               buffer[4096];
        ssize_t                            count = 0;
        bool                               read_ok = true;
        while ((count = ::read(fd, buffer, sizeof(buffer))) > 0) {
            text.append(buffer, static_cast<std::size_t>(count));
        }
        if (count < 0) {
            read_ok = false;
        }
        if (read_ok && !text.empty()) {
            std::string         parse_error;
            std::optional<Json> document = parse_jsonc_document(text, parse_error);
            if (document.has_value() && valid_envelope(*document, expected_id_)) {
                if (auto parsed = parse_grants(*document, expected_id_); parsed.has_value()) {
                    rules = std::move(*parsed);
                }
                if (auto parsed_roots = parse_roots_value(*document);
                    parsed_roots.has_value()) {
                    roots = std::move(*parsed_roots);
                }
            }
        }

        const bool ok = read_ok && [&] {
            fn(rules, roots);
            const std::string body = serialize_document(rules, roots, expected_id_).dump(2) + "\n";
            return write_atomically(body);
        }();

        (void)::flock(fd, LOCK_UN);
        ::close(fd);
        return ok;
    }

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
