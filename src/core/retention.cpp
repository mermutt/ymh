#include "ymh/core/retention.hpp"

#include <cstddef>
#include <string>
#include <string_view>

namespace ymh {
namespace {

// Worst-case JSON-escaped width of one raw byte under nlohmann's default
// `dump()` (control chars become `\u00XX`; quote/backslash become two bytes;
// everything else is emitted verbatim).
[[nodiscard]] constexpr std::size_t escaped_width(unsigned char byte) noexcept {
    if (byte == '"' || byte == '\\') {
        return 2;
    }
    if (byte < 0x20) {
        return 6;
    }
    return 1;
}

// Drop a trailing incomplete UTF-8 sequence so a prefix cut never emits a
// replacement char at the boundary (dsh trimTrailingPartialUtf8).
[[nodiscard]] std::string_view trim_trailing_partial_utf8(std::string_view bytes) noexcept {
    if (bytes.empty()) {
        return bytes;
    }
    std::ptrdiff_t index = static_cast<std::ptrdiff_t>(bytes.size()) - 1;
    while (index >= 0 &&
           (static_cast<unsigned char>(bytes[static_cast<std::size_t>(index)]) & 0xC0) == 0x80 &&
           bytes.size() - static_cast<std::size_t>(index) <= 3) {
        --index;
    }
    if (index < 0) {
        return bytes;
    }
    const auto        lead     = static_cast<unsigned char>(bytes[static_cast<std::size_t>(index)]);
    const std::size_t expected = lead < 0x80   ? 1
                                 : lead < 0xE0 ? 2
                                 : lead < 0xF0 ? 3
                                 : lead < 0xF8 ? 4
                                               : 0;
    if (expected == 0) {
        return bytes;
    }
    return bytes.size() - static_cast<std::size_t>(index) < expected
               ? bytes.substr(0, static_cast<std::size_t>(index))
               : bytes;
}

// Drop leading continuation bytes so a suffix cut starts on a lead byte (dsh
// trimLeadingContinuationUtf8).
[[nodiscard]] std::string_view trim_leading_continuation_utf8(std::string_view bytes) noexcept {
    std::size_t index = 0;
    while (index < bytes.size() && (static_cast<unsigned char>(bytes[index]) & 0xC0) == 0x80) {
        ++index;
    }
    return bytes.substr(index);
}

} // namespace

TextRetainer::TextRetainer(TextRetentionStrategy strategy, std::size_t max_bytes) {
    switch (strategy) {
        case TextRetentionStrategy::Head:
            head_budget_ = max_bytes;
            break;
        case TextRetentionStrategy::Tail:
            tail_budget_ = max_bytes;
            break;
        case TextRetentionStrategy::HeadTail:
            head_budget_ = max_bytes / 2;
            tail_budget_ = max_bytes - head_budget_;
            break;
    }
}

std::size_t TextRetainer::omitted_bytes() const noexcept {
    const std::size_t retained = head_.size() + tail_.size();
    return total_bytes_ > retained ? total_bytes_ - retained : 0;
}

PushDecision TextRetainer::push(std::span<const std::byte> bytes) {
    const std::size_t before = omitted_bytes();
    for (const std::byte raw : bytes) {
        const auto        byte  = static_cast<unsigned char>(raw);
        const std::size_t width = escaped_width(byte);
        if (head_escaped_ + width <= head_budget_) {
            head_.push_back(static_cast<char>(byte));
            head_escaped_ += width;
        }
        if (tail_budget_ > 0) {
            tail_.push_back(static_cast<char>(byte));
            tail_escaped_ += width;
            while (!tail_.empty() && tail_escaped_ > tail_budget_) {
                tail_escaped_ -= escaped_width(static_cast<unsigned char>(tail_.front()));
                tail_.pop_front();
            }
        }
    }
    total_bytes_ += bytes.size();

    const std::size_t after = omitted_bytes();
    return PushDecision{after == before,
                        Omitted{after > 0 ? OmittedKind::Exact : OmittedKind::None, after}};
}

RetainedText TextRetainer::finish() const {
    const std::size_t head_size = head_.size();
    const std::size_t tail_size = tail_.size();

    RetainedText result;
    if (head_size + tail_size >= total_bytes_) {
        // Nothing was omitted by budget, so the two sides are adjacent slices of
        // one stream: reconstruct the contiguous whole.
        result.text = head_;
        if (tail_size > 0) {
            const std::string tail(tail_.begin(), tail_.end());
            result.text.append(tail, head_size + tail_size - total_bytes_, std::string::npos);
        }
        return result;
    }

    const std::string_view head_text = trim_trailing_partial_utf8(head_);
    const std::string      tail(tail_.begin(), tail_.end());
    const std::string_view tail_text = trim_leading_continuation_utf8(tail);
    result.text.assign(head_text);
    result.text.append(tail_text);

    const std::size_t omitted = total_bytes_ - result.text.size();
    if (omitted > 0) {
        result.omitted = Omitted{OmittedKind::Exact, omitted};
    }
    return result;
}

std::string format_retention_notice(const RetentionNotice& notice, RecoveryTextFn recovery) {
    std::string clause;
    if (notice.omitted.kind != OmittedKind::None) {
        std::string labels;
        for (const std::string& label : notice.omitted_labels) {
            if (!labels.empty()) {
                labels += ", ";
            }
            labels += label;
        }
        if (notice.omitted.kind == OmittedKind::Exact) {
            clause = "Omitted " + std::to_string(notice.omitted.count);
            if (!labels.empty()) {
                clause += " " + labels;
            }
            clause += ".";
        } else {
            clause = labels.empty() ? std::string{"More were omitted."}
                                    : "More " + labels + " were omitted.";
        }
    }

    std::string recovery_text = recovery ? recovery(notice) : std::string{};
    if (clause.empty()) {
        return recovery_text;
    }
    if (recovery_text.empty()) {
        return clause;
    }
    return clause + " " + recovery_text;
}

} // namespace ymh
