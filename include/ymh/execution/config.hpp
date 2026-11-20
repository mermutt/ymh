#pragma once

// 07 §5.5 `ToolConfig`: the tunable bounds named across the tools/execution
// spec. Pinned here (not in `tools/`) so the execution layer
// (`LocalEnvironment`, `LocalFilesystem`, `LocalProcessService`) can consume the
// bounds without depending on the tool layer — the dependency direction is
// tools → execution, never the reverse.

#include <chrono>
#include <cstddef>

namespace ymh {

struct ToolConfig {
    // Durable output clamp on the serialized payload (§5.2, X10). Must be
    // <= PersistenceConfig::max_payload_bytes minus envelope headroom.
    std::size_t tool_result_max_bytes{1u * 1024u * 1024u};

    // Filesystem read bound (§6.3).
    std::size_t read_file_max_bytes{256u * 1024u};

    // Tool-produced image bound (`read_image`): larger files are rejected
    // instead of being truncated into an unusable base64 payload.
    std::size_t read_image_max_bytes{2u * 1024u * 1024u};

    // glob/grep result bound (§6.3).
    std::size_t search_max_results{1000};

    // Model-facing description bound (§4.4).
    std::size_t tool_description_max_bytes{4u * 1024u};

    // Live output coalescing (§8.2).
    std::chrono::milliseconds output_flush_interval{33};      // ~30 Hz
    std::size_t               output_flush_bytes{64u * 1024u};

    // Subprocess/PTY teardown grace (§5.3, §6.5, §9.3).
    std::chrono::milliseconds terminate_grace{2'000};

    // Outbound PTY write queue bound (14 §3.2, E-P8). A `write` is admitted
    // only if the whole payload fits; the pump drains in <= 64 KiB chunks.
    std::size_t pty_write_queue_cap{256u * 1024u};

    // 46-D8: the generic per-tool-run deadline. `0` disables the tool-deadline
    // layer entirely; it is never an immediate timeout.
    std::chrono::milliseconds tool_timeout{300'000};

    // 120-D5: edit-tool locals. These bounds do not change the filesystem read
    // bound (`read_file_max_bytes`) or the durable result bound
    // (`tool_result_max_bytes`).
    std::size_t edit_max_edits{100};                       // multi_edit entries
    std::size_t edit_patch_max_bytes{256u * 1024u};        // apply_patch `patch`
    std::size_t edit_patch_max_hunks{200};                 // apply_patch hunks
    std::size_t edit_diff_max_bytes{64u * 1024u};          // result diff section
};

} // namespace ymh
