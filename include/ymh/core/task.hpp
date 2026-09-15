#pragma once

// Minimal `Task<T>` value type (00-architecture.md §34/§35).
//
// Specs 06/07/08 pin async-looking seam signatures (`Task<LLMResponse>`,
// `Task<ToolResult>`, `Task<void>`, …) but neither §34 nor §35 pins the
// concrete `Task` shape, and no executor exists yet (scheduling is spec 06's,
// 08 §14.1(q)). The LLM provider is explicitly synchronous with respect to its
// execution context — it owns no threads, `io_context`, or detached tasks and
// runs to completion on the thread that invokes `stream()` (08 §9, L13) — so
// the faithful minimal materialisation is an *eager* task that already holds
// its result.
//
// This is deliberately small: it exists so the pinned `Task<T>` seam compiles
// and can be awaited/consumed without inventing an async runtime in the LLM
// wave. A later wave may replace the internals with a real coroutine/executor
// type without changing the seam signatures. `Task` carries a value only;
// provider failures are values (`LLMResponse.error`, `HttpResponse`) and never
// exceptions (08 §2.2, L16).

#include <cassert>
#include <optional>
#include <type_traits>
#include <utility>

namespace ymh {

template <typename T>
class Task {
public:
    Task() = default;

    explicit Task(T value) noexcept(std::is_nothrow_move_constructible_v<T>)
        : value_(std::move(value)) {}

    Task(Task&&) noexcept = default;
    Task& operator=(Task&&) noexcept = default;

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // An eager task is ready as soon as it is returned.
    [[nodiscard]] bool ready() const noexcept { return value_.has_value(); }
    [[nodiscard]] bool valid() const noexcept { return value_.has_value(); }

    T& get() & {
        assert(value_.has_value());
        return *value_;
    }

    const T& get() const& {
        assert(value_.has_value());
        return *value_;
    }

    T&& get() && {
        assert(value_.has_value());
        return std::move(*value_);
    }

private:
    std::optional<T> value_;
};

template <>
class Task<void> {
public:
    Task() = default;

    [[nodiscard]] bool ready() const noexcept { return true; }
    [[nodiscard]] bool valid() const noexcept { return true; }

    void get() const noexcept {}
};

} // namespace ymh
