#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/agent/chunk_coalescer.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

// 29-D4: `AssistantChunk` is published live-only; a new binary never appends
// it durably, so the coalescer's output is observed on the bus.
class LiveChunks {
public:
    explicit LiveChunks(EventBus& bus)
        : subscription_(bus.subscribe([this](const Event& event) {
              if (event.type == EventType::AssistantChunk) {
                  chunks.push_back(event.payload.get<payload::AssistantChunk>());
              }
          })) {}

    std::vector<payload::AssistantChunk> chunks;

private:
    Subscription subscription_;
};

TEST(ChunkCoalescer, FlushesOnBatchBoundaryAndBarrier) {
    ToolEnv env("coalesce_batch");
    LiveChunks live(env.bus);

    ChunkCoalescer coalescer(*env.session, "m1", 3, std::chrono::milliseconds{1000});
    coalescer.onText("a");
    coalescer.onText("b");
    EXPECT_EQ(live.chunks.size(), 0u);

    coalescer.onText("c");
    EXPECT_EQ(live.chunks.size(), 3u);

    coalescer.onText("d");
    EXPECT_EQ(live.chunks.size(), 3u);

    coalescer.flush();
    EXPECT_EQ(live.chunks.size(), 4u);

    std::size_t durable_chunks = 0;
    for (const EventRecord& record : env.session->events()) {
        if (record.event.type == EventType::AssistantChunk) {
            ++durable_chunks;
        }
    }
    EXPECT_EQ(durable_chunks, 0u);
}

TEST(ChunkCoalescer, FlushesOnInterval) {
    ToolEnv env("coalesce_interval");
    LiveChunks live(env.bus);

    ChunkCoalescer coalescer(*env.session, "m1", 32, std::chrono::milliseconds{0});
    coalescer.onText("a");
    coalescer.onText("b");
    EXPECT_EQ(live.chunks.size(), 2u);
}

TEST(ChunkCoalescer, PreservesOrderKindAndMonotonicIndex) {
    ToolEnv env("coalesce_order");
    LiveChunks live(env.bus);

    ChunkCoalescer coalescer(*env.session, "m1", 8, std::chrono::milliseconds{1000});
    coalescer.onText("a");
    coalescer.onReasoning("r");
    coalescer.onText("b");
    coalescer.flush();

    const std::vector<payload::AssistantChunk>& result = live.chunks;
    ASSERT_EQ(result.size(), 3u);
    EXPECT_EQ(result[0].kind, payload::AssistantChunkKind::Text);
    EXPECT_EQ(result[0].text, "a");
    EXPECT_EQ(result[0].index, 0u);
    EXPECT_EQ(result[1].kind, payload::AssistantChunkKind::Reasoning);
    EXPECT_EQ(result[1].text, "r");
    EXPECT_EQ(result[1].index, 1u);
    EXPECT_EQ(result[2].kind, payload::AssistantChunkKind::Text);
    EXPECT_EQ(result[2].text, "b");
    EXPECT_EQ(result[2].index, 2u);
    for (const payload::AssistantChunk& chunk : result) {
        EXPECT_EQ(chunk.message, "m1");
    }
}

} // namespace
