#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

#include "ymh/core/event.hpp"
#include "ymh/session/session.hpp"
#include "ymh/session/session_persistence.hpp"

namespace ymh_test {

inline std::atomic<int> g_uuid_counter{1};

inline std::string uuid(int value) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    stream << std::setw(8) << (value & 0xffffffff) << "-0000-4000-8000-" << std::setw(12)
           << (value & 0xffffffffffff);
    return stream.str();
}

inline std::string fresh_uuid() {
    return uuid(g_uuid_counter.fetch_add(1));
}

struct TempWorkspace {
    std::filesystem::path root;

    TempWorkspace() {
        root = std::filesystem::temp_directory_path() /
               ("ymh-file-store-test-" + std::to_string(::getpid()) + "-" + fresh_uuid());
        std::error_code error;
        std::filesystem::remove_all(root, error);
        std::filesystem::create_directories(root);
    }

    ~TempWorkspace() {
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }

    TempWorkspace(const TempWorkspace&)            = delete;
    TempWorkspace& operator=(const TempWorkspace&) = delete;

    [[nodiscard]] ymh::PersistenceConfig persistence() const {
        ymh::PersistenceConfig config;
        config.db_path   = root / ".ymh" / "sessions.db";
        config.lock_path = root / ".ymh" / "sessions.lock";
        config.boot_id   = ymh::BootId{"test-boot"};
        return config;
    }
};

inline std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline ymh::SessionHeader root_header(const std::filesystem::path& root,
                                      const std::string&           id) {
    ymh::SessionHeader header;
    header.id.value      = id;
    header.cwd           = std::filesystem::canonical(root);
    header.createdAt     = now_ms();
    header.updatedAt     = header.createdAt;
    header.title         = "test title";
    header.model         = "test-model";
    header.serverProfile = "interactive";
    header.kind          = ymh::SessionKind::Root;
    return header;
}

inline ymh::SessionHeader fork_header(const std::filesystem::path& root,
                                      const std::string&           id,
                                      const std::string&           parent,
                                      std::size_t                  seed) {
    ymh::SessionHeader header = root_header(root, id);
    header.kind          = ymh::SessionKind::Fork;
    header.parentSession = ymh::SessionId{parent};
    header.seedLength    = seed;
    header.title.clear();
    return header;
}

inline ymh::Event event_for(const std::string& id, ymh::EventType type,
                            nlohmann::json payload) {
    ymh::Event event;
    event.id.value         = fresh_uuid();
    event.session_id.value = id;
    event.timestamp        = std::chrono::system_clock::now();
    event.type             = type;
    event.payload          = std::move(payload);
    return event;
}

inline nlohmann::json started_payload() {
    return nlohmann::json{
        {"model", "test-model"},
        {"server_profile", "interactive"},
        {"title", "test title"},
    };
}

inline nlohmann::json user_payload(std::string text) {
    return nlohmann::json{{"content", nlohmann::json::array()},
                          {"text", std::move(text)}};
}

} // namespace ymh_test
