#include "ymh/transport/frame_codec.hpp"

#include <cstdint>
#include <string>

#include "ymh/transport/protocol.hpp"

namespace ymh::protocol {
namespace {

[[nodiscard]] bool is_valid_utf8(std::string_view bytes) noexcept {
    std::size_t index = 0;
    while (index < bytes.size()) {
        const auto byte = static_cast<unsigned char>(bytes[index]);
        std::size_t extra = 0;
        if (byte < 0x80) {
            ++index;
            continue;
        }
        if ((byte & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((byte & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((byte & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            return false;
        }
        if (index + extra >= bytes.size()) {
            return false;
        }
        for (std::size_t offset = 1; offset <= extra; ++offset) {
            const auto continuation = static_cast<unsigned char>(bytes[index + offset]);
            if ((continuation & 0xC0) != 0x80) {
                return false;
            }
        }
        index += extra + 1;
    }
    return true;
}

[[nodiscard]] std::uint32_t read_length(std::string_view header) noexcept {
    return (static_cast<std::uint32_t>(static_cast<unsigned char>(header[0])) << 24u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(header[1])) << 16u) |
           (static_cast<std::uint32_t>(static_cast<unsigned char>(header[2])) << 8u) |
           static_cast<std::uint32_t>(static_cast<unsigned char>(header[3]));
}

} // namespace

std::string FrameCodec::encode(std::string_view json_body, std::size_t max) {
    if (json_body.size() > max) {
        throw ProtocolError(code_value(AppCode::PayloadTooLarge),
                            "frame body exceeds max_frame_bytes", true);
    }
    const auto length = static_cast<std::uint32_t>(json_body.size());
    std::string frame;
    frame.reserve(kHeaderBytes + json_body.size());
    frame.push_back(static_cast<char>((length >> 24u) & 0xFFu));
    frame.push_back(static_cast<char>((length >> 16u) & 0xFFu));
    frame.push_back(static_cast<char>((length >> 8u) & 0xFFu));
    frame.push_back(static_cast<char>(length & 0xFFu));
    frame.append(json_body.data(), json_body.size());
    return frame;
}

DecodeStatus FrameCodec::decode_step(std::string& buf, std::size_t max, std::string& out) {
    if (buf.size() < kHeaderBytes) {
        return DecodeStatus::NeedMore;
    }
    const std::uint32_t length = read_length(std::string_view{buf}.substr(0, kHeaderBytes));
    if (length == 0) {
        throw ProtocolError(code_value(RpcCode::InvalidRequest), "zero-length frame", true);
    }
    if (length > max) {
        throw ProtocolError(code_value(AppCode::FrameTooLarge), "frame exceeds max_frame_bytes",
                            true);
    }
    if (buf.size() < kHeaderBytes + length) {
        return DecodeStatus::NeedMore;
    }
    out.assign(buf, kHeaderBytes, length);
    buf.erase(0, kHeaderBytes + length);
    if (!is_valid_utf8(out)) {
        throw ProtocolError(code_value(RpcCode::ParseError), "frame body is not valid UTF-8",
                            true);
    }
    return DecodeStatus::Ok;
}

std::vector<std::string> FrameCodec::decode(std::string& buf, std::size_t max) {
    std::vector<std::string> frames;
    std::string frame;
    while (decode_step(buf, max, frame) == DecodeStatus::Ok) {
        frames.push_back(std::move(frame));
        frame.clear();
    }
    return frames;
}

} // namespace ymh::protocol
