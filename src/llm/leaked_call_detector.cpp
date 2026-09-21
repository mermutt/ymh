#include "ymh/llm/leaked_call_detector.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ymh {
namespace {

constexpr std::string_view kWhitespace = " \t\r\n";
constexpr std::string_view kOpenTag    = "<tool_call>";
constexpr std::string_view kCloseTag   = "</tool_call>";

constexpr std::string_view kChannelStart = "<|start|>assistant";
constexpr std::string_view kChannelMsg   = "<|message|>";
constexpr std::string_view kChannelEom   = "<|eom|>";
constexpr std::string_view kChannelEot   = "<|eot|>";

constexpr std::string_view kInvokeOpen  = "<atem:invoke";
constexpr std::string_view kInvokeClose = "</atem:invoke>";
constexpr std::string_view kNameAttr    = "name=\"";

bool is_name_boundary(char c) noexcept {
    return !(c == '_' || (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
             (c >= 'a' && c <= 'z'));
}

void collect_invokes(std::string_view body, std::vector<std::string>& names) {
    std::size_t pos = 0;
    while (true) {
        const std::size_t open = body.find(kInvokeOpen, pos);
        if (open == std::string_view::npos) {
            return;
        }
        const std::size_t tag_end = body.find('>', open + kInvokeOpen.size());
        if (tag_end == std::string_view::npos) {
            return;
        }
        const std::size_t close = body.find(kInvokeClose, tag_end);
        if (close == std::string_view::npos) {
            return;
        }

        const std::string_view tag = body.substr(open, tag_end - open + 1);
        std::size_t            attr = tag.find(kNameAttr);
        while (attr != std::string_view::npos) {
            const bool boundary =
                attr == 0 || is_name_boundary(tag[attr - 1]);
            const std::size_t value_start = attr + kNameAttr.size();
            const std::size_t value_end   = tag.find('"', value_start);
            if (boundary && value_end != std::string_view::npos) {
                names.emplace_back(tag.substr(value_start, value_end - value_start));
                break;
            }
            attr = tag.find(kNameAttr, attr + 1);
        }
        pos = close + kInvokeClose.size();
    }
}

void scan_messages(std::string_view content, std::vector<std::string>& names) {
    bool        saw_message = false;
    std::size_t pos         = 0;
    while (true) {
        const std::size_t start = content.find(kChannelStart, pos);
        if (start == std::string_view::npos) {
            break;
        }
        std::size_t after = start + kChannelStart.size();
        std::string recipient;
        if (content.substr(after, 4) == " to=") {
            const std::size_t recipient_start = after + 4;
            const std::size_t recipient_end =
                content.find_first_of(" \t\r\n<", recipient_start);
            if (recipient_end == std::string_view::npos) {
                break;
            }
            recipient = std::string(content.substr(recipient_start,
                                                   recipient_end - recipient_start));
            after = recipient_end;
        }

        const std::size_t message = content.find(kChannelMsg, after);
        if (message == std::string_view::npos) {
            break;
        }
        const std::size_t body_start = message + kChannelMsg.size();

        std::size_t body_end = content.size();
        if (const std::size_t eom = content.find(kChannelEom, body_start);
            eom != std::string_view::npos && eom < body_end) {
            body_end = eom;
        }
        if (const std::size_t eot = content.find(kChannelEot, body_start);
            eot != std::string_view::npos && eot < body_end) {
            body_end = eot;
        }

        saw_message = true;
        if (recipient != "self" && recipient != "user") {
            collect_invokes(content.substr(body_start, body_end - body_start), names);
        }

        if (body_end >= content.size()) {
            break;
        }
        if (content.substr(body_end, kChannelEom.size()) == kChannelEom) {
            pos = body_end + kChannelEom.size();
        } else if (content.substr(body_end, kChannelEot.size()) == kChannelEot) {
            pos = body_end + kChannelEot.size();
        } else {
            pos = body_end;
        }
    }

    if (!saw_message) {
        const std::size_t first = content.find_first_not_of(kWhitespace);
        if (first == std::string_view::npos) {
            return;
        }
        const std::size_t last = content.find_last_not_of(kWhitespace);
        collect_invokes(content.substr(first, last - first + 1), names);
    }
}

} // namespace

LeakedParse parse_leaked_json_call(std::string_view content) {
    LeakedParse result;

    const std::size_t first = content.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return result;
    }
    const std::size_t last = content.find_last_not_of(kWhitespace);
    const std::string_view text = content.substr(first, last - first + 1);

    std::size_t pos = 0;
    while (pos < text.size()) {
        const std::size_t token = text.find_first_not_of(kWhitespace, pos);
        if (token == std::string_view::npos) {
            break;
        }
        pos = token;
        if (text.substr(pos, kOpenTag.size()) != kOpenTag) {
            return {};
        }
        const std::size_t body_start = pos + kOpenTag.size();
        const std::size_t close      = text.find(kCloseTag, body_start);
        if (close == std::string_view::npos) {
            return {};
        }
        result.complete_block_seen = true;

        const std::string_view body = text.substr(body_start, close - body_start);
        const nlohmann::json   parsed = nlohmann::json::parse(body, nullptr, false);

        bool           ok        = false;
        std::string    name;
        nlohmann::json arguments = nlohmann::json::object();
        if (!parsed.is_discarded() && parsed.is_object()) {
            const nlohmann::json* fn = &parsed;
            if (parsed.contains("function") && parsed["function"].is_object()) {
                fn = &parsed["function"];
            }
            if (fn->contains("name") && (*fn)["name"].is_string()) {
                name = (*fn)["name"].get<std::string>();
                if (fn->contains("arguments")) {
                    arguments = (*fn)["arguments"];
                }
                ok = true;
            }
        }
        if (ok) {
            result.calls.push_back(LeakedCall{std::move(name), std::move(arguments)});
        } else {
            result.malformed_block_seen = true;
        }

        pos = close + kCloseTag.size();
    }

    if (!result.complete_block_seen) {
        return {};
    }
    return result;
}

std::optional<std::vector<std::string>> detect_native_atem_calls(std::string_view content) {
    std::vector<std::string> names;
    scan_messages(content, names);
    if (names.empty()) {
        return std::nullopt;
    }
    return names;
}

} // namespace ymh
