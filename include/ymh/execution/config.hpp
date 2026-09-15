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

    // glob/grep result bound (§6.3).
    std::size_t search_max_results{1000};

    // Model-facing description bound (§4.4).
    std::size_t tool_description_max_bytes{4u * 1024u};

    // Live output coalescing (§8.2).
    std::chrono::milliseconds output_flush_interval{33};      // ~30 Hz
    std::size_t               output_flush_bytes{64u * 1024u};

    // Subprocess/PTY teardown grace (§5.3, §6.5, §9.3).
    std::chrono::milliseconds terminate_grace{2'000};
};

} // namespace ymh
