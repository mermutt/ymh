#include "ymh/transport/json_rpc.hpp"

#include <cctype>
#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "ymh/transport/frame_codec.hpp"

namespace ymh::protocol {
namespace {

constexpr std::string_view kJsonRpcVersion = "2.0";

[[nodiscard]] bool is_ws(char ch) noexcept {
    return ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r';
}

// Returns the index just past the first complete JSON value, or nullopt when
// the text is not a complete value. Used to distinguish trailing garbage
// (InvalidRequest + close) from malformed JSON (recoverable ParseError).
[[nodiscard]] std::optional<std::size_t> find_value_end(std::string_view text) noexcept {
    std::size_t pos = 0;
    while (pos < text.size() && is_ws(text[pos])) {
        ++pos;
    }
    if (pos >= text.size()) {
        return std::nullopt;
    }
    const char first = text[pos];
    if (first == '{' || first == '[') {
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        for (std::size_t index = pos; index < text.size(); ++index) {
            const char ch = text[index];
            if (in_string) {
                if (escaped) {
                    escaped = false;
                } else if (ch == '\\') {
                    escaped = true;
                } else if (ch == '"') {
                    in_string = false;
                }
                continue;
            }
            if (ch == '"') {
                in_string = true;
            } else if (ch == '{' || ch == '[') {
                ++depth;
            } else if (ch == '}' || ch == ']') {
                --depth;
                if (depth == 0) {
                    return index + 1;
                }
            }
        }
        return std::nullopt;
    }
    if (first == '"') {
        bool escaped = false;
        for (std::size_t index = pos + 1; index < text.size(); ++index) {
            const char ch = text[index];
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                return index + 1;
            }
        }
        return std::nullopt;
    }
    std::size_t index = pos;
    while (index < text.size() && !is_ws(text[index])) {
        ++index;
    }
    return index;
}

[[nodiscard]] ProtocolError parse_error(std::string message) {
    return ProtocolError(code_value(RpcCode::ParseError), std::move(message), false);
}

[[nodiscard]] ProtocolError invalid_request(std::string message, bool fatal) {
    return ProtocolError(code_value(RpcCode::InvalidRequest), std::move(message), fatal);
}

} // namespace

nlohmann::json encode(const Request& request) {
    return nlohmann::json{{"jsonrpc", std::string{kJsonRpcVersion}},
                          {"id", request.id},
                          {"method", request.method},
                          {"params", request.params}};
}

nlohmann::json encode(const Response& response) {
    return nlohmann::json{{"jsonrpc", std::string{kJsonRpcVersion}},
                          {"id", response.id},
                          {"result", response.result}};
}

nlohmann::json encode(const ErrorResponse& error) {
    nlohmann::json object = nlohmann::json{{"code", error.code}, {"message", error.message}};
    if (!error.data.is_null()) {
        object["data"] = error.data;
    }
    return nlohmann::json{{"jsonrpc", std::string{kJsonRpcVersion}},
                          {"id", error.id},
                          {"error", std::move(object)}};
}

nlohmann::json encode(const Notification& notification) {
    return nlohmann::json{{"jsonrpc", std::string{kJsonRpcVersion}},
                          {"method", notification.method},
                          {"params", notification.params}};
}

nlohmann::json encode(const Message& message) {
    return std::visit([](const auto& value) { return encode(value); }, message);
}

Message parse_message(std::string_view body) {
    const auto end = find_value_end(body);
    if (!end.has_value()) {
        throw parse_error("body is not a complete JSON value");
    }
    for (std::size_t index = *end; index < body.size(); ++index) {
        if (!is_ws(body[index])) {
            throw invalid_request("trailing bytes after JSON value", true);
        }
    }

    nlohmann::json json;
    try {
        json = nlohmann::json::parse(body.substr(0, *end));
    } catch (const nlohmann::json::exception& error) {
        throw parse_error(error.what());
    }

    if (json.is_array()) {
        throw invalid_request("batch requests are not supported", false);
    }
    if (!json.is_object()) {
        throw invalid_request("message must be a JSON object", false);
    }
    const auto version = json.find("jsonrpc");
    if (version == json.end() || !version->is_string() || version->get<std::string>() != kJsonRpcVersion) {
        throw invalid_request("jsonrpc must be \"2.0\"", false);
    }

    const auto method = json.find("method");
    if (method != json.end()) {
        if (!method->is_string()) {
            throw invalid_request("method must be a string", false);
        }
        Request request;
        request.method = method->get<std::string>();
        const auto params = json.find("params");
        if (params != json.end() && !params->is_null()) {
            if (!params->is_object() && !params->is_array()) {
                throw invalid_request("params must be an object or array", false);
            }
            request.params = *params;
        }
        const auto id = json.find("id");
        if (id != json.end() && !id->is_null()) {
            from_json(*id, request.id);
            return request;
        }
        Notification notification;
        notification.method = request.method;
        notification.params = std::move(request.params);
        return notification;
    }

    const auto error = json.find("error");
    if (error != json.end()) {
        if (!error->is_object()) {
            throw invalid_request("error must be an object", false);
        }
        ErrorResponse response;
        const auto id = json.find("id");
        if (id != json.end()) {
            from_json(*id, response.id);
        }
        response.code = error->value("code", 0);
        response.message = error->value("message", std::string{});
        const auto data = error->find("data");
        if (data != error->end()) {
            response.data = *data;
        }
        return response;
    }

    const auto result = json.find("result");
    if (result != json.end()) {
        Response response;
        const auto id = json.find("id");
        if (id != json.end()) {
            from_json(*id, response.id);
        }
        response.result = *result;
        return response;
    }

    throw invalid_request("message is neither a request, notification, nor response", false);
}

} // namespace ymh::protocol
