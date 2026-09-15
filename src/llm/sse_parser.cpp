#include "ymh/llm/sse_parser.hpp"

#include <utility>

namespace ymh {

SseParser::SseParser(std::size_t max_line_bytes)
    : max_line_bytes_(max_line_bytes) {}

SseFeedStatus SseParser::feed(std::string_view bytes, const SseEventSink& sink) {
    if (overflowed_) {
        return SseFeedStatus::Overflow;
    }
    if (done_) {
        return SseFeedStatus::Ok;
    }

    for (const char c : bytes) {
        if (c == '\n') {
            std::string_view line = line_;
            if (!line.empty() && line.back() == '\r') {
                line.remove_suffix(1);
            }
            handleLine(line, sink);
            line_.clear();

            if (overflowed_) {
                return SseFeedStatus::Overflow;
            }
            if (done_) {
                return SseFeedStatus::Ok;
            }
            continue;
        }

        if (line_.size() >= max_line_bytes_) {
            overflowed_ = true;
            return SseFeedStatus::Overflow;
        }
        line_.push_back(c);
    }

    return SseFeedStatus::Ok;
}

void SseParser::finish(const SseEventSink& sink) {
    if (overflowed_ || done_) {
        return;
    }

    if (!line_.empty()) {
        std::string_view line = line_;
        if (line.back() == '\r') {
            line.remove_suffix(1);
        }
        handleLine(line, sink);
        line_.clear();
    }

    dispatch(sink);
}

void SseParser::handleLine(std::string_view line, const SseEventSink& sink) {
    if (line.empty()) {
        dispatch(sink);
        return;
    }

    if (line.front() == ':') {
        return;  // comment / keep-alive
    }

    constexpr std::string_view kDataField = "data:";
    if (line.substr(0, kDataField.size()) == kDataField) {
        std::string_view value = line.substr(kDataField.size());
        if (!value.empty() && value.front() == ' ') {
            value.remove_prefix(1);
        }
        if (has_data_) {
            data_.push_back('\n');
        }
        data_.append(value);
        has_data_ = true;
    }
    // `event:`, `id:`, `retry:`, and unknown fields are ignored (08 §6.2).
}

void SseParser::dispatch(const SseEventSink& sink) {
    if (!has_data_) {
        return;
    }

    SseEvent event;
    event.data = std::move(data_);
    data_.clear();
    has_data_ = false;

    if (event.data == "[DONE]") {
        event.done = true;
        done_ = true;
    }

    sink(event);
}

} // namespace ymh
