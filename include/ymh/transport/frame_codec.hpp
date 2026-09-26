#pragma once

// Length-prefixed framing pinned by docs/design/05-transport.md §3.3 (T1/T2).
//
// Frame = 4-byte unsigned big-endian length + exactly that many UTF-8 JSON
// bytes. A bad length prefix (zero or over the cap) or a non-UTF-8 body means
// the byte stream can no longer be trusted, so `ProtocolError::fatal` is set
// and the caller closes the connection (T18). A body that is valid UTF-8 but
// invalid JSON is recoverable and is handled one layer up.

#include <cstddef>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace ymh::protocol {

struct ProtocolError final : std::runtime_error {
    ProtocolError(int rpc_code, std::string message, bool fatal_connection)
        : std::runtime_error(std::move(message)),
          code(rpc_code),
          fatal(fatal_connection) {}

    int  code;              // RpcCode | AppCode
    bool fatal;             // untrustworthy framing: close the connection
};

enum class DecodeStatus {
    Ok,        // one complete frame was produced in `out`
    NeedMore,  // the buffer holds a partial frame
};

class FrameCodec {
public:
    static constexpr std::size_t kHeaderBytes = 4;

    // Prepends the 4-byte big-endian length. Throws ProtocolError{PayloadTooLarge}
    // when the encoded body exceeds `max`.
    static std::string encode(std::string_view json_body, std::size_t max);

    // Consumes zero or more complete frames from `buf`, leaving any partial tail
    // in place. Throws ProtocolError{FrameTooLarge|ParseError|InvalidRequest}
    // with `fatal == true` on an untrustworthy length prefix or non-UTF-8 body.
    static std::vector<std::string> decode(std::string& buf, std::size_t max);

    // One-frame step used by the server so frames preceding a fatal error are
    // still handled before the connection closes.
    static DecodeStatus decode_step(std::string& buf, std::size_t max, std::string& out);
};

} // namespace ymh::protocol
