#pragma once

// `/export` support (UI-local). Serializes a session's durable event log to a
// markdown file under the workspace root, optionally handing it to the user's
// editor. The transcript is reconstructed from the persisted event log, never
// from the trimmed TUI model, so the export is complete and ordered even when
// the view has scrolled or coalesced entries.
//
// Path resolution goes through `ExecutionEnvironment::resolve()` (root-relative,
// realpath-canonicalized; 00 §18, X1-X4), so an output path can never escape the
// workspace root and `getcwd()` is never a resolution base.

#include <ctime>
#include <filesystem>
#include <string>
#include <string_view>

#include "ymh/core/event.hpp"
#include "ymh/execution/environment.hpp"
#include "ymh/session/session.hpp"

namespace ymh::ui {

// Maximum length of the sanitized filename stem derived from a session title.
inline constexpr std::size_t kMaxExportStem = 64;

// Sanitizes a session title into a filename stem: whitespace, '/' and '\\'
// collapse to '-', other non-alphanumeric characters are dropped, the length is
// capped at `kMaxExportStem`, and trailing separators are trimmed. Returns ""
// when nothing usable remains (the caller falls back to "session").
[[nodiscard]] std::string sanitize_export_stem(std::string_view title);

// Default export filename: "<stem>-<YYYYMMDD-HHMMSS>.md" in UTC. An empty
// sanitized title falls back to "session".
[[nodiscard]] std::string export_filename(const std::string& title, std::time_t utc_now);

// Resolves the export target under `env.root()`. When `requested` is empty the
// default filename is derived from `title` and `utc_now`. Throws
// `ToolError{ToolErrorCode::PathEscape}` when the path resolves outside the
// workspace root.
[[nodiscard]] std::filesystem::path resolve_export_path(const ExecutionEnvironment& env,
                                                        const std::string& requested,
                                                        const std::string& title,
                                                        std::time_t utc_now);

// Deterministic markdown rendering of the session header plus its durable event
// log: user and assistant text, folded reasoning, tool calls with their
// arguments and outputs, and UTC timestamps.
[[nodiscard]] std::string render_session_markdown(const SessionHeader& header,
                                                  const EventRange& events);

// Writes `markdown` to `path`. Returns false (never throws) on I/O failure.
bool write_export_file(const std::filesystem::path& path, std::string_view markdown);

// $VISUAL, then $EDITOR, then "vi".
[[nodiscard]] std::string editor_from_environment();

// Forks and execs `editor` (whitespace-split) with `file` appended as the last
// argument, inheriting the caller's stdio, then waits for it. Returns the
// child's exit status, or -1 when it could not be launched.
int run_editor(const std::filesystem::path& file, const std::string& editor);

} // namespace ymh::ui
