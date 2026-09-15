#include "ymh/execution/output.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace ymh {
namespace {

constexpr std::size_t kReplacementLength = 3;
constexpr char kReplacement[kReplacementLength] = {static_cast<char>(0xEF),
                                                   static_cast<char>(0xBF),
                                                   static_cast<char>(0xBD)};

std::size_t utf8_sequence_length(unsigned char lead) noexcept {
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 0;
}

bool is_continuation(unsigned char byte) noexcept { return (byte & 0xC0) == 0x80; }

bool valid_utf8_sequence(std::string_view bytes, std::size_t at) noexcept {
    const auto lead = static_cast<unsigned char>(bytes[at]);
    const std::size_t length = utf8_sequence_length(lead);
    if (length == 0 || at + length > bytes.size()) {
        return false;
    }
    for (std::size_t i = 1; i < length; ++i) {
        if (!is_continuation(static_cast<unsigned char>(bytes[at + i]))) {
            return false;
        }
    }
    if (length == 2) {
        return lead >= 0xC2;
    }
    if (length == 3) {
        const auto second = static_cast<unsigned char>(bytes[at + 1]);
        if (lead == 0xE0) return second >= 0xA0;
        if (lead == 0xED) return second < 0xA0;
    }
    if (length == 4) {
        const auto second = static_cast<unsigned char>(bytes[at + 1]);
        if (lead == 0xF0) return second >= 0x90;
        if (lead == 0xF4) return second < 0x90;
        return lead <= 0xF3;
    }
    return true;
}

} // namespace

std::string sanitize_utf8(std::string_view bytes, bool& lost) {
    std::string out;
    out.reserve(bytes.size());
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) {
            out.push_back(bytes[i]);
            ++i;
            continue;
        }
        if (valid_utf8_sequence(bytes, i)) {
            out.append(bytes.substr(i, utf8_sequence_length(lead)));
            i += utf8_sequence_length(lead);
            continue;
        }
        out.append(kReplacement, kReplacementLength);
        lost = true;
        ++i;
    }
    return out;
}

std::string_view trim_to_utf8_start(std::string_view bytes) noexcept {
    std::size_t i = 0;
    while (i < bytes.size() &&
           is_continuation(static_cast<unsigned char>(bytes[i]))) {
        ++i;
    }
    return bytes.substr(i);
}

OutputRing::OutputRing(std::size_t capacity_bytes)
    : capacity_(capacity_bytes == 0 ? 1 : capacity_bytes) {}

void OutputRing::append(std::string_view chunk, bool /*is_stderr*/) {
    total_appended_ += chunk.size();
    if (chunk.size() >= capacity_) {
        if (!bytes_.empty() || chunk.size() > capacity_) {
            truncated_ = true;
        }
        bytes_.clear();
        const std::string_view keep = chunk.substr(chunk.size() - capacity_);
        bytes_.insert(bytes_.end(), keep.begin(), keep.end());
        return;
    }
    while (bytes_.size() + chunk.size() > capacity_) {
        bytes_.pop_front();
        truncated_ = true;
    }
    bytes_.insert(bytes_.end(), chunk.begin(), chunk.end());
}

std::size_t OutputRing::size() const noexcept { return bytes_.size(); }

std::size_t OutputRing::capacity() const noexcept { return capacity_; }

bool OutputRing::truncated() const noexcept { return truncated_; }

std::string OutputRing::tail(std::size_t max_bytes) const {
    const std::size_t take = std::min(max_bytes, bytes_.size());
    std::string out;
    out.reserve(take);
    for (std::size_t i = bytes_.size() - take; i < bytes_.size(); ++i) {
        out.push_back(bytes_[i]);
    }
    const std::string_view trimmed = trim_to_utf8_start(out);
    return std::string(trimmed);
}

void OutputRing::clear() noexcept {
    bytes_.clear();
    total_appended_ = 0;
    truncated_ = false;
}

RingOutputSink::RingOutputSink(OutputRing& ring) : ring_(&ring) {}

void RingOutputSink::write(std::string_view chunk) {
    bool lost = false;
    std::string clean = sanitize_utf8(chunk, lost);
    utf8_loss_ = utf8_loss_ || lost;
    bytes_written_ += chunk.size();
    ring_->append(clean, false);
}

void RingOutputSink::writeErr(std::string_view chunk) {
    bool lost = false;
    std::string clean = sanitize_utf8(chunk, lost);
    utf8_loss_ = utf8_loss_ || lost;
    bytes_written_ += chunk.size();
    ring_->append(clean, true);
}

void RingOutputSink::close() {}

std::size_t RingOutputSink::bytesWritten() const noexcept { return bytes_written_; }

bool RingOutputSink::truncated() const noexcept {
    return utf8_loss_ || ring_->truncated();
}

std::string RingOutputSink::materialize(std::size_t max_bytes) const {
    return ring_->tail(max_bytes);
}

} // namespace ymh
