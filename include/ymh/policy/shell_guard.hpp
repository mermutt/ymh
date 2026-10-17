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

// 89-D6: lexical containment. Relative candidates resolve against `root`.
// `single_quoted` suppresses $HOME expansion only; the `~` over-approximation
// is unconditional (SG4).
[[nodiscard]] bool path_within_root(const std::filesystem::path& root,
                                    const std::filesystem::path& candidate,
                                    bool single_quoted);

} // namespace ymh
