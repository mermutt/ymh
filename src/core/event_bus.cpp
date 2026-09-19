#include "ymh/core/event_bus.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ymh {
namespace detail {

namespace {

struct GlobalEntry {
    std::uint64_t id = 0;
    EventBus::Handler handler;
};

struct CommittedEntry {
    std::uint64_t id = 0;
    EventBus::RecordHandler handler;
};

struct SessionChannel {
    std::mutex                     mutex;
    std::vector<GlobalEntry>       subscribers;
    SessionMailbox                 mailbox;
};

} // namespace

struct EventBusState {
    std::mutex mutex;
    std::uint64_t next_subscription_id = 1;
    std::vector<GlobalEntry> global;
    std::vector<CommittedEntry> committed;
    std::unordered_map<std::string, std::shared_ptr<SessionChannel>> channels;

    std::uint64_t next_id() {
        return next_subscription_id++;
    }

    std::shared_ptr<SessionChannel> channel_for(const std::string& session) {
        const auto existing = channels.find(session);
        if (existing != channels.end()) {
            return existing->second;
        }
        auto channel = std::make_shared<SessionChannel>();
        channels.emplace(session, channel);
        return channel;
    }

    void unsubscribe(std::uint64_t id, Subscription::Channel channel_kind,
                     const std::optional<std::string>& session) {
        std::lock_guard<std::mutex> lock(mutex);

        if (channel_kind == Subscription::Channel::Committed) {
            std::erase_if(committed, [id](const CommittedEntry& entry) { return entry.id == id; });
            return;
        }

        if (channel_kind == Subscription::Channel::Global || !session.has_value()) {
            std::erase_if(global, [id](const GlobalEntry& entry) { return entry.id == id; });
            return;
        }

        const auto found = channels.find(*session);
        if (found == channels.end()) {
            return;
        }

        SessionChannel& session_channel = *found->second;
        {
            std::lock_guard<std::mutex> channel_lock(session_channel.mutex);
            std::erase_if(session_channel.subscribers,
                          [id](const GlobalEntry& entry) { return entry.id == id; });
            if (!session_channel.subscribers.empty()) {
                return;
            }
        }
        channels.erase(found);
    }
};

} // namespace detail

void SessionMailbox::push(Event event) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push_back(std::move(event));
}

bool SessionMailbox::drain(const std::function<bool(Event&&)>& sink) {
    for (;;) {
        Event event;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                return true;
            }
            event = std::move(queue_.front());
            queue_.pop_front();
        }

        if (!sink(std::move(event))) {
            return empty();
        }
    }
}

bool SessionMailbox::empty() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.empty();
}

std::size_t SessionMailbox::size() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
}

Subscription::Subscription(
    std::weak_ptr<detail::EventBusState> state,
    std::uint64_t                        id,
    std::optional<std::string>           session,
    Channel                              channel) noexcept
    : state_(std::move(state)), id_(id), session_(std::move(session)), channel_(channel) {}

Subscription::~Subscription() {
    unsubscribe();
}

Subscription::Subscription(Subscription&& other) noexcept
    : state_(std::move(other.state_)),
      id_(std::exchange(other.id_, 0)),
      session_(std::move(other.session_)),
      channel_(other.channel_) {
    other.session_.reset();
}

Subscription& Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        unsubscribe();
        state_   = std::move(other.state_);
        id_      = std::exchange(other.id_, 0);
        session_ = std::move(other.session_);
        channel_ = other.channel_;
        other.session_.reset();
    }
    return *this;
}

void Subscription::unsubscribe() noexcept {
    if (id_ != 0) {
        if (const std::shared_ptr<detail::EventBusState> state = state_.lock()) {
            state->unsubscribe(id_, channel_, session_);
        }
    }
    state_.reset();
    id_ = 0;
    session_.reset();
    channel_ = Channel::Global;
}

bool Subscription::active() const noexcept {
    return id_ != 0 && !state_.expired();
}

EventBus::EventBus() : state_(std::make_shared<detail::EventBusState>()) {}

EventBus::~EventBus() = default;

Subscription EventBus::subscribe(Handler handler) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const std::uint64_t id = state_->next_id();
    state_->global.push_back(detail::GlobalEntry{id, std::move(handler)});
    return Subscription{state_, id, std::nullopt, Subscription::Channel::Global};
}

Subscription EventBus::subscribe(const SessionId& session, Handler handler) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const std::uint64_t id = state_->next_id();
    const std::shared_ptr<detail::SessionChannel> channel = state_->channel_for(session.value);
    std::lock_guard<std::mutex> channel_lock(channel->mutex);
    channel->subscribers.push_back(detail::GlobalEntry{id, std::move(handler)});
    return Subscription{state_, id, session.value, Subscription::Channel::Session};
}

Subscription EventBus::subscribeCommitted(RecordHandler handler) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const std::uint64_t id = state_->next_id();
    state_->committed.push_back(detail::CommittedEntry{id, std::move(handler)});
    return Subscription{state_, id, std::nullopt, Subscription::Channel::Committed};
}

void EventBus::publish(Event event) {
    std::vector<Handler> global_handlers;
    std::shared_ptr<detail::SessionChannel> channel;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        global_handlers.reserve(state_->global.size());
        for (const detail::GlobalEntry& entry : state_->global) {
            global_handlers.push_back(entry.handler);
        }

        const auto found = state_->channels.find(event.session_id.value);
        if (found != state_->channels.end()) {
            channel = found->second;
        }
    }

    for (const Handler& handler : global_handlers) {
        handler(event);
    }

    if (channel == nullptr) {
        return;
    }

    channel->mailbox.push(std::move(event));
    channel->mailbox.drain([&channel](Event&& queued) {
        std::vector<Handler> session_handlers;
        {
            std::lock_guard<std::mutex> lock(channel->mutex);
            session_handlers.reserve(channel->subscribers.size());
            for (const detail::GlobalEntry& entry : channel->subscribers) {
                session_handlers.push_back(entry.handler);
            }
        }
        for (const Handler& handler : session_handlers) {
            handler(queued);
        }
        return true;
    });
}

void EventBus::publishCommitted(const EventRecord& record) {
    Event event = record.event;
    std::vector<Handler>                    global_handlers;
    std::vector<RecordHandler>              committed_handlers;
    std::shared_ptr<detail::SessionChannel> channel;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        global_handlers.reserve(state_->global.size());
        for (const detail::GlobalEntry& entry : state_->global) {
            global_handlers.push_back(entry.handler);
        }
        committed_handlers.reserve(state_->committed.size());
        for (const detail::CommittedEntry& entry : state_->committed) {
            committed_handlers.push_back(entry.handler);
        }

        const auto found = state_->channels.find(event.session_id.value);
        if (found != state_->channels.end()) {
            channel = found->second;
        }
    }

    for (const Handler& handler : global_handlers) {
        handler(event);
    }
    for (const RecordHandler& handler : committed_handlers) {
        handler(record);
    }

    if (channel == nullptr) {
        return;
    }

    channel->mailbox.push(std::move(event));
    channel->mailbox.drain([&channel](Event&& queued) {
        std::vector<Handler> session_handlers;
        {
            std::lock_guard<std::mutex> lock(channel->mutex);
            session_handlers.reserve(channel->subscribers.size());
            for (const detail::GlobalEntry& entry : channel->subscribers) {
                session_handlers.push_back(entry.handler);
            }
        }
        for (const Handler& handler : session_handlers) {
            handler(queued);
        }
        return true;
    });
}

SessionMailbox* EventBus::mailbox(const SessionId& session) noexcept {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->channels.find(session.value);
    if (found == state_->channels.end()) {
        return nullptr;
    }
    return &found->second->mailbox;
}

} // namespace ymh
