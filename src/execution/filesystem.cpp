#include "ymh/execution/filesystem.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <regex>
#include <string>
#include <system_error>

#include "ymh/execution/environment.hpp"
#include "ymh/execution/errors.hpp"
#include "ymh/execution/glob.hpp"

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

namespace ymh {
namespace {

[[noreturn]] void throw_io(const std::filesystem::path& path, std::string_view what) {
    throw ToolError{ToolErrorCode::Io,
                    std::string{what} + ": " + path.string() + ": " +
                        std::strerror(errno)};
}

} // namespace

LocalFilesystem::LocalFilesystem(std::filesystem::path root, ToolConfig config)
    : root_(std::move(root)), config_(config) {}

void LocalFilesystem::require_contained(const std::filesystem::path& path) const {
    if (!path_is_within(path, root_)) {
        throw ToolError{ToolErrorCode::PathEscape,
                        "path escapes workspace root: " + path.string()};
    }
}

Task<FileData> LocalFilesystem::read(const std::filesystem::path& path) {
    require_contained(path);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) {
            throw ToolError{ToolErrorCode::NotFound, "no such file: " + path.string()};
        }
        if (errno == ELOOP) {
            throw ToolError{ToolErrorCode::PathEscape, "symlink target: " + path.string()};
        }
        throw_io(path, "read");
    }

    FileData data;
    struct stat info {};
    if (::fstat(fd, &info) == 0 && info.st_size > 0) {
        data.total_bytes = static_cast<std::size_t>(info.st_size);
    }

    const std::size_t limit = config_.read_file_max_bytes;
    std::string bytes;
    bytes.resize(limit);
    std::size_t used = 0;
    while (used < limit) {
        const std::size_t want = std::min<std::size_t>(limit - used, 65536);
        const ssize_t got = ::read(fd, bytes.data() + used, want);
        if (got < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            throw_io(path, "read");
        }
        if (got == 0) {
            break;
        }
        used += static_cast<std::size_t>(got);
    }
    ::close(fd);

    bytes.resize(used);
    data.bytes = std::move(bytes);
    if (data.total_bytes > 0 && used < data.total_bytes) {
        data.truncated = true;
    }
    return Task<FileData>(std::move(data));
}

Task<void> LocalFilesystem::write(const std::filesystem::path& path, const Data& data) {
    require_contained(path);
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
    }
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC |
                                         O_NOFOLLOW,
                          0644);
    if (fd < 0) {
        if (errno == ELOOP) {
            throw ToolError{ToolErrorCode::PathEscape, "symlink target: " + path.string()};
        }
        throw_io(path, "write");
    }
    std::size_t written = 0;
    while (written < data.bytes.size()) {
        const ssize_t n = ::write(fd, data.bytes.data() + written,
                                  data.bytes.size() - written);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            ::close(fd);
            throw_io(path, "write");
        }
        written += static_cast<std::size_t>(n);
    }
    if (::close(fd) != 0) {
        throw_io(path, "close");
    }
    return Task<void>{};
}

Task<void> LocalFilesystem::remove(const std::filesystem::path& path) {
    require_contained(path);
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    if (ec) {
        throw ToolError{ToolErrorCode::Io,
                        "remove: " + path.string() + ": " + ec.message()};
    }
    if (!removed) {
        throw ToolError{ToolErrorCode::NotFound, "no such file: " + path.string()};
    }
    return Task<void>{};
}

Task<FileInfo> LocalFilesystem::stat(const std::filesystem::path& path) {
    require_contained(path);
    std::error_code ec;
    const std::filesystem::file_status link_status =
        std::filesystem::symlink_status(path, ec);
    FileInfo info;
    if (ec || !std::filesystem::exists(link_status)) {
        return Task<FileInfo>(info);
    }
    info.exists = true;
    info.is_symlink = std::filesystem::is_symlink(link_status);
    const std::filesystem::file_status status = std::filesystem::status(path, ec);
    if (!ec) {
        info.is_directory = std::filesystem::is_directory(status);
        info.is_regular = std::filesystem::is_regular_file(status);
    }
    if (info.is_regular) {
        info.size = std::filesystem::file_size(path, ec);
        if (ec) {
            info.size = 0;
        }
    }
    info.modified = std::filesystem::last_write_time(path, ec);
    return Task<FileInfo>(info);
}

Task<std::vector<std::filesystem::path>> LocalFilesystem::glob(const GlobQuery& query) {
    require_contained(query.base);
    std::vector<std::filesystem::path> results;
    std::error_code ec;

    const auto consider = [&](const std::filesystem::path& candidate) {
        if (results.size() >= query.max_results) {
            return;
        }
        if (!std::filesystem::is_regular_file(candidate, ec)) {
            return;
        }
        std::error_code rel_ec;
        const std::filesystem::path rel =
            std::filesystem::relative(candidate, root_, rel_ec);
        const std::string text = rel_ec ? candidate.filename().string()
                                        : rel.generic_string();
        if (path_glob_match(query.pattern, text)) {
            results.push_back(candidate);
        }
    };

    if (std::filesystem::is_regular_file(query.base, ec)) {
        consider(query.base);
    } else {
        std::filesystem::recursive_directory_iterator it(
            query.base, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; it != end && results.size() < query.max_results; it.increment(ec)) {
            if (ec) {
                break;
            }
            consider(it->path());
        }
    }

    std::sort(results.begin(), results.end());
    if (results.size() > query.max_results) {
        results.resize(query.max_results);
    }
    return Task<std::vector<std::filesystem::path>>(std::move(results));
}

Task<std::vector<GrepMatch>> LocalFilesystem::grep(const GrepQuery& query) {
    require_contained(query.base);
    std::regex expression;
    try {
        expression = std::regex(query.pattern, std::regex::ECMAScript);
    } catch (const std::regex_error& error) {
        throw ToolError{ToolErrorCode::InvalidArguments,
                        std::string{"invalid regex: "} + error.what()};
    }

    std::vector<std::filesystem::path> files;
    std::error_code ec;
    if (std::filesystem::is_regular_file(query.base, ec)) {
        files.push_back(query.base);
    } else {
        std::filesystem::recursive_directory_iterator it(
            query.base, std::filesystem::directory_options::skip_permission_denied, ec);
        const std::filesystem::recursive_directory_iterator end;
        for (; it != end; it.increment(ec)) {
            if (ec) {
                break;
            }
            if (it->is_regular_file(ec)) {
                files.push_back(it->path());
            }
        }
    }
    std::sort(files.begin(), files.end());

    std::vector<GrepMatch> matches;
    for (const auto& file : files) {
        if (matches.size() >= query.max_results) {
            break;
        }
        if (!query.glob.empty() && !glob_match(query.glob, file.filename().string())) {
            continue;
        }
        std::ifstream in(file, std::ios::binary);
        if (!in) {
            continue;
        }
        std::string line;
        std::size_t line_number = 0;
        while (std::getline(in, line)) {
            ++line_number;
            if (line_number > 1'000'000) {
                break;
            }
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (std::regex_search(line, expression)) {
                matches.push_back(GrepMatch{file, line_number, line});
                if (matches.size() >= query.max_results) {
                    break;
                }
            }
        }
    }
    return Task<std::vector<GrepMatch>>(std::move(matches));
}

} // namespace ymh
