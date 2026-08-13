/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/EventCoalescer.h"
#include "ai/openai/codex/frontend/UpdateBatch.h"
#include "support/TestResult.h"

#include <cstddef>
#include <string>

namespace {
    namespace frontend = ai::openai::codex::frontend;

    void testCoalescingAndBounds(tests::support::TestResult& result) {
        frontend::EventCoalescer coalescer({32});
        const frontend::CoalescingKey agentKey = frontend::CoalescingKey::itemContent("thread-1", "turn-1", "item-1", "agentText");

        std::string accumulated;
        std::size_t schedulingRequests = 0;
        for (std::size_t index = 0; index < 1000; ++index) {
            accumulated += static_cast<char>('a' + static_cast<int>(index % 26));
            const frontend::CoalescerMarkResult marked = coalescer.mark({agentKey,
                                                                         "item.content.updated",
                                                                         frontend::Json{{"threadId", "thread-1"},
                                                                                        {"turnId", "turn-1"},
                                                                                        {"itemId", "item-1"},
                                                                                        {"channel", "agentText"},
                                                                                        {"content", accumulated}},
                                                                         frontend::FlushUrgency::Deferred});
            schedulingRequests += marked.scheduleRequired ? 1U : 0U;
        }
        result.expectTrue(coalescer.dirtyCount() == 1 && schedulingRequests == 1 && coalescer.flushScheduled(),
                          "1,000 raw deltas dirty one entity and request exactly one next-tick flush");

        const frontend::CoalescerDrainResult drained = coalescer.drain();
        result.expectTrue(drained.updates.size() == 1 && drained.updates.front().data["content"] == accumulated,
                          "coalescing preserves the exact final accumulated agent text");

        const frontend::FrontendEvent coalescedEvent{
            frontend::SequenceNumber{1}, drained.updates.front().type, drained.updates.front().data, frontend::Json::object()};
        frontend::UpdateBatchBuilder batches({16, 16U * 1024U});
        const auto built = batches.build({coalescedEvent});
        result.expectTrue(built.success() && built.batches.size() == 1 && built.batches.front().batch.events.size() == 1 &&
                              built.batches.front().batch.events.size() < 1000,
                          "1,000 token deltas become one normalized frontend message, substantially below raw granularity");

        const auto reasoning = coalescer.mark({frontend::CoalescingKey::itemContent("thread-1", "turn-1", "item-1", "reasoningText"),
                                               "item.content.updated",
                                               frontend::Json{{"threadId", "thread-1"},
                                                              {"turnId", "turn-1"},
                                                              {"itemId", "item-1"},
                                                              {"channel", "reasoningText"},
                                                              {"content", "reasoning-final"}},
                                               frontend::FlushUrgency::Deferred});
        const auto commandOutput = coalescer.mark({frontend::CoalescingKey::itemContent("thread-1", "turn-1", "item-1", "commandOutput"),
                                                   "item.content.updated",
                                                   frontend::Json{{"threadId", "thread-1"},
                                                                  {"turnId", "turn-1"},
                                                                  {"itemId", "item-1"},
                                                                  {"channel", "commandOutput"},
                                                                  {"content", "command-final"}},
                                                   frontend::FlushUrgency::Deferred});
        const auto otherTurn = coalescer.mark({frontend::CoalescingKey::itemContent("thread-1", "turn-2", "item-1", "reasoningText"),
                                               "item.content.updated",
                                               frontend::Json{{"threadId", "thread-1"},
                                                              {"turnId", "turn-2"},
                                                              {"itemId", "item-1"},
                                                              {"channel", "reasoningText"},
                                                              {"content", "other-turn"}},
                                               frontend::FlushUrgency::Deferred});
        result.expectTrue(reasoning.accepted() && commandOutput.accepted() && otherTurn.accepted() && coalescer.dirtyCount() == 3,
                          "reasoning, command output, and a same-named item in another turn never coalesce together");

        const frontend::CoalescerMarkResult terminal = coalescer.mark({frontend::CoalescingKey::item("thread-1", "turn-1", "item-1"),
                                                                       "item.updated",
                                                                       frontend::Json{{"status", "completed"}},
                                                                       frontend::FlushUrgency::Immediate});
        result.expectTrue(terminal.immediateFlush, "item completion upgrades a pending flush to immediate");
        const frontend::CoalescerDrainResult terminalDrain = coalescer.drain();
        result.expectTrue(terminalDrain.updates.size() == 4 && terminalDrain.updates.back().type == "item.updated",
                          "terminal flush preserves independent dirty-entity insertion order");

        frontend::EventCoalescer terminalOrdering({8});
        const frontend::CoalescingKey turnKey = frontend::CoalescingKey::turn("thread-order", "turn-order");
        result.expectTrue(terminalOrdering
                              .mark({turnKey,
                                     "turn.updated",
                                     frontend::Json{{"turn", {{"id", "turn-order"}, {"terminal", false}}}},
                                     frontend::FlushUrgency::Deferred})
                              .accepted(),
                          "turn start dirties the turn key before content arrives");
        result.expectTrue(terminalOrdering
                              .mark({frontend::CoalescingKey::itemContent("thread-order", "turn-order", "item-order", "agentText"),
                                     "item.content.updated",
                                     frontend::Json{{"threadId", "thread-order"},
                                                    {"turnId", "turn-order"},
                                                    {"itemId", "item-order"},
                                                    {"channel", "agentText"},
                                                    {"content", "final"}},
                                     frontend::FlushUrgency::Deferred})
                              .accepted(),
                          "final item content remains independently dirty from its turn");
        result.expectTrue(terminalOrdering
                              .mark({turnKey,
                                     "turn.updated",
                                     frontend::Json{{"turn", {{"id", "turn-order"}, {"terminal", true}}}},
                                     frontend::FlushUrgency::Immediate})
                              .accepted(),
                          "turn completion replaces its earlier dirty turn state");
        const frontend::CoalescerDrainResult orderedTerminalDrain = terminalOrdering.drain();
        result.expectTrue(orderedTerminalDrain.updates.size() == 2 && orderedTerminalDrain.updates[0].type == "item.content.updated" &&
                              orderedTerminalDrain.updates[1].type == "turn.updated" &&
                              orderedTerminalDrain.updates[1].data["turn"]["terminal"] == true,
                          "final item content precedes terminal turn state even when turn.started dirtied the key first");

        frontend::EventCoalescer bounded({1});
        result.expectTrue(
            bounded
                .mark(
                    {frontend::CoalescingKey::thread("one"), "thread.updated", frontend::Json::object(), frontend::FlushUrgency::Deferred})
                .accepted(),
            "bounded coalescer accepts its first dirty entity");
        const auto overflow = bounded.mark(
            {frontend::CoalescingKey::thread("two"), "thread.updated", frontend::Json::object(), frontend::FlushUrgency::Deferred});
        result.expectTrue(overflow.status == frontend::CoalescerMarkStatus::SnapshotRequired && bounded.dirtyCount() == 1,
                          "dirty-entity capacity is bounded and degrades to a snapshot instead of growing");
    }

} // namespace

int main() {
    tests::support::TestResult result;
    testCoalescingAndBounds(result);
    return result.processResult();
}
