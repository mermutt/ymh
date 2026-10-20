#include "ymh/session/tool_result_text.hpp"

namespace ymh {
namespace {

void append_ascii(std::string& out, std::string_view text, std::size_t cap) {
    for (const char raw : text) {
        if (out.size() >= cap) {
            return;
        }
        const auto byte = static_cast<unsigned char>(raw);
        out.push_back(byte >= 0x20 && byte <= 0x7e ? raw : '?');
    }
}

} // namespace

std::string bounded_ascii(std::string_view text, std::size_t cap) {
    std::string out;
    out.reserve(cap);
    append_ascii(out, text, cap);
    if (out.size() < text.size() && cap >= 3) {
        out.resize(cap - 3);
        out += "...";
    }
    return out;
}

std::string denial_detail(std::string_view reason) {
    if (reason.empty()) return "unspecified";
    if (reason == "user") return "the user denied it";
    if (reason == "timeout") return "permission request timed out";
    if (reason == "cancelled") return "request cancelled";
    if (reason == "shutdown") return "daemon shut down";
    if (reason == "transport") return "transport failure";
    if (reason == "no-subscribers") return "no attached client";
    if (reason == "internal") return "internal error";
    if (reason == "no-resolver" || reason == "no permission resolver attached") {
        return "no permission resolver attached";
    }
    if (reason == "rule") return "denied by a permission rule";
    if (reason == "permission:protected-path") return "protected path";
    return bounded_ascii(reason, kMaxDenialReasonBytes);
}

bool is_canonical_denial(std::string_view text) noexcept {
    if (text.size() > kMaxDenialTextBytes) return false;
    if (!text.starts_with(kDeniedToolMarker)) return false;
    if (text.find(kDeniedNotExecuted) == std::string_view::npos) return false;
    for (const char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte < 0x20 || byte > 0x7e) return false;
    }
    return true;
}

std::string denial_text(std::string_view tool, std::string_view reason) {
    std::string out;
    out.reserve(kMaxDenialTextBytes);
    out += "DENIED: tool '";
    out += bounded_ascii(tool, kMaxDenialToolBytes);
    out += "' was NOT executed; permission was not granted (reason: ";
    out += denial_detail(reason);
    out += "). The tool did not run and produced no tool output; "
           "do not report this call as successful.";
    if (out.size() > kMaxDenialTextBytes) {
        out.resize(kMaxDenialTextBytes - 3);
        out += "...";
    }
    return out;
}

std::string tool_message_content(const payload::ToolResult& result) {
    if (result.outcome != payload::ToolOutcome::Denied) {
        return result.output;
    }
    if (is_canonical_denial(result.output)) {
        return result.output;
    }
    std::string_view reason = result.output;
    if (reason.empty() && result.error.has_value()) {
        reason = *result.error;
    }
    return denial_text(result.name, reason);
}

} // namespace ymh
