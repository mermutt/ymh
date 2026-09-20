#include "ymh/agent/repeat_tool_reminder.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>

namespace ymh {
namespace {

constexpr std::string_view kGentleText =
    "You are repeating the exact same tool call with identical arguments. "
    "Carefully analyze the previous result before calling again: if the task is "
    "not complete, try a different approach or different arguments instead of "
    "repeating the call.";

std::string detailed_text(const std::string& name,
                          std::size_t        count,
                          const std::string& arguments_preview) {
    return "Repeated tool call detected:\n"
           "- tool: `" +
           name +
           "`\n"
           "- consecutive_calls: `" +
           std::to_string(count) +
           "`\n"
           "- arguments: `" +
           arguments_preview +
           "`\n"
           "The repeated calls are not making progress. Do not call this tool with these "
           "exact arguments again. Inspect the latest result and choose a different "
           "action, different arguments, or finish the task if enough evidence has been "
           "gathered.";
}

} // namespace

RepeatToolReminder::RepeatToolReminder(RepeatToolReminderConfig config)
    : config_(std::move(config)) {
    std::sort(config_.thresholds.begin(), config_.thresholds.end());
}

std::optional<ContextMessage> RepeatToolReminder::observe(const ToolCallAssembled& committed) {
    const std::string canonical = committed.arguments.dump();
    if (committed.name == name_ && canonical == canonical_args_) {
        ++count_;
    } else {
        name_           = committed.name;
        canonical_args_ = canonical;
        count_          = 1;
        fired_.clear();
    }

    for (std::size_t index = 0; index < config_.thresholds.size(); ++index) {
        const std::size_t threshold = config_.thresholds[index];
        if (count_ != threshold) {
            continue;
        }
        if (std::find(fired_.begin(), fired_.end(), threshold) != fired_.end()) {
            return std::nullopt;
        }
        fired_.push_back(threshold);

        ContextMessage message;
        message.role    = Role::User;
        message.context = ContextFormed{ContextForm::Notice, {}, "repeated tool call"};
        message.source  = message_source(MessageSource::Kind::Plugin);
        message.source.plugin = "repeat-tool-reminder";
        if (index == 0) {
            message.text = std::string{kGentleText};
        } else {
            message.text =
                detailed_text(committed.name, count_,
                              canonical.substr(0, config_.arguments_preview_chars));
        }
        return message;
    }
    return std::nullopt;
}

} // namespace ymh
