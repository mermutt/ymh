#include "ymh/cli/assistant_stream_printer.hpp"

#include <algorithm>
#include <ostream>
#include <utility>
#include <vector>

#include "ymh/agent/message.hpp"
#include "ymh/session/events.hpp"

namespace ymh {
namespace {

std::string assistant_text_of(const std::vector<ContentBlock>& content) {
    std::string text;
    for (const ContentBlock& block : content) {
        if (block.kind == ContentBlockKind::Text) {
            text += block.text;
        }
    }
    return text;
}

void append_bounded(std::string& buffer, const std::string& chunk) {
    if (buffer.size() >= AssistantStreamPrinter::kMaxBufferedBytes) {
        return;
    }
    const std::size_t room = AssistantStreamPrinter::kMaxBufferedBytes - buffer.size();
    buffer.append(chunk, 0, std::min(room, chunk.size()));
}

} // namespace

AssistantStreamPrinter::Commit AssistantStreamPrinter::feed(const Event& event, std::ostream& out,
                                                            std::ostream& err,
                                                            bool print_reasoning) {
    Commit commit;
    switch (event.type) {
        case EventType::AssistantChunk: {
            commit.handled = true;
            const auto& chunk = event.payload.get<payload::AssistantChunk>();
            if (chunk.message != message_) {
                message_ = chunk.message;
                text_.clear();
                reasoning_.clear();
            }
            if (chunk.kind == payload::AssistantChunkKind::Text) {
                append_bounded(text_, chunk.text);
            } else {
                append_bounded(reasoning_, chunk.text);
            }
            break;
        }
        case EventType::AssistantMessage: {
            commit.handled = true;
            const auto& message = event.payload.get<payload::AssistantMessage>();
            // 26-D9 / 35 §3.6: the durable settlement is the recovery path for a
            // dropped live chunk, so it wins; the buffer is a live-only fallback.
            const std::string durable = assistant_text_of(message.content);
            commit.text = durable.empty() ? text_ : durable;
            out << commit.text;
            out.flush();
            if (print_reasoning && !reasoning_.empty()) {
                err << reasoning_;
                err.flush();
            }
            message_.clear();
            text_.clear();
            reasoning_.clear();
            break;
        }
        case EventType::AssistantAttempt:
            commit.handled = true;
            message_.clear();
            text_.clear();
            reasoning_.clear();
            break;
        default:
            break;
    }
    return commit;
}

} // namespace ymh
