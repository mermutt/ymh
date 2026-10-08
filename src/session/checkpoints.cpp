#include "ymh/session/checkpoints.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "ymh/core/fs.hpp"
#include "ymh/core/logging.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/filesystem.hpp"
#include "ymh/llm/llm_runtime.hpp"

namespace ymh {
namespace {

constexpr int         kCheckpointSchemaVersion  = 1;
constexpr std::size_t kCheckpointKeepPerSession = 100;
constexpr auto        kCheckpointRetention      = std::chrono::hours{24 * 30};

void log_checkpoint(LogLevel level, const std::string& message) {
    category_logger(LogCategory::Session).log(level, message);
}

std::int64_t to_ms(const std::chrono::system_clock::time_point time_point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               time_point.time_since_epoch())
        .count();
}

std::int64_t mtime_ms_of(const std::filesystem::path& path) {
    std::error_code ec;
    const auto      time = std::filesystem::last_write_time(path, ec);
    if (ec) {
        return 0;
    }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               time.time_since_epoch())
        .count();
}

const char* kind_name(PreImageKind kind) {
    switch (kind) {
        case PreImageKind::Content: return "content";
        case PreImageKind::Absent:  return "absent";
        case PreImageKind::Symlink: return "symlink";
        case PreImageKind::Failed:  return "failed";
    }
    return "failed";
}

std::optional<PreImageKind> parse_kind(const std::string& value) {
    if (value == "content") {
        return PreImageKind::Content;
    }
    if (value == "absent") {
        return PreImageKind::Absent;
    }
    if (value == "symlink") {
        return PreImageKind::Symlink;
    }
    if (value == "failed") {
        return PreImageKind::Failed;
    }
    return std::nullopt;
}

bool write_all(int fd, std::string_view body) {
    std::size_t written = 0;
    while (written < body.size()) {
        const ssize_t count = ::write(fd, body.data() + written, body.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        written += static_cast<std::size_t>(count);
    }
    return true;
}

bool replace_atomically(const std::filesystem::path& target, std::string_view body) {
    const std::filesystem::path temp =
        target.string() + ".tmp." + std::to_string(::getpid());
    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        return false;
    }
    bool ok = write_all(fd, body);
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
    if (::rename(temp.c_str(), target.c_str()) != 0) {
        (void)::unlink(temp.c_str());
        return false;
    }
    fsync_parent_directory(target);
    return true;
}

bool write_blob(const std::filesystem::path& target, std::string_view body) {
    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        return true;
    }
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        return false;
    }
    const std::filesystem::path temp =
        target.string() + ".tmp." + std::to_string(::getpid());
    int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0 && errno == EEXIST) {
        (void)::unlink(temp.c_str());
        fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    }
    if (fd < 0) {
        return false;
    }
    bool ok = write_all(fd, body);
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
    if (::rename(temp.c_str(), target.c_str()) != 0) {
        (void)::unlink(temp.c_str());
        return false;
    }
    fsync_parent_directory(target);
    return true;
}

bool has_file_key(const Checkpoint& checkpoint, const std::string& key) {
    return std::any_of(checkpoint.files.begin(), checkpoint.files.end(),
                       [&key](const CheckpointFile& file) { return file.path == key; });
}

}

std::string checkpoint_blob_id(std::string_view bytes) {
    return sha256_hex(bytes);
}

std::optional<std::filesystem::path>
first_symlink_component(const std::filesystem::path& candidate) {
    std::filesystem::path prefix = candidate.root_path();
    for (const auto& component : candidate.relative_path()) {
        prefix /= component;
        std::error_code  ec;
        const auto       status = std::filesystem::symlink_status(prefix, ec);
        if (!ec && std::filesystem::is_symlink(status)) {
            return prefix;
        }
    }
    std::error_code ec;
    if (std::filesystem::hard_link_count(candidate, ec) > 1 && !ec) {
        return candidate;
    }
    return std::nullopt;
}

std::string workspace_relative(const std::filesystem::path& workspace_root,
                               const std::filesystem::path& path) {
    return path.lexically_relative(workspace_root).generic_string();
}

CheckpointStore::CheckpointStore(std::filesystem::path workspace_root,
                                 std::function<std::string(std::string_view)> hash)
    : workspace_root_(std::move(workspace_root)),
      store_dir_(workspace_root_ / ".ymh" / "checkpoints"),
      hash_(hash ? std::move(hash)
                 : std::function<std::string(std::string_view)>(&checkpoint_blob_id)) {}

void CheckpointStore::ensure_loaded_locked() const {
    if (loaded_) {
        return;
    }

    const std::filesystem::path index = store_dir_ / "index.json";
    std::string                 text;
    const int                   fd = ::open(index.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            loaded_ = true;
            return;
        }
        throw CheckpointUnavailableError("cannot open checkpoint index: " + index.string());
    }
    bool      ok  = true;
    char      buf[65536];
    for (;;) {
        const ssize_t count = ::read(fd, buf, sizeof(buf));
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            ok = false;
            break;
        }
        if (count == 0) {
            break;
        }
        text.append(buf, static_cast<std::size_t>(count));
    }
    (void)::close(fd);
    if (!ok) {
        throw CheckpointUnavailableError("cannot read checkpoint index: " + index.string());
    }

    std::map<std::string, std::vector<Checkpoint>> parsed;
    bool                                           corrupt = false;
    try {
        const nlohmann::json doc = nlohmann::json::parse(text);
        if (!doc.is_object() || doc.at("version").get<int>() != kCheckpointSchemaVersion) {
            corrupt = true;
        } else {
            const nlohmann::json& sessions = doc.at("sessions");
            if (!sessions.is_object()) {
                corrupt = true;
            } else {
                for (auto it = sessions.begin(); it != sessions.end(); ++it) {
                    const nlohmann::json& checkpoints = it.value().at("checkpoints");
                    if (!checkpoints.is_array()) {
                        corrupt = true;
                        break;
                    }
                    std::vector<Checkpoint> decoded;
                    for (const nlohmann::json& source : checkpoints) {
                        Checkpoint checkpoint;
                        checkpoint.session      = SessionId{it.key()};
                        checkpoint.turn         = source.at("turn").get<TurnId>();
                        checkpoint.created_at_ms = source.at("created_at_ms").get<std::int64_t>();
                        checkpoint.incomplete   = source.value("incomplete", false);
                        for (const nlohmann::json& file : source.at("files")) {
                            CheckpointFile entry;
                            entry.path     = file.at("path").get<std::string>();
                            entry.kind     = parse_kind(file.at("kind").get<std::string>()).value();
                            entry.blob     = file.value("blob", std::string{});
                            entry.size     = file.value("size", std::int64_t{0});
                            entry.mtime_ms = file.value("mtime_ms", std::int64_t{0});
                            checkpoint.files.push_back(std::move(entry));
                        }
                        decoded.push_back(std::move(checkpoint));
                    }
                    parsed[it.key()] = std::move(decoded);
                }
            }
        }
    } catch (const std::exception&) {
        corrupt = true;
        parsed.clear();
    }

    if (corrupt) {
        const std::filesystem::path quarantined =
            index.string() + ".corrupt." + std::to_string(to_ms(std::chrono::system_clock::now()));
        std::error_code ec;
        std::filesystem::rename(index, quarantined, ec);
        log_checkpoint(LogLevel::Warn,
                       "checkpoint index corrupt; quarantined to " + quarantined.string());
        sessions_.clear();
        loaded_ = true;
        return;
    }

    sessions_ = std::move(parsed);
    loaded_   = true;
}

TurnId CheckpointStore::oldest_retained_turn_locked(const std::string& session) const {
    const auto it = sessions_.find(session);
    if (it == sessions_.end() || it->second.empty()) {
        return 0;
    }
    TurnId oldest = it->second.front().turn;
    for (const Checkpoint& checkpoint : it->second) {
        oldest = std::min(oldest, checkpoint.turn);
    }
    return oldest;
}

std::optional<Checkpoint> CheckpointStore::find_locked(const std::string& session,
                                                       TurnId              turn) const {
    const auto it = sessions_.find(session);
    if (it == sessions_.end()) {
        return std::nullopt;
    }
    for (const Checkpoint& checkpoint : it->second) {
        if (checkpoint.turn == turn) {
            return checkpoint;
        }
    }
    return std::nullopt;
}

void CheckpointStore::evict_locked(const std::string&                   session,
                                   std::chrono::system_clock::time_point now) {
    const auto it = sessions_.find(session);
    if (it == sessions_.end()) {
        return;
    }
    std::vector<Checkpoint>& checkpoints = it->second;
    std::sort(checkpoints.begin(), checkpoints.end(),
              [](const Checkpoint& left, const Checkpoint& right) {
                  return left.turn < right.turn;
              });
    if (checkpoints.size() > kCheckpointKeepPerSession) {
        checkpoints.erase(checkpoints.begin(),
                          checkpoints.begin() +
                              static_cast<std::ptrdiff_t>(checkpoints.size() -
                                                          kCheckpointKeepPerSession));
    }
    const std::int64_t retention_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(kCheckpointRetention).count();
    const std::int64_t now_value = to_ms(now);
    while (!checkpoints.empty() &&
           (now_value - checkpoints.front().created_at_ms) > retention_ms) {
        checkpoints.erase(checkpoints.begin());
    }
    if (checkpoints.empty()) {
        sessions_.erase(it);
    }
}

void CheckpointStore::capture(const SessionId&                             session,
                              TurnId                                       turn,
                              const std::vector<std::filesystem::path>&   paths,
                              const ExecutionEnvironment&                  env) {
    if (paths.empty() || turn == 0) {
        return;
    }
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_loaded_locked();

        std::vector<Checkpoint>& checkpoints = sessions_[session.value];
        if (!find_locked(session.value, turn).has_value()) {
            Checkpoint created;
            created.session       = session;
            created.turn          = turn;
            created.created_at_ms = to_ms(std::chrono::system_clock::now());
            checkpoints.push_back(std::move(created));
        }
        Checkpoint* checkpoint = nullptr;
        for (Checkpoint& candidate : checkpoints) {
            if (candidate.turn == turn) {
                checkpoint = &candidate;
                break;
            }
        }

        const std::size_t read_limit = env.toolConfig().read_file_max_bytes;
        for (const std::filesystem::path& raw : paths) {
            if (raw.empty()) {
                continue;
            }
            const std::filesystem::path candidate =
                raw.is_absolute() ? raw : workspace_root_ / raw;

            if (first_symlink_component(candidate).has_value()) {
                const std::string key =
                    workspace_relative(workspace_root_, candidate.lexically_normal());
                if (!has_file_key(*checkpoint, key)) {
                    CheckpointFile entry;
                    entry.path = key;
                    entry.kind = PreImageKind::Symlink;
                    checkpoint->files.push_back(std::move(entry));
                }
                continue;
            }

            std::filesystem::path resolved;
            try {
                resolved = env.resolve(raw.string());
            } catch (const ToolError&) {
                const std::string key =
                    workspace_relative(workspace_root_, candidate.lexically_normal());
                if (!has_file_key(*checkpoint, key)) {
                    CheckpointFile entry;
                    entry.path = key;
                    entry.kind = PreImageKind::Failed;
                    checkpoint->files.push_back(std::move(entry));
                }
                checkpoint->incomplete = true;
                continue;
            }

            const std::string key = workspace_relative(workspace_root_, resolved);
            if (has_file_key(*checkpoint, key)) {
                continue;
            }

            std::error_code ec;
            if (!std::filesystem::exists(resolved, ec)) {
                CheckpointFile entry;
                entry.path = key;
                entry.kind = PreImageKind::Absent;
                checkpoint->files.push_back(std::move(entry));
                continue;
            }

            const std::uintmax_t size = std::filesystem::file_size(resolved, ec);
            if (ec || size > read_limit) {
                CheckpointFile entry;
                entry.path = key;
                entry.kind = PreImageKind::Failed;
                checkpoint->files.push_back(std::move(entry));
                checkpoint->incomplete = true;
                continue;
            }

            std::ifstream input(resolved, std::ios::binary);
            std::string   bytes((std::istreambuf_iterator<char>(input)),
                              std::istreambuf_iterator<char>());
            if (input.bad() || bytes.size() != size) {
                CheckpointFile entry;
                entry.path = key;
                entry.kind = PreImageKind::Failed;
                checkpoint->files.push_back(std::move(entry));
                checkpoint->incomplete = true;
                continue;
            }

            const std::string blob = hash_(bytes);
            const std::filesystem::path blob_path =
                store_dir_ / "blobs" / blob.substr(0, 2) / blob;
            CheckpointFile entry;
            entry.path = key;
            if (std::filesystem::exists(blob_path, ec) || write_blob(blob_path, bytes)) {
                entry.kind     = PreImageKind::Content;
                entry.blob     = blob;
                entry.size     = static_cast<std::int64_t>(bytes.size());
                entry.mtime_ms = mtime_ms_of(resolved);
            } else {
                entry.kind             = PreImageKind::Failed;
                checkpoint->incomplete = true;
            }
            checkpoint->files.push_back(std::move(entry));
        }

        persist_locked();
        evict_locked(session.value, std::chrono::system_clock::now());
        collect_garbage_locked();
        persist_locked();
    } catch (const std::exception& error) {
        log_checkpoint(LogLevel::Warn,
                       std::string("checkpoint capture failed: ") + error.what());
    }
}

std::int64_t CheckpointStore::revertible_count(const SessionId& session, TurnId turn) const {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_loaded_locked();
        const auto it = sessions_.find(session.value);
        if (it == sessions_.end() || it->second.empty()) {
            return 0;
        }
        if (turn < oldest_retained_turn_locked(session.value)) {
            return 0;
        }
        std::set<std::string> paths;
        for (const Checkpoint& checkpoint : it->second) {
            if (checkpoint.turn < turn) {
                continue;
            }
            for (const CheckpointFile& file : checkpoint.files) {
                paths.insert(file.path);
            }
        }
        return static_cast<std::int64_t>(paths.size());
    } catch (const std::exception& error) {
        log_checkpoint(LogLevel::Warn,
                       std::string("checkpoint index unreadable: ") + error.what());
        return 0;
    }
}

CheckpointRestoreReport CheckpointStore::restore(ExecutionEnvironment& env,
                                                 const SessionId&      session,
                                                 TurnId                turn) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_loaded_locked();

    CheckpointRestoreReport report;
    const auto              it = sessions_.find(session.value);
    if (it == sessions_.end() || it->second.empty()) {
        report.detail = "nothing to restore";
        return report;
    }
    if (turn < oldest_retained_turn_locked(session.value)) {
        report.expired = true;
        report.detail  = "checkpoint expired; code restore unavailable";
        return report;
    }

    std::vector<const Checkpoint*> ordered;
    for (const Checkpoint& checkpoint : it->second) {
        if (checkpoint.turn >= turn) {
            ordered.push_back(&checkpoint);
        }
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const Checkpoint* left, const Checkpoint* right) {
                  return left->turn < right->turn;
              });
    std::map<std::string, CheckpointFile> restore_set;
    for (const Checkpoint* checkpoint : ordered) {
        for (const CheckpointFile& file : checkpoint->files) {
            restore_set.emplace(file.path, file);
        }
    }

    if (restore_set.empty()) {
        report.detail = "nothing to restore";
        return report;
    }

    for (const auto& [path, entry] : restore_set) {
        const std::filesystem::path candidate = workspace_root_ / path;
        if (first_symlink_component(candidate).has_value()) {
            ++report.skipped;
            continue;
        }

        std::filesystem::path resolved;
        try {
            resolved = env.resolve(path);
        } catch (const ToolError&) {
            ++report.failed;
            continue;
        }

        if (entry.kind == PreImageKind::Failed) {
            ++report.failed;
            continue;
        }
        if (entry.kind == PreImageKind::Symlink) {
            ++report.skipped;
            continue;
        }
        if (entry.kind == PreImageKind::Absent) {
            try {
                env.fs().remove(resolved).get();
                ++report.restored;
            } catch (const ToolError& error) {
                if (error.code() == ToolErrorCode::NotFound) {
                    ++report.restored;
                } else {
                    ++report.failed;
                }
            }
            continue;
        }

        const std::filesystem::path blob_path =
            store_dir_ / "blobs" / entry.blob.substr(0, 2) / entry.blob;
        std::ifstream input(blob_path, std::ios::binary);
        if (!input) {
            ++report.failed;
            continue;
        }
        std::string bytes((std::istreambuf_iterator<char>(input)),
                          std::istreambuf_iterator<char>());
        if (input.bad()) {
            ++report.failed;
            continue;
        }

        std::error_code     ec;
        const std::uintmax_t current_size = std::filesystem::file_size(resolved, ec);
        if (ec || static_cast<std::int64_t>(current_size) != entry.size ||
            mtime_ms_of(resolved) != entry.mtime_ms) {
            ++report.changed;
        }
        try {
            env.fs().write(resolved, Data{std::move(bytes)}).get();
            ++report.restored;
        } catch (const std::exception&) {
            ++report.failed;
        }
    }

    if (report.restored == 0 && report.failed > 0) {
        report.detail = "No files were restored: " + std::to_string(report.failed) +
                        " files failed (backup missing, or the file could not be updated)";
    }
    if (report.changed > 0) {
        if (!report.detail.empty()) {
            report.detail += " ";
        }
        report.detail += "(" + std::to_string(report.changed) +
                         " files changed since the checkpoint)";
    }
    return report;
}

void CheckpointStore::removeSession(const SessionId& session) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_loaded_locked();
        sessions_.erase(session.value);
        persist_locked();
        collect_garbage_locked();
    } catch (const std::exception& error) {
        log_checkpoint(LogLevel::Warn,
                       std::string("checkpoint removeSession failed: ") + error.what());
    }
}

void CheckpointStore::sweep(const std::set<SessionId>&             live_sessions,
                            std::chrono::system_clock::time_point now) {
    try {
        std::lock_guard<std::mutex> lock(mutex_);
        ensure_loaded_locked();
    } catch (const std::exception& error) {
        log_checkpoint(LogLevel::Warn,
                       std::string("checkpoint sweep skipped (index unreadable): ") +
                           error.what());
        dirty_ = true;
        return;
    }

    try {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            if (live_sessions.count(SessionId{it->first}) == 0) {
                it = sessions_.erase(it);
            } else {
                ++it;
            }
        }

        std::vector<std::string> keys;
        keys.reserve(sessions_.size());
        for (const auto& [key, checkpoints] : sessions_) {
            (void)checkpoints;
            keys.push_back(key);
        }
        for (const std::string& key : keys) {
            evict_locked(key, now);
        }

        std::error_code ec;
        if (std::filesystem::is_directory(store_dir_, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(store_dir_, ec)) {
                const std::string name = entry.path().filename().string();
                if (name.rfind("index.json.tmp.", 0) == 0) {
                    std::filesystem::remove(entry.path(), ec);
                }
            }
        }

        persist_locked();
        collect_garbage_locked();
    } catch (const std::exception& error) {
        log_checkpoint(LogLevel::Warn,
                       std::string("checkpoint sweep failed: ") + error.what());
        dirty_ = true;
    }
}

void CheckpointStore::persist_locked() {
    nlohmann::json document;
    document["version"]     = kCheckpointSchemaVersion;
    document["sessions"]    = nlohmann::json::object();
    nlohmann::json& sessions = document["sessions"];
    for (const auto& [key, checkpoints] : sessions_) {
        if (checkpoints.empty()) {
            continue;
        }
        nlohmann::json encoded = nlohmann::json::array();
        for (const Checkpoint& checkpoint : checkpoints) {
            nlohmann::json files = nlohmann::json::array();
            for (const CheckpointFile& file : checkpoint.files) {
                files.push_back(nlohmann::json{{"path", file.path},
                                               {"kind", kind_name(file.kind)},
                                               {"blob", file.blob},
                                               {"size", file.size},
                                               {"mtime_ms", file.mtime_ms}});
            }
            encoded.push_back(nlohmann::json{{"turn", checkpoint.turn},
                                             {"created_at_ms", checkpoint.created_at_ms},
                                             {"incomplete", checkpoint.incomplete},
                                             {"files", std::move(files)}});
        }
        sessions[key] = nlohmann::json{{"checkpoints", std::move(encoded)}};
    }

    std::error_code ec;
    std::filesystem::create_directories(store_dir_, ec);
    const std::string body = document.dump(2) + "\n";
    if (!replace_atomically(store_dir_ / "index.json", body)) {
        log_checkpoint(LogLevel::Warn, "checkpoint index persist failed; will retry");
        dirty_ = true;
        return;
    }
    dirty_ = false;
}

void CheckpointStore::collect_garbage_locked() {
    std::set<std::string> referenced;
    for (const auto& [key, checkpoints] : sessions_) {
        (void)key;
        for (const Checkpoint& checkpoint : checkpoints) {
            for (const CheckpointFile& file : checkpoint.files) {
                if (file.kind == PreImageKind::Content && !file.blob.empty()) {
                    referenced.insert(file.blob);
                }
            }
        }
    }

    std::error_code ec;
    const std::filesystem::path blobs = store_dir_ / "blobs";
    if (!std::filesystem::is_directory(blobs, ec)) {
        return;
    }
    for (const auto& shard : std::filesystem::directory_iterator(blobs, ec)) {
        if (!shard.is_directory()) {
            continue;
        }
        for (const auto& entry : std::filesystem::directory_iterator(shard.path(), ec)) {
            const std::string name = entry.path().filename().string();
            if (name.find(".tmp.") != std::string::npos || referenced.count(name) == 0) {
                std::filesystem::remove(entry.path(), ec);
            }
        }
        std::filesystem::remove(shard.path(), ec);
    }
}

}
