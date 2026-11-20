#include "ymh/session/session_persistence.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#include <nlohmann/json.hpp>

#include "ymh/session/errors.hpp"
#include "ymh/session/session.hpp"

namespace ymh {
namespace {

// The durable session store is append-only: one immutable event log per session
// plus a small header file that is atomically replaced only when the mutable
// projection (title/model) changes. There is no database, no WAL, and no
// cross-process transaction engine.
constexpr int         kLayoutFormat      = 1;
constexpr int         kLegacyApplication = 0x594D4801;
constexpr std::string_view kLayoutMarker = "layout-v1";
constexpr std::string_view kStoreDirName = "sessions";

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// GCC's -Wunused-result is not silenced by a `(void)` cast on functions
// declared with warn_unused_result (write/ftruncate/fdatasync on the office
// toolchain). Passing the result to an unnamed parameter marks it used without
// inventing a dead variable, while keeping the best-effort calls explicit.
template <typename T>
void ignore_result(T&&) noexcept {}

std::int64_t to_epoch_ms(std::chrono::system_clock::time_point point) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(point.time_since_epoch()).count();
}

std::chrono::system_clock::time_point from_epoch_ms(std::int64_t ms) {
    return std::chrono::system_clock::time_point{std::chrono::milliseconds{ms}};
}

void write_all_fd(int fd, std::string_view bytes) {
    std::size_t written = 0;
    while (written < bytes.size()) {
        const ssize_t count =
            ::write(fd, bytes.data() + written, bytes.size() - written);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw StoreError(std::string{"write failed: "} + std::strerror(errno));
        }
        if (count == 0) {
            throw StoreError("write returned 0 bytes");
        }
        written += static_cast<std::size_t>(count);
    }
}

std::optional<std::string> read_file(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return std::nullopt;
        }
        throw StoreError("cannot open " + path.string() + ": " + std::strerror(errno));
    }
    std::string bytes;
    char        buffer[16 * 1024];
    while (true) {
        const ssize_t count = ::read(fd, buffer, sizeof buffer);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            const std::string error = std::strerror(errno);
            ::close(fd);
            throw StoreError("cannot read " + path.string() + ": " + error);
        }
        if (count == 0) {
            break;
        }
        bytes.append(buffer, static_cast<std::size_t>(count));
    }
    ::close(fd);
    return bytes;
}

void fsync_directory(const std::filesystem::path& directory) noexcept {
    const int fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return;
    }
    ignore_result(::fsync(fd));
    ::close(fd);
}

// Header files are tiny; the temp+rename keeps a reader from observing a
// half-written projection. fsync-before-rename is required on NFS, where a
// rename can otherwise publish a zero-length file.
void atomic_write(const std::filesystem::path& path, std::string_view bytes) {
    static std::atomic<std::uint64_t> counter{0};
    const std::filesystem::path directory = path.parent_path();
    std::error_code             error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        throw StoreError("cannot create directory " + directory.string() + ": " +
                         error.message());
    }

    const std::string temp_name =
        "." + path.filename().string() + ".tmp." + std::to_string(::getpid()) + "." +
        std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
    const std::filesystem::path temp = directory / temp_name;

    const int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw StoreError("cannot create " + temp.string() + ": " + std::strerror(errno));
    }
    try {
        write_all_fd(fd, bytes);
        if (::fsync(fd) != 0) {
            throw StoreError("cannot fsync " + temp.string() + ": " + std::strerror(errno));
        }
        ::close(fd);
    } catch (...) {
        const int saved = errno;
        ::close(fd);
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        errno = saved;
        throw;
    }

    std::error_code rename_error;
    std::filesystem::rename(temp, path, rename_error);
    if (rename_error) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        throw StoreError("cannot replace " + path.string() + ": " + rename_error.message());
    }
    fsync_directory(directory);
}

bool is_uuid_like_component(const std::string& value) {
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

std::filesystem::path session_dir_of(const std::filesystem::path& store_dir,
                                     const SessionId&            id) {
    if (!is_uuid_like_component(id.value)) {
        throw CorruptionError("invalid session id: " + id.value);
    }
    return store_dir / id.value;
}

std::filesystem::path header_path_of(const std::filesystem::path& store_dir,
                                     const SessionId&            id) {
    return session_dir_of(store_dir, id) / "header.json";
}

std::filesystem::path events_path_of(const std::filesystem::path& store_dir,
                                     const SessionId&            id) {
    return session_dir_of(store_dir, id) / "events.jsonl";
}

struct HeaderEnvelope {
    SessionHeader header;
    Sequence      base_sequence = 0;
};

bool has_newline_terminator(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    struct stat status {};
    bool        terminated = false;
    if (::fstat(fd, &status) == 0 && status.st_size > 0) {
        char last = '\0';
        if (::pread(fd, &last, 1, status.st_size - 1) == 1) {
            terminated = last == '\n';
        }
    }
    ::close(fd);
    return terminated;
}

// Returns the last complete JSON line. A trailing partial line (a crash in the
// middle of write) is ignored. A malformed complete line is corruption.
std::optional<nlohmann::json> read_last_json_line(const std::filesystem::path& path) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            return std::nullopt;
        }
        throw StoreError("cannot open " + path.string() + ": " + std::strerror(errno));
    }
    struct stat status {};
    if (::fstat(fd, &status) != 0 || status.st_size == 0) {
        ::close(fd);
        return std::nullopt;
    }
    const bool terminated = has_newline_terminator(path);

    std::string tail;
    off_t       position = status.st_size;
    std::size_t newlines = 0;
    char        buffer[4096];
    while (position > 0 && newlines < 2) {
        const std::size_t wanted =
            std::min<std::size_t>(sizeof buffer, static_cast<std::size_t>(position));
        position -= static_cast<off_t>(wanted);
        const ssize_t count = ::pread(fd, buffer, wanted, position);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            const std::string error = std::strerror(errno);
            ::close(fd);
            throw StoreError("cannot read " + path.string() + ": " + error);
        }
        tail.insert(0, buffer, static_cast<std::size_t>(count));
        newlines = static_cast<std::size_t>(
            std::count(tail.begin(), tail.end(), '\n'));
    }
    ::close(fd);
    if (tail.empty()) {
        return std::nullopt;
    }

    std::size_t end = tail.size();
    if (terminated && end > 0 && tail[end - 1] == '\n') {
        --end;
    } else if (!terminated) {
        const std::size_t partial = tail.rfind('\n');
        if (partial == std::string::npos) {
            return std::nullopt;
        }
        end = partial;
    }
    if (end == 0) {
        return std::nullopt;
    }
    const std::size_t previous = tail.rfind('\n', end - 1);
    const std::size_t begin    = previous == std::string::npos ? 0 : previous + 1;
    const std::string line     = tail.substr(begin, end - begin);
    if (line.empty()) {
        return std::nullopt;
    }
    try {
        return nlohmann::json::parse(line);
    } catch (const nlohmann::json::exception&) {
        if (!terminated) {
            return std::nullopt;
        }
        throw CorruptionError("malformed event record in " + path.string());
    }
}

std::optional<Sequence> last_own_sequence(const std::filesystem::path& events) {
    const std::optional<nlohmann::json> line = read_last_json_line(events);
    if (!line.has_value() || !line->contains("seq") || !line->at("seq").is_number_integer()) {
        return std::nullopt;
    }
    return line->at("seq").get<Sequence>();
}

std::optional<std::int64_t> last_event_timestamp(const std::filesystem::path& events) {
    const std::optional<nlohmann::json> line = read_last_json_line(events);
    if (!line.has_value() || !line->contains("timestamp") ||
        !line->at("timestamp").is_number_integer()) {
        return std::nullopt;
    }
    return line->at("timestamp").get<std::int64_t>();
}

// Calls `consume` for each complete event line. Stops when `consume` returns
// false. A trailing partial line is ignored; corruption in the middle throws.
void for_each_event(const std::filesystem::path& events,
                    const std::function<bool(const EventRecord&)>& consume) {
    std::ifstream stream{events, std::ios::binary};
    if (!stream) {
        return;
    }
    const bool          terminated = has_newline_terminator(events);
    std::string         line;
    Sequence            previous = 0;
    while (std::getline(stream, line)) {
        const bool at_eof = stream.eof();
        if (line.empty()) {
            if (at_eof && !terminated) {
                break;
            }
            continue;
        }
        nlohmann::json parsed;
        try {
            parsed = nlohmann::json::parse(line);
        } catch (const nlohmann::json::exception&) {
            if (at_eof && !terminated) {
                break;
            }
            throw CorruptionError("malformed event record in " + events.string());
        }
        try {
            EventRecord record;
            if (!parsed.contains("seq") || !parsed.at("seq").is_number_integer()) {
                throw CorruptionError("event record has no sequence in " + events.string());
            }
            record.seq   = parsed.at("seq").get<Sequence>();
            record.event = parsed.get<Event>();
            const std::string expected = events.parent_path().filename().string();
            if (!expected.empty() && record.event.session_id.value != expected) {
                throw CorruptionError("event session id does not match directory in " +
                                      events.string());
            }
            if (record.seq <= previous) {
                throw CorruptionError("event log is not ascending by Sequence");
            }
            previous = record.seq;
            if (!consume(record)) {
                return;
            }
        } catch (const nlohmann::json::exception& error) {
            throw CorruptionError("malformed event record in " + events.string() + ": " +
                                  error.what());
        }
    }
}

EventRange read_events_after(const std::filesystem::path& events, Sequence after,
                             std::size_t limit) {
    EventRange result;
    if (limit == 0) {
        return result;
    }
    const bool unbounded = limit >= kUnbounded;
    for_each_event(events, [&](const EventRecord& record) {
        if (record.seq <= after) {
            return true;
        }
        result.push_back(record);
        return unbounded || result.size() < limit;
    });
    return result;
}

HeaderEnvelope read_envelope(const std::filesystem::path& header) {
    const std::optional<std::string> bytes = read_file(header);
    if (!bytes.has_value()) {
        throw UnknownSession("unknown session: " + header.parent_path().filename().string());
    }
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(*bytes);
    } catch (const nlohmann::json::exception& error) {
        throw CorruptionError("malformed session header " + header.string() + ": " +
                              error.what());
    }
    HeaderEnvelope envelope;
    if (parsed.contains("header")) {
        const int format = parsed.value("format", kLayoutFormat);
        if (format > kLayoutFormat) {
            throw SchemaVersionError("session store layout is newer than this binary");
        }
        envelope.base_sequence = parsed.value("base_sequence", Sequence{0});
        envelope.header        = parsed.at("header").get<SessionHeader>();
    } else {
        // Forward-compatible read of a bare header written by an early build.
        envelope.header = parsed.get<SessionHeader>();
    }
    return envelope;
}

void write_envelope(const std::filesystem::path& directory, const SessionHeader& header,
                    Sequence base_sequence) {
    nlohmann::json payload{
        {"format", kLayoutFormat},
        {"base_sequence", base_sequence},
        {"header", header},
    };
    atomic_write(directory / "header.json", payload.dump() + "\n");
}

std::string event_line(const EventRecord& record) {
    nlohmann::json payload = record.event;
    payload["seq"]         = record.seq;
    return payload.dump() + "\n";
}

bool is_durable_boundary(EventType type) {
    return type == EventType::TurnEnded || type == EventType::TurnFailed ||
           type == EventType::TurnCancelled || type == EventType::SessionEnded;
}

// A crash can leave the final append half-written. The partial suffix is not an
// event (the caller never received a sequence for it), so the next writer
// truncates it before appending. This runs only on the single-writer path.
void repair_trailing_partial_line(int fd, const std::filesystem::path& path) {
    struct stat status {};
    if (::fstat(fd, &status) != 0) {
        throw StoreError("cannot stat " + path.string() + ": " + std::strerror(errno));
    }
    if (status.st_size == 0) {
        return;
    }
    char last = '\0';
    if (::pread(fd, &last, 1, status.st_size - 1) != 1) {
        throw StoreError("cannot read " + path.string() + ": " + std::strerror(errno));
    }
    if (last == '\n') {
        return;
    }
    off_t       position = status.st_size;
    std::string chunk;
    char        buffer[4096];
    while (position > 0) {
        const std::size_t wanted =
            std::min<std::size_t>(sizeof buffer, static_cast<std::size_t>(position));
        position -= static_cast<off_t>(wanted);
        const ssize_t count = ::pread(fd, buffer, wanted, position);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw StoreError("cannot read " + path.string() + ": " + std::strerror(errno));
        }
        chunk.insert(0, buffer, static_cast<std::size_t>(count));
        const std::size_t newline = chunk.rfind('\n');
        if (newline != std::string::npos) {
            const off_t cut = position + static_cast<off_t>(newline) + 1;
            if (::ftruncate(fd, cut) != 0) {
                throw StoreError("cannot repair " + path.string() + ": " +
                                 std::strerror(errno));
            }
            return;
        }
    }
    if (::ftruncate(fd, 0) != 0) {
        throw StoreError("cannot repair " + path.string() + ": " + std::strerror(errno));
    }
}

// ---- legacy SQLite import -------------------------------------------------

[[noreturn]] void throw_sqlite(sqlite3* db, const std::string& context) {
    throw StoreError(context + ": " + sqlite3_errmsg(db));
}

class Statement {
public:
    Statement(sqlite3* db, const char* sql) : db_(db) {
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt_, nullptr) != SQLITE_OK) {
            throw_sqlite(db_, std::string{"prepare failed: "} + sql);
        }
    }

    ~Statement() {
        if (stmt_ != nullptr) {
            sqlite3_finalize(stmt_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    void bindText(int index, const std::string& value) {
        if (sqlite3_bind_text(stmt_, index, value.c_str(), static_cast<int>(value.size()),
                              SQLITE_TRANSIENT) != SQLITE_OK) {
            throw_sqlite(db_, "bind text failed");
        }
    }

    void bindInt64(int index, std::int64_t value) {
        if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
            throw_sqlite(db_, "bind int failed");
        }
    }

    int step() {
        const int rc = sqlite3_step(stmt_);
        if (rc != SQLITE_ROW && rc != SQLITE_DONE) {
            throw_sqlite(db_, "step failed");
        }
        return rc;
    }

    [[nodiscard]] std::string columnText(int index) const {
        const unsigned char* text = sqlite3_column_text(stmt_, index);
        if (text == nullptr) {
            return {};
        }
        const int bytes = sqlite3_column_bytes(stmt_, index);
        return std::string{reinterpret_cast<const char*>(text), static_cast<std::size_t>(bytes)};
    }

    [[nodiscard]] std::int64_t columnInt64(int index) const {
        return static_cast<std::int64_t>(sqlite3_column_int64(stmt_, index));
    }

    [[nodiscard]] bool columnIsNull(int index) const {
        return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
    }

    [[nodiscard]] std::optional<std::string> columnOptionalText(int index) const {
        if (columnIsNull(index)) {
            return std::nullopt;
        }
        return columnText(index);
    }

    [[nodiscard]] std::optional<std::int64_t> columnOptionalInt64(int index) const {
        if (columnIsNull(index)) {
            return std::nullopt;
        }
        return columnInt64(index);
    }

private:
    sqlite3*      db_   = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

constexpr std::string_view kMetadataKey          = "metadata";
constexpr std::string_view kAgentPresetKey       = "agent_preset";
constexpr std::string_view kDepthKey             = "depth";
constexpr std::string_view kPermissionPresetKey  = "permission_preset";
constexpr std::string_view kModelNameKey         = "model_name";

void decode_metadata_column(const std::optional<std::string>& raw, SessionHeader& header) {
    header.metadata          = std::nullopt;
    header.agent_preset      = std::nullopt;
    header.depth             = 0;
    header.permission_preset = std::nullopt;
    header.model_name        = std::nullopt;
    if (!raw.has_value()) {
        return;
    }
    nlohmann::json object;
    try {
        object = nlohmann::json::parse(*raw);
    } catch (const nlohmann::json::exception&) {
        header.metadata = *raw;
        return;
    }
    if (!object.is_object()) {
        header.metadata = *raw;
        return;
    }
    const bool has_envelope = object.contains(std::string{kMetadataKey});
    const bool has_reserved =
        object.contains(std::string{kAgentPresetKey}) || object.contains(std::string{kDepthKey}) ||
        object.contains(std::string{kPermissionPresetKey}) ||
        object.contains(std::string{kModelNameKey});
    if (!has_envelope) {
        if (has_reserved) {
            throw CorruptionError(
                "sessions.metadata carries a reserved key without the opaque envelope");
        }
        header.metadata = *raw;
        return;
    }
    const nlohmann::json& bag = object.at(std::string{kMetadataKey});
    if (bag.is_null()) {
        header.metadata = std::nullopt;
    } else if (bag.is_string()) {
        header.metadata = bag.get<std::string>();
    } else {
        throw CorruptionError("sessions.metadata opaque bag must be a string or null");
    }
    if (object.contains(std::string{kAgentPresetKey})) {
        header.agent_preset = object.at(std::string{kAgentPresetKey}).get<std::string>();
    }
    if (object.contains(std::string{kPermissionPresetKey})) {
        header.permission_preset =
            object.at(std::string{kPermissionPresetKey}).get<std::string>();
    }
    if (object.contains(std::string{kModelNameKey})) {
        header.model_name = object.at(std::string{kModelNameKey}).get<std::string>();
    }
    if (object.contains(std::string{kDepthKey})) {
        const std::uint64_t value = object.at(std::string{kDepthKey}).get<std::uint64_t>();
        if (value > std::numeric_limits<std::uint32_t>::max()) {
            throw CorruptionError("sessions.metadata depth is out of range");
        }
        header.depth = static_cast<std::uint32_t>(value);
    }
}

SessionHeader read_legacy_header_row(const Statement& statement) {
    SessionHeader header;
    header.id.value      = statement.columnText(0);
    header.cwd           = std::filesystem::path{statement.columnText(1)};
    header.createdAt     = statement.columnInt64(2);
    header.updatedAt     = statement.columnInt64(3);
    header.title         = statement.columnText(4);
    header.model         = statement.columnText(5);
    header.serverProfile = statement.columnText(6);
    header.kind = parse_session_kind(statement.columnText(7)).value_or(SessionKind::Root);
    if (const auto parent = statement.columnOptionalText(8)) {
        header.parentSession = SessionId{*parent};
    }
    if (const auto seed = statement.columnOptionalInt64(9)) {
        header.seedLength = static_cast<std::size_t>(*seed);
    }
    decode_metadata_column(statement.columnOptionalText(10), header);
    return header;
}

std::filesystem::path store_dir_of(const PersistenceConfig& config) {
    return config.db_path.parent_path() / std::string{kStoreDirName};
}

std::filesystem::path workspace_of(const PersistenceConfig& config) {
    return config.db_path.parent_path().parent_path();
}

bool marker_exists(const std::filesystem::path& store_dir) {
    std::error_code error;
    return std::filesystem::is_regular_file(store_dir / std::string{kLayoutMarker}, error) &&
           !error;
}

int read_layout_version(const std::filesystem::path& store_dir) {
    const std::optional<std::string> bytes =
        read_file(store_dir / std::string{kLayoutMarker});
    if (!bytes.has_value()) {
        return 0;
    }
    try {
        const nlohmann::json parsed = nlohmann::json::parse(*bytes);
        if (parsed.is_object() && parsed.contains("format")) {
            return parsed.at("format").get<int>();
        }
    } catch (const nlohmann::json::exception&) {
        throw CorruptionError("malformed session layout marker");
    }
    return kLayoutFormat;
}

void write_layout_marker(const std::filesystem::path& store_dir) {
    atomic_write(store_dir / std::string{kLayoutMarker},
                 nlohmann::json{{"format", kLayoutFormat}}.dump() + "\n");
}

bool legacy_db_exists(const PersistenceConfig& config) {
    std::error_code error;
    return std::filesystem::is_regular_file(config.db_path, error) && !error;
}

int acquire_lock_retrying(const std::filesystem::path& lock_path,
                          std::chrono::milliseconds budget) {
    const int fd = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) {
        throw StoreOpenError(StoreOpenErrorCode::Unavailable,
                             "cannot open session store lock: " + lock_path.string());
    }
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (true) {
        if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
            return fd;
        }
        if (errno != EWOULDBLOCK && errno != EAGAIN) {
            const std::string error = std::strerror(errno);
            ::close(fd);
            throw StoreOpenError(StoreOpenErrorCode::Unavailable,
                                 "cannot lock session store: " + error);
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            ::close(fd);
            throw StoreOpenError(StoreOpenErrorCode::Locked,
                                 "session store is locked: " + lock_path.string());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{25});
    }
}

void migrate_legacy_store(const PersistenceConfig& config) {
    const std::filesystem::path store_dir = store_dir_of(config);
    if (marker_exists(store_dir) || !legacy_db_exists(config)) {
        return;
    }
    std::error_code             error;
    std::filesystem::create_directories(store_dir, error);
    if (error) {
        throw StoreOpenError(StoreOpenErrorCode::Unavailable,
                             "cannot create session store directory: " + store_dir.string());
    }
    std::filesystem::permissions(store_dir, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(config.db_path.c_str(), &db,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const std::string message = db != nullptr ? sqlite3_errmsg(db) : "open failed";
        if (db != nullptr) {
            sqlite3_close_v2(db);
        }
        throw StoreOpenError(StoreOpenErrorCode::Unavailable,
                             "cannot open legacy session store: " + message);
    }

    struct Closer {
        sqlite3* db;
        ~Closer() {
            if (db != nullptr) {
                sqlite3_close_v2(db);
            }
        }
    } closer{db};

    char* checkpoint_message = nullptr;
    ignore_result(sqlite3_exec(db, "PRAGMA wal_checkpoint(TRUNCATE)", nullptr, nullptr,
                               &checkpoint_message));
    sqlite3_free(checkpoint_message);

    try {
        {
            Statement application{db, "PRAGMA application_id"};
            if (application.step() != SQLITE_ROW ||
                application.columnInt64(0) != kLegacyApplication) {
                throw SchemaVersionError("legacy session store is not a ymh database");
            }
        }
        {
            Statement version{db, "PRAGMA user_version"};
            if (version.step() != SQLITE_ROW || version.columnInt64(0) != 1) {
                throw SchemaVersionError("legacy session store schema does not match");
            }
        }

        Statement sessions{
            db,
            "SELECT id, cwd, created_at, updated_at, title, model, server_profile, kind, "
            "parent_session, seed_length, metadata FROM sessions ORDER BY created_at, id"};
        while (sessions.step() == SQLITE_ROW) {
            const SessionHeader header = read_legacy_header_row(sessions);
            const std::filesystem::path directory = session_dir_of(store_dir, header.id);
            std::error_code             directory_error;
            const bool                  header_exists =
                std::filesystem::is_regular_file(directory / "header.json", directory_error);
            if (header_exists) {
                continue;
            }
            std::filesystem::remove_all(directory, directory_error);
            if (!std::filesystem::create_directories(directory, directory_error)) {
                if (directory_error) {
                    throw StoreError("cannot create " + directory.string() + ": " +
                                     directory_error.message());
                }
            }
            std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace,
                                         directory_error);

            const std::filesystem::path events = directory / "events.jsonl";
            const int                   fd =
                ::open(events.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0) {
                throw StoreError("cannot create " + events.string() + ": " +
                                 std::strerror(errno));
            }

            Sequence maximum = 0;
            try {
                Statement rows{
                    db,
                    "SELECT sequence, event_id, timestamp, type, payload FROM events "
                    "WHERE session_id = ? ORDER BY sequence"};
                rows.bindText(1, header.id.value);
                while (rows.step() == SQLITE_ROW) {
                    const auto parsed_type = parse_event_type(rows.columnText(3));
                    if (!parsed_type.has_value()) {
                        throw CorruptionError("undecodable event type: " + rows.columnText(3));
                    }
                    Event event;
                    event.id.value         = rows.columnText(1);
                    event.session_id.value = header.id.value;
                    event.timestamp        = from_epoch_ms(rows.columnInt64(2));
                    event.type             = *parsed_type;
                    try {
                        event.payload = nlohmann::json::parse(rows.columnText(4));
                    } catch (const nlohmann::json::exception& error) {
                        throw CorruptionError("malformed event payload: " +
                                              std::string{error.what()});
                    }
                    EventRecord record;
                    record.seq   = rows.columnInt64(0);
                    record.event = std::move(event);
                    maximum      = std::max(maximum, record.seq);
                    const std::string line = event_line(record);
                    write_all_fd(fd, line);
                }
                ignore_result(::fdatasync(fd));
                ::close(fd);
            } catch (...) {
                ::close(fd);
                throw;
            }
            write_envelope(directory, header, maximum);
        }

        write_layout_marker(store_dir);
    } catch (...) {
        throw;
    }

    sqlite3_close_v2(db);
    closer.db = nullptr;

    // Keep the legacy database as a backup. The marker is authoritative; a
    // stale sessions.db left beside it is ignored.
    const std::filesystem::path retired = config.db_path.string() + ".migrated";
    std::error_code             rename_error;
    std::filesystem::rename(config.db_path, retired, rename_error);
    if (!rename_error) {
        for (const char* suffix : {"-wal", "-shm"}) {
            std::error_code ignored;
            std::filesystem::rename(config.db_path.string() + suffix,
                                    retired.string() + suffix, ignored);
        }
    }
}

} // namespace

class SessionPersistence::Impl {
public:
    PersistenceConfig           config;
    std::filesystem::path       store_dir;
    bool                        writable = false;
    bool                        closed   = false;
    int                         lock_fd  = -1;
    mutable std::mutex          mutex;
    std::set<std::string>       leases;

    struct CachedSession {
        SessionHeader header;
        Sequence      base_sequence = 0;
        Sequence      next_sequence = 0;
        bool          next_known    = false;
        int           events_fd     = -1;
    };
    mutable std::map<std::string, CachedSession> cache;

    static std::unique_ptr<Impl> create(const PersistenceConfig& config, bool writable);

    ~Impl() { close_noexcept(); }

    void close_noexcept() noexcept {
        if (closed) {
            return;
        }
        closed = true;
        for (auto& [id, cached] : cache) {
            if (cached.events_fd >= 0) {
                ::close(cached.events_fd);
                cached.events_fd = -1;
            }
        }
        cache.clear();
        leases.clear();
        if (lock_fd >= 0) {
            ignore_result(::flock(lock_fd, LOCK_UN));
            ::close(lock_fd);
            lock_fd = -1;
        }
    }

    [[nodiscard]] std::optional<HeaderEnvelope> load_envelope(const SessionId& id) const {
        const std::filesystem::path header = header_path_of(store_dir, id);
        std::error_code             error;
        if (!std::filesystem::is_regular_file(header, error)) {
            return std::nullopt;
        }
        HeaderEnvelope envelope = read_envelope(header);
        if (const auto timestamp = last_event_timestamp(events_path_of(store_dir, id));
            timestamp.has_value()) {
            envelope.header.updatedAt = *timestamp;
        }
        return envelope;
    }

    [[nodiscard]] CachedSession& cached_session(const HeaderEnvelope& envelope) {
        auto [iterator, inserted] = cache.try_emplace(envelope.header.id.value);
        if (inserted || iterator->second.header.id.value.empty()) {
            iterator->second.header        = envelope.header;
            iterator->second.base_sequence = envelope.base_sequence;
        }
        return iterator->second;
    }

    [[nodiscard]] Sequence next_sequence(const SessionId& id) {
        const std::optional<HeaderEnvelope> envelope = load_envelope(id);
        if (!envelope.has_value()) {
            throw UnknownSession("unknown session: " + id.value);
        }
        CachedSession& cached = cached_session(*envelope);
        if (cached.next_known) {
            return cached.next_sequence;
        }
        Sequence next = cached.base_sequence + 1;
        if (const auto last = last_own_sequence(events_path_of(store_dir, id));
            last.has_value()) {
            next = std::max(next, *last + 1);
        } else if (cached.header.kind == SessionKind::Fork &&
                   cached.header.seedLength.value_or(0) > 0) {
            std::vector<std::string> stack;
            const EventRange prefix =
                resolve_after(*cached.header.parentSession, 0,
                              cached.header.seedLength.value_or(0), stack);
            if (!prefix.empty()) {
                next = std::max(next, prefix.back().seq + 1);
            }
        }
        cached.next_sequence = next;
        cached.next_known    = true;
        return next;
    }

    [[nodiscard]] EventRange resolve_after(const SessionId& id, Sequence after,
                                           std::size_t limit,
                                           std::vector<std::string>& stack) const {
        const std::optional<HeaderEnvelope> envelope = load_envelope(id);
        if (!envelope.has_value()) {
            throw UnknownSession("unknown session: " + id.value);
        }
        if (std::find(stack.begin(), stack.end(), id.value) != stack.end()) {
            throw CorruptionError("fork cycle detected at session " + id.value);
        }
        if (limit == 0) {
            return {};
        }
        stack.push_back(id.value);

        EventRange result;
        const SessionHeader& header = envelope->header;
        if (header.kind == SessionKind::Fork) {
            if (!header.parentSession.has_value()) {
                throw CorruptionError("fork without parent: " + id.value);
            }
            const std::size_t seed = header.seedLength.value_or(0);
            if (seed > 0) {
                const EventRange prefix = resolve_after(*header.parentSession, 0, seed, stack);
                if (prefix.size() < seed) {
                    throw CorruptionError("fork seed exceeds parent view: " + id.value);
                }
                for (const EventRecord& record : prefix) {
                    if (record.seq > after) {
                        result.push_back(record);
                        if (result.size() >= limit) {
                            break;
                        }
                    }
                }
            }
        }
        if (result.size() < limit) {
            const EventRange own =
                read_events_after(events_path_of(store_dir, id), after, limit - result.size());
            result.insert(result.end(), own.begin(), own.end());
        }
        stack.pop_back();
        return result;
    }

    [[nodiscard]] int events_fd(const SessionId& id) {
        auto iterator = cache.find(id.value);
        if (iterator == cache.end()) {
            throw UnknownSession("unknown session: " + id.value);
        }
        if (iterator->second.events_fd < 0) {
            const std::filesystem::path events = events_path_of(store_dir, id);
            const int fd = ::open(events.c_str(), O_RDWR | O_APPEND | O_CLOEXEC, 0600);
            if (fd < 0) {
                throw StoreError("cannot open " + events.string() + ": " +
                                 std::strerror(errno));
            }
            iterator->second.events_fd = fd;
        }
        return iterator->second.events_fd;
    }

    [[nodiscard]] std::vector<Sequence> append_batch_locked(
        const SessionId& id, std::span<const Event> events, bool require_lease) {
        if (!writable) {
            throw StoreOpenError("store is read-only");
        }
        if (events.empty()) {
            return {};
        }
        const std::optional<HeaderEnvelope> envelope = load_envelope(id);
        if (!envelope.has_value()) {
            throw UnknownSession("unknown session: " + id.value);
        }
        if (require_lease && !leases.contains(id.value)) {
            throw LeaseLost("not the lease holder for session " + id.value);
        }
        for (const Event& event : events) {
            if (event.payload.dump().size() > config.max_payload_bytes) {
                throw PayloadTooLarge("event payload exceeds the configured cap");
            }
            if (event.session_id.value != id.value) {
                throw std::invalid_argument("append: event does not belong to session " +
                                            id.value);
            }
        }

        CachedSession& cached = cached_session(*envelope);
        Sequence       next   = next_sequence(id);
        const int      fd     = events_fd(id);
        repair_trailing_partial_line(fd, events_path_of(store_dir, id));
        off_t          start  = ::lseek(fd, 0, SEEK_END);
        if (start < 0) {
            throw StoreError("cannot seek " + events_path_of(store_dir, id).string());
        }

        std::vector<Sequence> sequences;
        sequences.reserve(events.size());
        bool durable = false;
        bool projection_changed = false;
        try {
            for (const Event& event : events) {
                const Sequence sequence = next++;
                EventRecord    record;
                record.seq   = sequence;
                record.event = event;
                const std::string line = event_line(record);
                write_all_fd(fd, line);
                sequences.push_back(sequence);
                durable = durable || is_durable_boundary(event.type);
                cached.header.updatedAt = to_epoch_ms(event.timestamp);
                if (event.type == EventType::SessionRenamed) {
                    cached.header.title =
                        event.payload.get<payload::SessionRenamed>().title;
                    projection_changed = true;
                } else if (event.type == EventType::SessionModelChanged) {
                    const auto& change = event.payload.get<payload::SessionModelChanged>();
                    cached.header.model      = change.model;
                    cached.header.model_name = change.model_name.empty()
                                                   ? std::nullopt
                                                   : std::optional<std::string>{change.model_name};
                    projection_changed = true;
                }
            }
            cached.next_sequence = next;
            cached.next_known    = true;
            if (durable) {
                ignore_result(::fdatasync(fd));
            }
        } catch (...) {
            ignore_result(::ftruncate(fd, start));
            throw;
        }
        if (projection_changed) {
            write_envelope(session_dir_of(store_dir, id), cached.header,
                           cached.base_sequence);
        }
        return sequences;
    }

    [[nodiscard]] bool has_dependents_locked(const SessionId& id) const {
        std::error_code error;
        if (!std::filesystem::is_directory(store_dir, error)) {
            return false;
        }
        const std::filesystem::directory_iterator end;
        for (std::filesystem::directory_iterator iterator{store_dir, error}; !error &&
             iterator != end; iterator.increment(error)) {
            const std::filesystem::path header = iterator->path() / "header.json";
            if (!std::filesystem::is_regular_file(header, error)) {
                error.clear();
                continue;
            }
            const HeaderEnvelope envelope = read_envelope(header);
            if (envelope.header.parentSession.has_value() &&
                envelope.header.parentSession->value == id.value) {
                return true;
            }
        }
        return false;
    }
};

std::unique_ptr<SessionPersistence::Impl> SessionPersistence::Impl::create(
    const PersistenceConfig& config, bool writable) {
    std::unique_ptr<Impl> owner{new Impl()};
    Impl&                 impl = *owner;
    impl.config                = config;
    impl.store_dir             = store_dir_of(config);
    impl.writable              = writable;

    const std::filesystem::path workspace = workspace_of(config);
    std::error_code             error;
    if (!std::filesystem::is_directory(workspace, error)) {
        throw StoreOpenError("workspace root does not exist or is not a directory: " +
                             workspace.string());
    }

    if (writable) {
        std::filesystem::create_directories(config.db_path.parent_path(), error);
        if (error) {
            throw StoreOpenError("cannot create state directory: " +
                                 config.db_path.parent_path().string());
        }
        std::filesystem::permissions(config.db_path.parent_path(),
                                     std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, error);
        impl.lock_fd =
            ::open(config.lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (impl.lock_fd < 0) {
            throw StoreOpenError("cannot open sidecar lock: " + config.lock_path.string());
        }
        if (::flock(impl.lock_fd, LOCK_EX | LOCK_NB) != 0) {
            const int lock_error = errno;
            ::close(impl.lock_fd);
            impl.lock_fd = -1;
            if (lock_error == EWOULDBLOCK || lock_error == EAGAIN) {
                throw StoreOpenError(StoreOpenErrorCode::Locked,
                                     "workspace is locked by another daemon: " +
                                         config.lock_path.string());
            }
            throw StoreOpenError(StoreOpenErrorCode::Unavailable,
                                 "cannot acquire sidecar lock: " + config.lock_path.string());
        }
        nlohmann::json diagnostic{{"pid", ::getpid()},
                                  {"boot_id", config.boot_id.value},
                                  {"acquired_at", now_ms()}};
        if (!config.build.empty()) {
            diagnostic["build"] = config.build;
        }
        if (!config.config_digest.empty()) {
            diagnostic["config_digest"] = config.config_digest;
        }
        const std::string text = diagnostic.dump();
        ignore_result(::ftruncate(impl.lock_fd, 0));
        ignore_result(::write(impl.lock_fd, text.data(), text.size()));
    }

    const bool needs_migration = !marker_exists(impl.store_dir) && legacy_db_exists(config);
    if (needs_migration) {
        if (writable) {
            migrate_legacy_store(config);
        } else {
            const int migration_lock =
                acquire_lock_retrying(config.lock_path, std::chrono::milliseconds{2'000});
            try {
                migrate_legacy_store(config);
            } catch (...) {
                ignore_result(::flock(migration_lock, LOCK_UN));
                ::close(migration_lock);
                throw;
            }
            ignore_result(::flock(migration_lock, LOCK_UN));
            ::close(migration_lock);
        }
    }

    if (!marker_exists(impl.store_dir)) {
        if (writable) {
            std::filesystem::create_directories(impl.store_dir, error);
            if (error) {
                throw StoreOpenError("cannot create session store: " + impl.store_dir.string());
            }
            std::filesystem::permissions(impl.store_dir, std::filesystem::perms::owner_all,
                                         std::filesystem::perm_options::replace, error);
            write_layout_marker(impl.store_dir);
        } else {
            std::error_code exists_error;
            if (std::filesystem::exists(impl.store_dir, exists_error)) {
                const bool non_empty = std::filesystem::directory_iterator{impl.store_dir,
                                                                           exists_error} !=
                                       std::filesystem::directory_iterator{};
                if (!exists_error && non_empty) {
                    throw SchemaVersionError("session store has no layout marker");
                }
            }
        }
    } else {
        const int version = read_layout_version(impl.store_dir);
        if (version <= 0 || version > kLayoutFormat) {
            throw SchemaVersionError("session store layout is newer than this binary");
        }
    }

    return owner;
}

SessionPersistence::SessionPersistence(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

SessionPersistence::~SessionPersistence() = default;

std::unique_ptr<SessionPersistence> SessionPersistence::open(const PersistenceConfig& config) {
    return std::unique_ptr<SessionPersistence>{new SessionPersistence(Impl::create(config, true))};
}

std::unique_ptr<SessionPersistence> SessionPersistence::openReadOnly(
    const PersistenceConfig& config) {
    return std::unique_ptr<SessionPersistence>{
        new SessionPersistence(Impl::create(config, false))};
}

SessionHeader SessionPersistence::create(SessionHeader header) {
    validateHeader(header);
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }

    const std::filesystem::path directory = session_dir_of(impl_->store_dir, header.id);
    std::error_code             error;
    if (!std::filesystem::create_directories(directory, error)) {
        if (error) {
            throw StoreError("cannot create " + directory.string() + ": " + error.message());
        }
        throw CorruptionError("session already exists: " + header.id.value);
    }
    std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, error);

    Sequence base = 0;
    if (header.kind == SessionKind::Fork && header.seedLength.value_or(0) > 0) {
        if (!header.parentSession.has_value()) {
            throw CorruptionError("fork without parent: " + header.id.value);
        }
        std::vector<std::string> stack;
        const std::size_t        seed = header.seedLength.value_or(0);
        const EventRange         prefix =
            impl_->resolve_after(*header.parentSession, 0, seed, stack);
        if (prefix.size() < seed) {
            throw CorruptionError("fork seed exceeds parent view: " + header.id.value);
        }
        if (!prefix.empty()) {
            base = prefix.back().seq;
        }
    }

    const std::filesystem::path events = directory / "events.jsonl";
    const int                   fd =
        ::open(events.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        std::filesystem::remove_all(directory, error);
        throw StoreError("cannot create " + events.string() + ": " + std::strerror(errno));
    }
    ignore_result(::fdatasync(fd));
    ::close(fd);

    try {
        write_envelope(directory, header, base);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
        throw;
    }

    Impl::CachedSession cached;
    cached.header        = header;
    cached.base_sequence = base;
    cached.next_sequence = base + 1;
    cached.next_known    = true;
    impl_->cache[header.id.value] = cached;
    impl_->leases.insert(header.id.value);
    return header;
}

std::optional<SessionHeader> SessionPersistence::load(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::optional<HeaderEnvelope> envelope = impl_->load_envelope(id);
    if (!envelope.has_value()) {
        return std::nullopt;
    }
    return envelope->header;
}

std::vector<SessionHeader> SessionPersistence::list() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<SessionHeader> headers;
    std::error_code            error;
    if (!std::filesystem::is_directory(impl_->store_dir, error)) {
        return headers;
    }
    const std::filesystem::directory_iterator end;
    for (std::filesystem::directory_iterator iterator{impl_->store_dir, error};
         !error && iterator != end; iterator.increment(error)) {
        const std::filesystem::path header = iterator->path() / "header.json";
        if (!std::filesystem::is_regular_file(header, error)) {
            error.clear();
            continue;
        }
        const HeaderEnvelope envelope = read_envelope(header);
        SessionHeader        item     = envelope.header;
        if (const auto timestamp =
                last_event_timestamp(iterator->path() / "events.jsonl");
            timestamp.has_value()) {
            item.updatedAt = *timestamp;
        }
        headers.push_back(std::move(item));
    }
    std::sort(headers.begin(), headers.end(), [](const SessionHeader& left,
                                                 const SessionHeader& right) {
        if (left.createdAt != right.createdAt) {
            return left.createdAt < right.createdAt;
        }
        return left.id.value < right.id.value;
    });
    return headers;
}

void SessionPersistence::erase(SessionId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    if (impl_->has_dependents_locked(id)) {
        throw DependentSessionError("session has dependent children: " + id.value);
    }
    auto iterator = impl_->cache.find(id.value);
    if (iterator != impl_->cache.end()) {
        if (iterator->second.events_fd >= 0) {
            ::close(iterator->second.events_fd);
        }
        impl_->cache.erase(iterator);
    }
    impl_->leases.erase(id.value);
    std::error_code error;
    std::filesystem::remove_all(session_dir_of(impl_->store_dir, id), error);
    if (error) {
        throw StoreError("cannot erase session " + id.value + ": " + error.message());
    }
}

EventRange SessionPersistence::read(SessionId id, Sequence after) const {
    return readAfter(id, after, kUnbounded);
}

EventRange SessionPersistence::readAfter(SessionId id, Sequence after,
                                         std::size_t limit) const {
    std::lock_guard<std::mutex>       lock(impl_->mutex);
    std::vector<std::string>          stack;
    return impl_->resolve_after(id, after, limit, stack);
}

EventRange SessionPersistence::readRange(SessionId id, Sequence from, Sequence to) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<std::string>    stack;
    const EventRange            resolved = impl_->resolve_after(id, 0, kUnbounded, stack);
    EventRange                  result;
    for (const EventRecord& record : resolved) {
        if (record.seq >= from && record.seq <= to) {
            result.push_back(record);
        }
    }
    return result;
}

Sequence SessionPersistence::append(SessionId id, Event event) {
    const Event                 single[] = {std::move(event)};
    const std::vector<Sequence> sequences = appendBatch(id, single);
    return sequences.front();
}

std::vector<Sequence> SessionPersistence::appendBatch(SessionId id,
                                                      std::span<const Event> events) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->append_batch_locked(id, events, true);
}

bool SessionPersistence::isLeaseHolder(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->writable && impl_->leases.contains(id.value);
}

bool SessionPersistence::isUnprompted(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    bool                        prompted = false;
    for_each_event(events_path_of(impl_->store_dir, id), [&](const EventRecord& record) {
        if (record.event.type == EventType::UserMessage) {
            prompted = true;
            return false;
        }
        return true;
    });
    return !prompted;
}

bool SessionPersistence::hasDependents(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->has_dependents_locked(id);
}

Sequence SessionPersistence::eraseWithEvent(SessionId id, Event event) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    if (impl_->has_dependents_locked(id)) {
        throw DependentSessionError("session has dependent children: " + id.value);
    }
    const std::vector<Sequence> sequences =
        impl_->append_batch_locked(id, std::span<const Event>{&event, 1}, false);
    const Sequence sequence = sequences.front();
    auto           iterator = impl_->cache.find(id.value);
    if (iterator != impl_->cache.end()) {
        if (iterator->second.events_fd >= 0) {
            ::close(iterator->second.events_fd);
        }
        impl_->cache.erase(iterator);
    }
    impl_->leases.erase(id.value);
    std::error_code error;
    std::filesystem::remove_all(session_dir_of(impl_->store_dir, id), error);
    if (error) {
        throw StoreError("cannot erase session " + id.value + ": " + error.message());
    }
    return sequence;
}

Sequence SessionPersistence::headSequence(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const std::optional<HeaderEnvelope> envelope = impl_->load_envelope(id);
    if (!envelope.has_value()) {
        throw UnknownSession("unknown session: " + id.value);
    }
    if (const auto last = last_own_sequence(events_path_of(impl_->store_dir, id));
        last.has_value()) {
        return *last;
    }
    if (envelope->header.kind == SessionKind::Fork &&
        envelope->header.seedLength.value_or(0) > 0) {
        std::vector<std::string> stack;
        const EventRange         prefix = impl_->resolve_after(
            *envelope->header.parentSession, 0,
            envelope->header.seedLength.value_or(0), stack);
        if (!prefix.empty()) {
            return prefix.back().seq;
        }
    }
    return 0;
}

LeaseState SessionPersistence::leaseState(SessionId id) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->writable && impl_->leases.contains(id.value) ? LeaseState::HeldByMe
                                                               : LeaseState::Absent;
}

bool SessionPersistence::acquireLease(SessionId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    impl_->leases.insert(id.value);
    return true;
}

void SessionPersistence::renewLeases() {
    // A process-held store is the lease. There is no TTL to renew.
}

bool SessionPersistence::releaseLease(SessionId id) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!impl_->writable) {
        throw StoreOpenError("store is read-only");
    }
    const bool released = impl_->leases.erase(id.value) > 0;
    auto       iterator = impl_->cache.find(id.value);
    if (iterator != impl_->cache.end() && iterator->second.events_fd >= 0) {
        ::close(iterator->second.events_fd);
        iterator->second.events_fd = -1;
    }
    return released;
}

void SessionPersistence::checkpoint(SessionId) {
    // Snapshots are no longer part of the store; the append-only log is the
    // sole resume path.
}

std::optional<SessionSnapshot> SessionPersistence::loadSnapshot(SessionId) const {
    return std::nullopt;
}

bool SessionPersistence::snapshotIsCurrent(SessionId) const { return false; }

void SessionPersistence::discardSnapshot(SessionId) {}

int SessionPersistence::schemaVersion() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return kLayoutFormat;
}

void SessionPersistence::close() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->close_noexcept();
}

SqliteSessionHandle::SqliteSessionHandle(SessionPersistence& store, SessionId id, bool writable)
    : store_(store), id_(std::move(id)), writable_(writable) {}

SqliteSessionHandle::~SqliteSessionHandle() {
    release();
}

void SqliteSessionHandle::release() {
    if (!writable_ || released_) {
        return;
    }
    released_ = true;
    store_.releaseLease(id_);
}

} // namespace ymh
