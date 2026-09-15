#pragma once

// Short-path temp roots for Unix-domain-socket tests. `sun_path` is 108 bytes
// including the NUL terminator, and a test socket lives under
// `<root>/.ymh/host.sock`; a long `TMPDIR` (or a long test name) would push the
// bound path past the limit. This helper deliberately prefers `/tmp` (or
// `YMH_TEST_TMPDIR`) and mints a short `mkdtemp` directory.

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

namespace ymh::test {

inline constexpr std::size_t kUnixSocketPathMax = 108;

inline std::filesystem::path short_temp_base() {
    if (const char* override_dir = std::getenv("YMH_TEST_TMPDIR");
        override_dir != nullptr && *override_dir != '\0') {
        return override_dir;
    }
    std::error_code error;
    if (std::filesystem::is_directory("/tmp", error) && ::access("/tmp", W_OK) == 0) {
        return "/tmp";
    }
    return std::filesystem::temp_directory_path();
}

inline std::filesystem::path make_short_temp_root(const std::string& prefix) {
    const std::filesystem::path base = short_temp_base();
    std::string                 pattern = (base / (prefix + "-XXXXXX")).string();
    std::vector<char>           buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) {
        throw std::runtime_error("make_short_temp_root: mkdtemp failed for " + pattern);
    }
    return std::filesystem::path{buffer.data()};
}

[[nodiscard]] inline bool socket_path_fits(const std::filesystem::path& path) {
    return path.string().size() < kUnixSocketPathMax;
}

class ShortTempRoot {
public:
    explicit ShortTempRoot(const std::string& prefix = "ymh")
        : path_(make_short_temp_root(prefix)) {}

    ~ShortTempRoot() {
        if (!kept_) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    ShortTempRoot(const ShortTempRoot&) = delete;
    ShortTempRoot& operator=(const ShortTempRoot&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

    [[nodiscard]] std::filesystem::path host_socket() const {
        return path_ / ".ymh" / "host.sock";
    }

    [[nodiscard]] std::filesystem::path host_log() const { return path_ / ".ymh" / "host.log"; }

    [[nodiscard]] std::filesystem::path state_dir() const { return path_ / ".state"; }

    [[nodiscard]] std::filesystem::path config_dir() const { return path_ / ".config"; }

    void keep() noexcept { kept_ = true; }

    void write(const std::string& relative, const std::string& content) const {
        const std::filesystem::path target = path_ / relative;
        std::filesystem::create_directories(target.parent_path());
        std::ofstream(target, std::ios::binary) << content;
    }

private:
    std::filesystem::path path_;
    bool                  kept_{false};
};

} // namespace ymh::test
