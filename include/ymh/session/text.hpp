#pragma once

// 73-D1/79-D4.2.1: the shared Text-block projection. Promoted from an
// anonymous-namespace symbol in `src/agent/agent_loop.cpp` so the daemon
// (`HostRuntime::rewindTargets`) and the agent loop use the identical
// projection and a restored prompt round-trips unchanged.

#include <string>
#include <vector>

#include "ymh/agent/message.hpp"   // ContentBlock / ContentBlockKind

namespace ymh {

// The concatenation of every `Text` content block, in order; reasoning/tool/
// image blocks are excluded.
[[nodiscard]] inline std::string text_of_blocks(const std::vector<ContentBlock>& blocks) {
    std::string text;
    for (const ContentBlock& block : blocks) {
        if (block.kind == ContentBlockKind::Text) {
            text += block.text;
        }
    }
    return text;
}

} // namespace ymh
