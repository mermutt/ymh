#pragma once

// Vision-capability gate for outbound requests. A model whose resolved
// profile declares `capabilities.vision = false` must never receive image
// blocks; this helper rewrites them to visible text notes instead so the
// model can tell the user why it cannot see the image. Pure and provider
// independent so both the agent loop and tests share it.

#include <cstddef>
#include <string_view>
#include <vector>

#include "ymh/agent/message.hpp"

namespace ymh {

// Replace every `ContentBlockKind::Image` block in `messages` with a Text
// block `[image <media-type> omitted: <reason>]`. Returns the number of
// blocks replaced. Non-image blocks are untouched.
[[nodiscard]] std::size_t degrade_prompt_images(std::vector<Message>& messages,
                                                std::string_view        reason);

} // namespace ymh
