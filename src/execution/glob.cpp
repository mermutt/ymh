#include "ymh/execution/glob.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace ymh {
namespace {

bool flat_match(std::string_view pattern, std::string_view text) {
    std::size_t p = 0;
    std::size_t t = 0;
    std::size_t star = std::string_view::npos;
    std::size_t resume = 0;

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            resume = t;
        } else if (star != std::string_view::npos) {
            p = star + 1;
            t = ++resume;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

std::vector<std::string_view> split_segments(std::string_view path) {
    std::vector<std::string_view> segments;
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t slash = path.find('/', start);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        if (end > start) {
            segments.push_back(path.substr(start, end - start));
        }
        if (slash == std::string_view::npos) {
            break;
        }
        start = slash + 1;
    }
    return segments;
}

bool match_segments(const std::vector<std::string_view>& pattern,
                    const std::vector<std::string_view>& text,
                    std::size_t pi,
                    std::size_t ti) {
    if (pi == pattern.size()) {
        return ti == text.size();
    }
    if (pattern[pi] == "**") {
        if (match_segments(pattern, text, pi + 1, ti)) {
            return true;
        }
        for (std::size_t k = ti; k < text.size(); ++k) {
            if (match_segments(pattern, text, pi + 1, k + 1)) {
                return true;
            }
        }
        return false;
    }
    if (ti == text.size() || !flat_match(pattern[pi], text[ti])) {
        return false;
    }
    return match_segments(pattern, text, pi + 1, ti + 1);
}

} // namespace

bool glob_match(std::string_view pattern, std::string_view text) {
    return flat_match(pattern, text);
}

bool path_glob_match(std::string_view pattern, std::string_view text) {
    const std::vector<std::string_view> pattern_segments = split_segments(pattern);
    const std::vector<std::string_view> text_segments = split_segments(text);
    return match_segments(pattern_segments, text_segments, 0, 0);
}

} // namespace ymh
