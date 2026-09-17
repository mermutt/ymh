#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <thread>

#include "ymh/ui/context_request.hpp"
#include "ymh/ui/ui_model.hpp"

namespace {

using namespace ymh;
using namespace ymh::ui;

// 18 §4.3 (M9/C1): the counter is atomic (written on the UI thread, read on the
// pump thread) and must never be a plain std::uint64_t (a TSAN-visible race).
TEST(ContextRequestGuard, CloseInvalidatesInFlightReply) {
    std::atomic<std::uint64_t> generation{0};
    const std::uint64_t request = bump_context_generation(generation);
    EXPECT_TRUE(context_reply_is_current(generation, request));
    (void)bump_context_generation(generation);
    EXPECT_FALSE(context_reply_is_current(generation, request));
}

TEST(ContextRequestGuard, NewerRequestSupersedesOlderReply) {
    std::atomic<std::uint64_t> generation{0};
    const std::uint64_t first = bump_context_generation(generation);
    const std::uint64_t second = bump_context_generation(generation);
    EXPECT_NE(first, second);
    EXPECT_FALSE(context_reply_is_current(generation, first));
    EXPECT_TRUE(context_reply_is_current(generation, second));
}

TEST(ContextRequestGuard, RelaxedCounterIsRaceFreeUnderConcurrentBumpAndRead) {
    std::atomic<std::uint64_t> generation{0};
    std::atomic<bool>          stop{false};
    std::thread                reader([&] {
        while (!stop.load(std::memory_order_relaxed)) {
            (void)context_reply_is_current(generation, generation.load(std::memory_order_relaxed));
        }
    });
    for (int index = 0; index < 10000; ++index) {
        (void)bump_context_generation(generation);
    }
    stop.store(true, std::memory_order_relaxed);
    reader.join();
    EXPECT_EQ(generation.load(std::memory_order_relaxed), 10000u);
}

TEST(ContextOverlayModel, DefaultsAreClosed) {
    ContextOverlayModel overlay;
    EXPECT_FALSE(overlay.open);
    EXPECT_FALSE(overlay.loaded);
    EXPECT_EQ(overlay.view, 0);
    EXPECT_EQ(overlay.scroll, 0);
    EXPECT_TRUE(overlay.note.empty());
}

TEST(ContextOverlayModel, UiModelCarriesOverlayState) {
    UiModel model;
    model.mode = UiMode::Context;
    model.context.open = true;
    model.context.loaded = true;
    model.context.session = SessionId{"s"};
    model.context.view = 1;
    EXPECT_EQ(model.mode, UiMode::Context);
    EXPECT_TRUE(model.context.open);
    EXPECT_EQ(model.context.view, 1);
}

} // namespace
