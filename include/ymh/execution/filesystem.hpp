#pragma once

// Rooted filesystem service (07 §6.3). Every method receives an already
// `resolve()`d absolute path; `LocalFilesystem` re-verifies containment on
// writes as defense-in-depth against TOCTOU (§15). Reads are bounded by
// `ToolConfig::read_file_max_bytes`; glob/grep results by
// `ToolConfig::search_max_results`.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "ymh/core/task.hpp"
#include "ymh/execution/config.hpp"

namespace ymh {

struct Data {
    std::string bytes;
};

struct FileData {
    std::string bytes;
    bool        truncated = false;
    std::size_t total_bytes = 0;
};

struct FileInfo {
    bool                     exists = false;
    bool                     is_directory = false;
    bool                     is_regular = false;
    bool                     is_symlink = false;
    std::uintmax_t           size = 0;
    std::filesystem::file_time_type modified{};
};

struct GlobQuery {
    std::string           pattern;             // e.g. "src/**/*.cpp"
    std::filesystem::path base;                // resolved absolute directory
    std::size_t           max_results = 1000;
};

struct GrepQuery {
    std::string           pattern;             // ECMAScript regex
    std::filesystem::path base;                // resolved absolute directory or file
    std::string           glob;                // optional filename glob filter
    bool                  line_numbers = true;
    std::size_t           max_results = 1000;
};

struct GrepMatch {
    std::filesystem::path path;
    std::size_t           line = 0;            // 1-based
    std::string           text;
};

class Filesystem {
public:
    virtual ~Filesystem() = default;

    virtual Task<FileData> read(const std::filesystem::path&) = 0;
    virtual Task<void>     write(const std::filesystem::path&, const Data&) = 0;
    virtual Task<void>     remove(const std::filesystem::path&) = 0;
    virtual Task<FileInfo> stat(const std::filesystem::path&) = 0;
    virtual Task<std::vector<std::filesystem::path>> glob(const GlobQuery&) = 0;
    virtual Task<std::vector<GrepMatch>>             grep(const GrepQuery&) = 0;
};

class LocalFilesystem final : public Filesystem {
public:
    LocalFilesystem(std::filesystem::path root, ToolConfig config);

    Task<FileData> read(const std::filesystem::path& path) override;
    Task<void>     write(const std::filesystem::path& path, const Data& data) override;
    Task<void>     remove(const std::filesystem::path& path) override;
    Task<FileInfo> stat(const std::filesystem::path& path) override;
    Task<std::vector<std::filesystem::path>> glob(const GlobQuery& query) override;
    Task<std::vector<GrepMatch>>             grep(const GrepQuery& query) override;

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

private:
    void require_contained(const std::filesystem::path& path) const;

    std::filesystem::path root_;
    ToolConfig            config_;
};

} // namespace ymh
