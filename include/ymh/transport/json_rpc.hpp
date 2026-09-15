#pragma once

// JSON-RPC 2.0 envelopes and codec (docs/design/05-transport.md §5.4).
//
// A frame body is exactly one JSON-RPC message. The daemon receives requests
// and notifications; the client receives responses and notifications. Batch
// requests are unsupported (T-F15): a top-level array is InvalidRequest.
// Framing-level errors carry `"id": null` (RequestId's monostate, T-F4).

#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

#include "ymh/transport/protocol.hpp"

namespace ymh::protocol {

struct Request {
    RequestId      id;
    std::string    method;
    nlohmann::json params{nlohmann::json::object()};
};

struct Response {
    RequestId      id;
    nlohmann::json result;
};

struct ErrorResponse {
    RequestId      id;
    int            code{0};
    std::string    message;
    nlohmann::json data;
};

struct Notification {
    std::string    method;
    nlohmann::json params{nlohmann::json::object()};
};

using Message = std::variant<Request, Response, ErrorResponse, Notification>;

[[nodiscard]] nlohmann::json encode(const Request& request);
[[nodiscard]] nlohmann::json encode(const Response& response);
[[nodiscard]] nlohmann::json encode(const ErrorResponse& error);
[[nodiscard]] nlohmann::json encode(const Notification& notification);
[[nodiscard]] nlohmann::json encode(const Message& message);

// Parses one frame body. Throws ProtocolError on failure:
//   invalid JSON        -> ParseError, non-fatal
//   non-object / batch  -> InvalidRequest, non-fatal
//   trailing bytes      -> InvalidRequest, fatal
[[nodiscard]] Message parse_message(std::string_view body);

} // namespace ymh::protocol
