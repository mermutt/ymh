#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "test_support.hpp"

#include "ymh/agent/message.hpp"
#include "ymh/agent/prompt_images.hpp"

namespace {

using namespace ymh;
using ymh_test::TempWorkspace;

constexpr char kPngMagic[] = "\x89PNG\r\n\x1a\n";

std::string png_bytes(std::size_t payload = 16) {
    std::string bytes{kPngMagic, 8};
    bytes.append(payload, 'x');
    return bytes;
}

void write_bytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream{path, std::ios::binary};
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

bool send_all(int fd, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count =
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

// A one-request-per-connection HTTP/1.1 stub on an ephemeral loopback port.
// No external network is involved.
class HttpStub {
public:
    HttpStub() {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd_ < 0) {
            return;
        }
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        // Bound the blocking accept so the worker notices `stop_`.
        timeval timeout{};
        timeout.tv_sec  = 0;
        timeout.tv_usec = 200000;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        sockaddr_in address{};
        address.sin_family      = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port        = 0;
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(listen_fd_, 16) != 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
            return;
        }
        socklen_t length = sizeof(address);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this] { serve(); });
    }

    ~HttpStub() {
        stop_.store(true);
        if (worker_.joinable()) {
            worker_.join();
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
        }
    }

    HttpStub(const HttpStub&)            = delete;
    HttpStub& operator=(const HttpStub&) = delete;

    [[nodiscard]] bool ok() const { return listen_fd_ >= 0 && port_ != 0; }

    [[nodiscard]] std::string url(std::string_view path) const {
        return "http://127.0.0.1:" + std::to_string(port_) + std::string{path};
    }

private:
    void serve() {
        while (!stop_.load()) {
            const int fd = ::accept(listen_fd_, nullptr, nullptr);
            if (fd < 0) {
                continue;
            }
            handle(fd);
            ::close(fd);
        }
    }

    static void respond(int fd, std::string_view status, std::string_view body,
                        std::string_view content_type) {
        std::string response = "HTTP/1.1 ";
        response += status;
        response += "\r\nContent-Type: ";
        response += content_type;
        response += "\r\nContent-Length: ";
        response += std::to_string(body.size());
        response += "\r\nConnection: close\r\n\r\n";
        response += body;
        send_all(fd, response);
    }

    static void handle(int fd) {
        std::string request;
        char        buffer[1024];
        while (request.find("\r\n\r\n") == std::string::npos && request.size() < 8192) {
            const ssize_t count = ::recv(fd, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                return;
            }
            request.append(buffer, static_cast<std::size_t>(count));
        }
        const std::size_t line_end      = request.find("\r\n");
        const std::string request_line  = request.substr(0, line_end);
        const std::size_t first_space   = request_line.find(' ');
        const std::size_t second_space  = request_line.find(' ', first_space + 1);
        const std::string path =
            first_space == std::string::npos || second_space == std::string::npos
                ? std::string{}
                : request_line.substr(first_space + 1, second_space - first_space - 1);

        if (path == "/png") {
            respond(fd, "200 OK", png_bytes(), "image/png");
        } else if (path == "/redirect") {
            send_all(fd,
                     "HTTP/1.1 302 Found\r\n"
                     "Location: /png\r\n"
                     "Content-Length: 0\r\n"
                     "Connection: close\r\n\r\n");
        } else if (path == "/oversized") {
            respond(fd, "200 OK", png_bytes(4096), "image/png");
        } else {
            respond(fd, "404 Not Found", "not found", "text/plain");
        }
    }

    int               listen_fd_ = -1;
    int               port_      = 0;
    std::atomic<bool> stop_{false};
    std::thread       worker_;
};

Message text_message(const std::string& text) {
    Message message;
    message.role = Role::User;
    ContentBlock block;
    block.kind = ContentBlockKind::Text;
    block.text = text;
    message.content.push_back(std::move(block));
    return message;
}

std::size_t image_count(const Message& message) {
    std::size_t count = 0;
    for (const ContentBlock& block : message.content) {
        if (block.kind == ContentBlockKind::Image) {
            ++count;
        }
    }
    return count;
}

bool has_text(const Message& message, std::string_view needle) {
    for (const ContentBlock& block : message.content) {
        if (block.kind == ContentBlockKind::Text &&
            block.text.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

class PromptUrlTest : public ::testing::Test {
protected:
    static void SetUpTestSuite() {
        // Keep the stub hermetic even when the developer's shell exports a
        // proxy for every host.
        ::setenv("no_proxy", "127.0.0.1,localhost", 1);
        ::setenv("NO_PROXY", "127.0.0.1,localhost", 1);
    }
};

TEST_F(PromptUrlTest, IsRemoteImageSourceRecognizesHttpSchemesOnly) {
    EXPECT_TRUE(is_remote_image_source("http://example.com/a.png"));
    EXPECT_TRUE(is_remote_image_source("https://example.com/a.png"));
    EXPECT_TRUE(is_remote_image_source("HTTP://example.com/a.png"));
    EXPECT_TRUE(is_remote_image_source("HtTpS://example.com/a.png"));
    EXPECT_FALSE(is_remote_image_source("file:///tmp/a.png"));
    EXPECT_FALSE(is_remote_image_source("/tmp/a.png"));
    EXPECT_FALSE(is_remote_image_source("ftp://example.com/a.png"));
    EXPECT_FALSE(is_remote_image_source("https:/example.com/a.png"));
}

TEST_F(PromptUrlTest, LoadsLocalPathsAndFileUris) {
    TempWorkspace     workspace;
    PromptImageLimits limits;
    const std::string bytes = png_bytes();
    write_bytes(workspace.root / "shot.png", bytes);

    std::string reason;
    const std::optional<LoadedImageSource> relative =
        load_image_source("shot.png", workspace.root, limits, reason);
    ASSERT_TRUE(relative.has_value()) << reason;
    EXPECT_EQ(relative->image.media_type, "image/png");
    EXPECT_EQ(relative->image.bytes, bytes.size());
    EXPECT_FALSE(relative->image.data.empty());
    EXPECT_EQ(relative->display, (workspace.root / "shot.png").string());

    const std::string uri = "file://" + (workspace.root / "shot.png").string();
    const std::optional<LoadedImageSource> absolute =
        load_image_source(uri, workspace.root, limits, reason);
    ASSERT_TRUE(absolute.has_value()) << reason;
    EXPECT_EQ(absolute->image.media_type, "image/png");
    EXPECT_EQ(absolute->image.bytes, bytes.size());
}

TEST_F(PromptUrlTest, RejectsUnsupportedUrlSchemes) {
    TempWorkspace     workspace;
    PromptImageLimits limits;
    std::string       reason;

    const std::optional<LoadedImageSource> loaded =
        load_image_source("ftp://example.com/a.png", workspace.root, limits, reason);
    EXPECT_FALSE(loaded.has_value());
    EXPECT_EQ(reason, "unsupported URL scheme (use http or https)");
}

TEST_F(PromptUrlTest, DownloadsPngAndFollowsOneRedirect) {
    TempWorkspace     workspace;
    HttpStub          stub;
    ASSERT_TRUE(stub.ok());
    PromptImageLimits limits;
    const std::string bytes = png_bytes();

    std::string reason;
    const std::optional<LoadedImageSource> direct =
        load_image_source(stub.url("/png"), workspace.root, limits, reason);
    ASSERT_TRUE(direct.has_value()) << reason;
    EXPECT_EQ(direct->display, stub.url("/png"));
    EXPECT_EQ(direct->image.media_type, "image/png");
    EXPECT_EQ(direct->image.bytes, bytes.size());

    const std::optional<LoadedImageSource> redirected =
        load_image_source(stub.url("/redirect"), workspace.root, limits, reason);
    ASSERT_TRUE(redirected.has_value()) << reason;
    EXPECT_EQ(redirected->display, stub.url("/redirect"));
    EXPECT_EQ(redirected->image.media_type, "image/png");
    EXPECT_EQ(redirected->image.bytes, bytes.size());
}

TEST_F(PromptUrlTest, ReportsHttpStatusForAMissingResource) {
    TempWorkspace     workspace;
    HttpStub          stub;
    ASSERT_TRUE(stub.ok());
    PromptImageLimits limits;
    std::string       reason;

    const std::optional<LoadedImageSource> loaded =
        load_image_source(stub.url("/missing"), workspace.root, limits, reason);
    EXPECT_FALSE(loaded.has_value());
    EXPECT_EQ(reason, "HTTP 404");
}

TEST_F(PromptUrlTest, AbortsAnOversizedDownload) {
    TempWorkspace     workspace;
    HttpStub          stub;
    ASSERT_TRUE(stub.ok());
    PromptImageLimits limits;
    limits.max_image_bytes = 128;
    std::string reason;

    const std::optional<LoadedImageSource> loaded =
        load_image_source(stub.url("/oversized"), workspace.root, limits, reason);
    EXPECT_FALSE(loaded.has_value());
    EXPECT_NE(reason.find("limit"), std::string::npos) << reason;
}

TEST_F(PromptUrlTest, AttachesLocalAndRemoteSourcesTogether) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "shot.png", png_bytes());
    HttpStub stub;
    ASSERT_TRUE(stub.ok());

    Message message = text_message("describe these");
    const std::vector<std::string> sources{"shot.png", stub.url("/png")};
    const PromptImageResult        result =
        attach_image_sources(message, sources, workspace.root);

    EXPECT_TRUE(result.warnings.empty());
    EXPECT_EQ(result.attached, 2U);
    EXPECT_EQ(image_count(message), 2U);
}

TEST_F(PromptUrlTest, ABadUrlLeavesTheOtherSourcesAttached) {
    TempWorkspace workspace;
    write_bytes(workspace.root / "shot.png", png_bytes());
    HttpStub stub;
    ASSERT_TRUE(stub.ok());

    Message message = text_message("describe these");
    const std::vector<std::string> sources{"shot.png", stub.url("/missing")};
    const PromptImageResult        result =
        attach_image_sources(message, sources, workspace.root);

    EXPECT_EQ(result.attached, 1U);
    EXPECT_EQ(image_count(message), 1U);
    ASSERT_EQ(result.warnings.size(), 1U);
    EXPECT_NE(result.warnings[0].find("image not attached"), std::string::npos);
    EXPECT_NE(result.warnings[0].find("HTTP 404"), std::string::npos);
    EXPECT_TRUE(has_text(message, "image not attached"));
    EXPECT_TRUE(has_text(message, "HTTP 404"));
}

} // namespace
