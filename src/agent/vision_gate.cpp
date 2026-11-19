#include "ymh/agent/vision_gate.hpp"

#include <string>
#include <utility>

namespace ymh {

std::size_t degrade_prompt_images(std::vector<Message>& messages,
                                  std::string_view        reason) {
    std::size_t degraded = 0;
    for (Message& message : messages) {
        for (ContentBlock& block : message.content) {
            if (block.kind != ContentBlockKind::Image) {
                continue;
            }
            std::string note = "[image";
            if (!block.media_type.empty()) {
                note.push_back(' ');
                note.append(block.media_type);
            }
            note.append(" omitted: ");
            note.append(reason);
            note.push_back(']');

            block = ContentBlock{};
            block.kind = ContentBlockKind::Text;
            block.text = std::move(note);
            ++degraded;
        }
    }
    return degraded;
}

} // namespace ymh
