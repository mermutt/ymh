#pragma once

// The Wave-4 retention library (40-output-retention.md §3.2). Byte- and
// code-point-oriented bounded output with exact omission metadata. The library
// answers only "what did we keep, what did we omit?"; tool-domain states stay in
// tool-domain fields and are never folded into retention metadata
// (26-dsh-alignment.md §2.2.3).

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "ymh/core/omission.hpp"

namespace ymh {

// dsh dsh-deque: circular, amortized O(1) push/pop at both ends; removed entries
// are cleared immediately and storage shrinks at a quarter live capacity.
// `capacity()` exposes that shrink for the §13.1 probe; it is the only capacity
// observable.
template <class T>
class Deque {
public:
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t capacity() const noexcept { return buffer_.size(); }

    void push_back(T value) {
        ensure_room();
        buffer_[(head_ + size_) % buffer_.size()] = std::move(value);
        ++size_;
    }

    void push_front(T value) {
        ensure_room();
        head_ = (head_ + buffer_.size() - 1) % buffer_.size();
        buffer_[head_] = std::move(value);
        ++size_;
    }

    [[nodiscard]] std::optional<T> pop_front() {
        if (size_ == 0) {
            return std::nullopt;
        }
        std::optional<T> value = std::move(buffer_[head_]);
        buffer_[head_].reset();
        head_ = (head_ + 1) % buffer_.size();
        --size_;
        shrink_if_sparse();
        return value;
    }

    void clear() noexcept {
        buffer_.clear();
        head_ = 0;
        size_ = 0;
    }

private:
    void ensure_room() {
        if (size_ < buffer_.size()) {
            return;
        }
        reallocate(buffer_.size() == 0 ? 1 : buffer_.size() * 2);
    }

    void shrink_if_sparse() {
        if (buffer_.size() <= 1 || size_ > buffer_.size() / 4) {
            return;
        }
        std::size_t target = buffer_.size();
        while (target > 1 && size_ <= target / 4) {
            target /= 2;
        }
        reallocate(target);
    }

    void reallocate(std::size_t next) {
        std::vector<std::optional<T>> grown(next);
        for (std::size_t i = 0; i < size_; ++i) {
            grown[i] = std::move(buffer_[(head_ + i) % buffer_.size()]);
        }
        buffer_ = std::move(grown);
        head_   = 0;
    }

    std::vector<std::optional<T>> buffer_;
    std::size_t                   head_ = 0;
    std::size_t                   size_ = 0;
};

// dsh dsh-chunked-list: persistent append-only list; appending copies at most
// one 64-value chunk and shares the unchanged older chunks. Empty is `nullopt`.
template <class T>
struct ChunkedList {
    static constexpr std::size_t          kChunkSize = 64;
    std::vector<T>                        values;    // newest chunk, insertion order
    std::shared_ptr<const ChunkedList<T>> previous;  // absent == oldest chunk
};

template <class T>
[[nodiscard]] ChunkedList<T> append_chunked_list(std::optional<ChunkedList<T>> head, T value) {
    if (!head.has_value() || head->values.size() >= ChunkedList<T>::kChunkSize) {
        ChunkedList<T> next;
        next.values.push_back(std::move(value));
        if (head.has_value()) {
            next.previous = std::make_shared<const ChunkedList<T>>(std::move(*head));
        }
        return next;
    }
    ChunkedList<T> next = std::move(*head);
    next.values.push_back(std::move(value));
    return next;
}

template <class T>
[[nodiscard]] std::vector<T> iterate_chunked_list(const std::optional<ChunkedList<T>>& head) {
    std::vector<const ChunkedList<T>*> chunks;
    for (const ChunkedList<T>* node = head ? &*head : nullptr; node != nullptr;
         node = node->previous.get()) {
        chunks.push_back(node);
    }
    std::vector<T> values;
    for (auto it = chunks.rbegin(); it != chunks.rend(); ++it) {
        values.insert(values.end(), (*it)->values.begin(), (*it)->values.end());
    }
    return values;
}

struct PushDecision {
    bool    accepted = true;
    Omitted omitted;
};

template <class T>
struct RetainedItems {
    std::vector<T> items;
    Omitted        omitted;
};

struct RetainedText {
    std::string text;
    Omitted     omitted;
};

enum class TextRetentionStrategy : std::uint8_t {
    Head,
    Tail,
    HeadTail,
};

struct RetentionNotice {
    Omitted                  omitted;
    std::vector<std::string> omitted_labels;
};

using RecoveryTextFn = std::function<std::string(const RetentionNotice&)>;

template <class T>
class ItemRetainer {
public:
    explicit ItemRetainer(std::size_t max_items) : max_items_(max_items) {}

    PushDecision push(T value) {
        if (items_.size() < max_items_) {
            items_.push_back(std::move(value));
            return PushDecision{true, Omitted{}};
        }
        ++omitted_count_;
        return PushDecision{false, Omitted{OmittedKind::Exact, omitted_count_}};
    }

    [[nodiscard]] RetainedItems<T> finish() const {
        RetainedItems<T> result;
        result.items = items_;
        if (omitted_count_ > 0) {
            result.omitted = Omitted{OmittedKind::Exact, omitted_count_};
        }
        return result;
    }

private:
    std::size_t    max_items_ = 0;
    std::vector<T> items_;
    std::size_t    omitted_count_ = 0;
};

class TextRetainer {
public:
    explicit TextRetainer(TextRetentionStrategy strategy, std::size_t max_bytes);
    PushDecision push(std::span<const std::byte> bytes);
    [[nodiscard]] RetainedText finish() const;

private:
    [[nodiscard]] std::size_t omitted_bytes() const noexcept;

    std::size_t    head_budget_ = 0;
    std::size_t    tail_budget_ = 0;
    std::string    head_;
    std::size_t    head_escaped_ = 0;
    std::deque<char> tail_;
    std::size_t    tail_escaped_ = 0;
    std::size_t    total_bytes_ = 0;
};

[[nodiscard]] std::string format_retention_notice(const RetentionNotice&, RecoveryTextFn);

} // namespace ymh
