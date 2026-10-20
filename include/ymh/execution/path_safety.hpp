#pragma once

// Spec 90 (90-D13/H1/N3): lexical component-wise protection. True iff any
// root-relative component is exactly ".ymh" or ".git" (any depth,
// case-sensitive, no I/O, no symlink resolution). Used by both policy
// (evaluate/remember) and execution (LocalFilesystem::grep/glob, tool
// canonical checks) -- one shared implementation (LOW-6).

#include <filesystem>

namespace ymh {

// 90-D24 (Rev 9; simplified Rev 12): canonical /tmp only. Cached
// weakly_canonical("/tmp"); empty when /tmp is unavailable. No $TMPDIR, no
// $XDG_RUNTIME_DIR, no per-user base.
[[nodiscard]] const std::filesystem::path& canonical_tmp_root();

// 90-D23/PS28: lexical, component-wise, absolute, strict. Rejects an empty
// canonical root, a relative candidate, and any ".." component before
// normalization. `/tmp` itself is not under: equality is false.
[[nodiscard]] bool path_is_under_tmp_root(
    const std::filesystem::path& canonical_tmp_root,
    const std::filesystem::path& candidate);

[[nodiscard]] bool path_is_protected(
    const std::filesystem::path& canonical_root,
    const std::filesystem::path& candidate);

} // namespace ymh
