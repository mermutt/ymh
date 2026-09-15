#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <vector>

#include "support/test_env.hpp"
#include "ymh/agent/chunk_coalescer.hpp"

namespace {

using namespace ymh;
using namespace ymh::test;

std::vector<payload::AssistantChunk> chunks(const Session& session) {
    std::vector<payload::AssistantChunk> result;
    for (const EventRecord& record : session.events()) {
        if (record.event.type == EventType::AssistantChunk) {
            result.push_back(record.event.payload.get<payload::AssistantChunk>());
        }
    }
    return result;
}

TEST(ChunkCoalescer, FlushesOnBatchBoundaryAndBarrier) {
    ToolEnv env("coalesce_batch");

    ChunkCoalescer coalescer(*env.session, "m1", 3, std::chrono::milliseconds{1000});
    coalescer.onText("a");
    coalescer.onText("b");
    EXPECT_EQ(chunks(*env.session).size(), 0u);

    coalescer.onText("c");
    EXPECT_EQ(chunks(*env.session).size(), 3u);

    coalescer.onText("d");
    EXPECT_EQ(chunks(*env.session).size(), 3u);

    coalescer.flush();
    EXPECT_EQ(chunks(*env.session).size(), 4u);
}

TEST(ChunkCoalescer, FlushesOnInterval) {
    ToolEnv env("coalesce_interval");

    ChunkCoalescer coalescer(*env.session, "m1", 32, std::chrono::milliseconds{0});
    coalescer.onText("a");
    coalescer.onText("b");
    EXPECT_EQ(chunks(*env.session).size(), 2u);
}

TEST(ChunkCoalescer, PreservesOrderKindAndMonotonicIndex) {
    ToolEnv env("coalesce_order");

    ChunkCoalescer coalescer(*env.session, "m1", 8, std::chrono::milliseconds{1000});
    coalescer.onText("a");
    coalescer.onReasoning("r");
    coalescer.onText("b");
    coalescer.flush();

    const std::vector<payload::AssistantChunk> result = chunks(*env.session);
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
