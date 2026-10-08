#include "ymh/core/fs.hpp"

#include <fcntl.h>
#include <unistd.h>

namespace ymh {

void fsync_parent_directory(const std::filesystem::path& file) {
    const std::filesystem::path directory =
        file.parent_path().empty() ? std::filesystem::path{"."} : file.parent_path();
    const int dir_fd = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_fd >= 0) {
        (void)::fsync(dir_fd);
        ::close(dir_fd);
    }
}

}
