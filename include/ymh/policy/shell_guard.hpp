#pragma once

// Spec 89 (rev 3): the L1 shell-command safety classifier -- a pure, total,
// I/O-free fail-safe pre-permission guard. It sits as the FIRST branch of
// `RulePermissionPolicy::evaluate` (89-D1/89-D13). It is ADVISORY and a
// fail-safe clamp: a string classifier can never be "unbypassable" (89-D1).
// The real boundary is L2 kernel confinement; `Allow` requires BOTH
// (`L1.None => L2.on`, 89-D14).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

#include "ymh/policy/permission_policy.hpp"   // PermissionRequest

namespace ymh {

// 89-D1. `None` => normal policy; `Confirm` => fail-to-ASK; `Forbid` => hard
// Deny. Order: Forbid > Confirm > None. NEVER `None` for a destructive verb
// with unresolved/ambiguous operands (SG5).
enum class ShellCommandRisk : std::uint8_t { None, Confirm, Forbid };

// 90-D5: additive target scope. `NotApplicable` is a non-shell tool / empty
// text; `InRoot` means every provable filesystem target is in-root.
enum class ShellTargetScope : std::uint8_t {
    NotApplicable,
    InRoot,
    Outside,
    Unknown
};

// 90-D5: the guard risk plus the matching-only target scope and protected flag.
// 90-D23 (Rev 13): `tmp_exempt` is the I/O-free lexical precursor; the
// canonical/symlink/type checks live in `request_is_tmp_exempt`.
struct ShellClassification {
    ShellCommandRisk risk = ShellCommandRisk::None;
    ShellTargetScope scope = ShellTargetScope::NotApplicable;
    bool             protected_target = false;
    bool             tmp_exempt = false;
    std::vector<std::filesystem::path> tmp_targets;
};

// 89-D9/89-D7: the shell text the guard classifies plus the cwd it resolves
// relative operands against. `text` empty => a non-shell tool, the guard has no
// opinion.
struct ShellText {
    std::string           text;
    std::filesystem::path cwd;
};

// 89-D9: the text the guard classifies. Empty => non-shell tool, no opinion.
// shell          -> arguments["command"]
// terminal open  -> arguments["command"] or joined arguments["argv"]
// terminal write -> arguments["input"]
[[nodiscard]] std::string shell_guard_text(const PermissionRequest& request);

// 89-D1/89-D2..89-D9/89-D14/89-D17: pure, total, I/O-free. Never executes the
// command, never reads the filesystem, never resolves symlinks. `root` and
// `cwd` are canonical. `kernel_confinement == false` promotes in-root bounded
// destructive outcomes to Confirm (89-D14).
[[nodiscard]] ShellCommandRisk classify_shell_command(
    std::string_view text,
    const std::filesystem::path& root,
    const std::filesystem::path& cwd,
    bool kernel_confinement);

// 90-D5: `.risk` is exactly `classify_shell_command`; scope/protected are
// additive matching input. Pure/total, no I/O, no symlink resolution.
[[nodiscard]] ShellClassification classify_shell_command_with_scope(
    std::string_view text,
    const std::filesystem::path& root,
    const std::filesystem::path& cwd,
    bool kernel_confinement,
    const std::filesystem::path& canonical_tmp_root = {},
    bool relaxed_sed = false);

// 90-D22: request-level wrapper. Computes the declared `terminal open` cwd
// first, then classifies; empty text is NotApplicable for a non-shell tool and
// Unknown for shell/terminal, except a terminal open with a declared cwd.
// `terminal write` scope is forced Unknown (the PTY cwd is not modeled).
[[nodiscard]] ShellClassification classify_shell_request(
    const PermissionRequest& request,
    const std::filesystem::path& canonical_tmp_root = {});

// 90-D23/PS29: resolve-time temp verdict. Bounded per-target I/O: probes every
// path prefix for dangling/unresolvable symlinks and weakly-canonicalizes each
// `tmp_targets` entry; all but the declared terminal-open cwd must be regular
// files/new paths. Never true for path tools, reads, or a workspace at/under
// /tmp.
[[nodiscard]] bool request_is_tmp_exempt(
    const PermissionRequest& request,
    const ShellClassification& classification,
    const std::filesystem::path& canonical_tmp_root);

// 90-D7: one simple command (no unquoted `;`, `&&`, `||`, `|`, `&`, newline).
[[nodiscard]] bool is_simple_shell_command(std::string_view text);

// 90-D7: first executable word after 89-D4 wrapper/env stripping, only for a
// simple command; empty for compound/unparseable text.
[[nodiscard]] std::string command_prefix(std::string_view text);

// 89-D6: lexical containment. Relative candidates resolve against `root`.
// `single_quoted` suppresses $HOME expansion only; the `~` over-approximation
// is unconditional (SG4).
[[nodiscard]] bool path_within_root(const std::filesystem::path& root,
                                    const std::filesystem::path& candidate,
                                    bool single_quoted);

} // namespace ymh
