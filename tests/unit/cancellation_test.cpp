#include <gtest/gtest.h>

#include "ymh/core/cancellation.hpp"

TEST(CancellationTokenTest, FreshSourceIsNotCancelled) {
    const ymh::CancellationSource source;
    const ymh::CancellationToken token = source.token();

    EXPECT_FALSE(token.cancelled());
    EXPECT_FALSE(token.is_cancelled());
    EXPECT_FALSE(source.cancelled());
}

TEST(CancellationTokenTest, CancelMarksTokenCancelled) {
    ymh::CancellationSource source;
    const ymh::CancellationToken token = source.token();

    source.cancel();

    EXPECT_TRUE(token.cancelled());
    EXPECT_TRUE(source.cancelled());
}

TEST(CancellationTokenTest, CallbacksFireExactlyOnceOnCancel) {
    ymh::CancellationSource source;
    const ymh::CancellationToken token = source.token();

    int calls = 0;
    token.on_cancel([&calls] { ++calls; });
    EXPECT_EQ(calls, 0);

    source.cancel();
    EXPECT_EQ(calls, 1);

    source.cancel();
    EXPECT_EQ(calls, 1);
}

TEST(CancellationTokenTest, CallbackRegisteredAfterCancelFiresImmediately) {
    ymh::CancellationSource source;
    const ymh::CancellationToken token = source.token();
    source.cancel();

    int calls = 0;
    token.on_cancel([&calls] { ++calls; });

    EXPECT_EQ(calls, 1);
}

TEST(CancellationTokenTest, ThrowIfCancelled) {
    ymh::CancellationSource source;
    const ymh::CancellationToken token = source.token();

    EXPECT_NO_THROW(token.throw_if_cancelled());

    source.cancel();

    EXPECT_THROW(token.throw_if_cancelled(), ymh::CancellationError);
}

TEST(CancellationTokenTest, CopiesShareTheSameState) {
    ymh::CancellationSource source;
    const ymh::CancellationToken first = source.token();
    const ymh::CancellationToken second = first;

    source.cancel();

    EXPECT_TRUE(first.cancelled());
    EXPECT_TRUE(second.cancelled());
}

TEST(CancellationTokenTest, DefaultTokenNeverCancels) {
    const ymh::CancellationToken token;

    EXPECT_FALSE(token.cancelled());
    EXPECT_NO_THROW(token.throw_if_cancelled());
    EXPECT_NO_THROW(token.on_cancel([] {}));
}
