#pragma once

// Minimal blocking HTTP/1.1 test server on a loopback socket. It serves a
// scripted list of responses, one per accepted connection, so the libcurl
// transport can be exercised end to end without touching the network.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace ymh::test {

class MockHttpServer {
public:
    struct Response {
        int status = 200;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
        // When set, only this many body bytes are written (Content-Length still
        // advertises the full size) to simulate a mid-stream disconnect.
        std::size_t truncate_body_at = std::numeric_limits<std::size_t>::max();
    };

    explicit MockHttpServer(std::vector<Response> responses)
        : responses_(std::move(responses)) {
        listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        int reuse = 1;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        ::bind(listen_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::listen(listen_fd_, 16);

        socklen_t length = sizeof(address);
        ::getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&address), &length);
        port_ = ntohs(address.sin_port);

        timeval timeout{};
        timeout.tv_usec = 100'000;
        ::setsockopt(listen_fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        thread_ = std::thread([this] { run(); });
    }

    ~MockHttpServer() {
        stop_.store(true);
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
        }
    }

    MockHttpServer(const MockHttpServer&) = delete;
    MockHttpServer& operator=(const MockHttpServer&) = delete;

    [[nodiscard]] int port() const noexcept { return port_; }

    [[nodiscard]] std::string base_url() const {
        return "http://127.0.0.1:" + std::to_string(port_) + "/v1";
    }

    [[nodiscard]] std::size_t requests() const noexcept { return request_count_.load(); }

private:
    void run() {
        while (!stop_.load()) {
            const int client = ::accept(listen_fd_, nullptr, nullptr);
            if (client < 0) {
                continue;
            }
            handle(client);
            ::close(client);
        }
    }

    void handle(int client) {
        std::string request;
        char buffer[4096];
        std::size_t header_end = std::string::npos;
        while (header_end == std::string::npos) {
            const ssize_t count = ::recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                return;
            }
            request.append(buffer, static_cast<std::size_t>(count));
            header_end = request.find("\r\n\r\n");
        }

        const std::string headers = request.substr(0, header_end);
        if (lower(headers).find("expect: 100-continue") != std::string::npos) {
            const std::string cont = "HTTP/1.1 100 Continue\r\n\r\n";
            send_all(client, cont);
        }

        const std::size_t content_length = parse_content_length(headers);
        std::size_t have = request.size() - (header_end + 4);
        while (have < content_length) {
            const ssize_t count = ::recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) {
                break;
            }
            request.append(buffer, static_cast<std::size_t>(count));
            have += static_cast<std::size_t>(count);
        }

        const std::size_t index = request_count_.fetch_add(1);
        if (index >= responses_.size()) {
            send_all(client, "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 2\r\n"
                             "Connection: close\r\n\r\n{}");
            return;
        }

        const Response& response = responses_[index];
        std::string out = "HTTP/1.1 " + std::to_string(response.status) + " " +
                          reason(response.status) + "\r\n";
        for (const auto& [name, value] : response.headers) {
            out += name + ": " + value + "\r\n";
        }
        out += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        out += "Connection: close\r\n\r\n";
        send_all(client, out);

        const std::size_t send_bytes =
            response.truncate_body_at == std::numeric_limits<std::size_t>::max()
                ? response.body.size()
                : response.truncate_body_at;
        send_all(client, response.body.substr(0, send_bytes));
    }

    static std::string lower(std::string value) {
        for (char& c : value) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return value;
    }

    static std::size_t parse_content_length(const std::string& headers) {
        const std::string lowered = lower(headers);
        const std::string key = "content-length:";
        const std::size_t position = lowered.find(key);
        if (position == std::string::npos) {
            return 0;
        }
        std::size_t begin = position + key.size();
        while (begin < lowered.size() && std::isspace(static_cast<unsigned char>(lowered[begin]))) {
            ++begin;
        }
        std::size_t end = begin;
        while (end < lowered.size() && std::isdigit(static_cast<unsigned char>(lowered[end]))) {
            ++end;
        }
        return static_cast<std::size_t>(std::stoull(lowered.substr(begin, end - begin)));
    }

    static void send_all(int client, const std::string& data) {
        std::size_t sent = 0;
        while (sent < data.size()) {
            const ssize_t count =
                ::send(client, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
            if (count <= 0) {
                return;
            }
            sent += static_cast<std::size_t>(count);
        }
    }

    static const char* reason(int status) {
        switch (status) {
            case 200: return "OK";
            case 400: return "Bad Request";
            case 401: return "Unauthorized";
            case 403: return "Forbidden";
            case 404: return "Not Found";
            case 422: return "Unprocessable Entity";
            case 429: return "Too Many Requests";
            case 500: return "Internal Server Error";
            case 503: return "Service Unavailable";
            default: return "Status";
        }
    }

    std::vector<Response>  responses_;
    int                    listen_fd_ = -1;
    int                    port_ = 0;
    std::atomic<bool>      stop_{false};
    std::atomic<std::size_t> request_count_{0};
    std::thread            thread_;
};

} // namespace ymh::test
