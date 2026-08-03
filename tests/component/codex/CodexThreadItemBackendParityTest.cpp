/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/backend/Reducer.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/detail/ItemDecoder.h"
#include "support/TestResult.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace detail = ai::openai::codex::detail;
    namespace typed = ai::openai::codex::typed;

    using ai::openai::codex::Json;

    template <typename T>
    bool holdsExactAlternative(const typed::ThreadItem& item) {
        return std::holds_alternative<T>(item);
    }

    struct ItemCase {
        const char* discriminator;
        const char* fixture;
        bool (*holdsExact)(const typed::ThreadItem&);
    };

    constexpr std::array<ItemCase, 10> NewlyTypedItems{{
        {"collabAgentToolCall",
         "cases/unions/threaditem/collabagenttoolcall.json",
         &holdsExactAlternative<typed::CollabAgentToolCallThreadItem>},
        {"contextCompaction", "cases/unions/threaditem/contextcompaction.json", &holdsExactAlternative<typed::ContextCompactionThreadItem>},
        {"enteredReviewMode", "cases/unions/threaditem/enteredreviewmode.json", &holdsExactAlternative<typed::EnteredReviewModeThreadItem>},
        {"exitedReviewMode", "cases/unions/threaditem/exitedreviewmode.json", &holdsExactAlternative<typed::ExitedReviewModeThreadItem>},
        {"hookPrompt", "cases/unions/threaditem/hookprompt.json", &holdsExactAlternative<typed::HookPromptThreadItem>},
        {"imageGeneration", "cases/unions/threaditem/imagegeneration.json", &holdsExactAlternative<typed::ImageGenerationThreadItem>},
        {"imageView", "cases/unions/threaditem/imageview.json", &holdsExactAlternative<typed::ImageViewThreadItem>},
        {"plan", "cases/unions/threaditem/plan.json", &holdsExactAlternative<typed::PlanThreadItem>},
        {"sleep", "cases/unions/threaditem/sleep.json", &holdsExactAlternative<typed::SleepThreadItem>},
        {"subAgentActivity", "cases/unions/threaditem/subagentactivity.json", &holdsExactAlternative<typed::SubAgentActivityThreadItem>},
    }};

    std::optional<Json> readFixture(const std::filesystem::path& relative) {
        std::ifstream input(std::filesystem::path(CODEX_A1_FIXTURE_ROOT) / relative);
        if (!input.good()) {
            return std::nullopt;
        }
        try {
            Json value;
            input >> value;
            return std::optional<Json>{std::move(value)};
        } catch (...) {
            return std::nullopt;
        }
    }

    const typed::ItemMetadata* knownMetadata(const typed::ThreadItem& item) {
        return std::visit(
            [](const auto& value) -> const typed::ItemMetadata* {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, typed::UnknownItem>) {
                    return nullptr;
                } else {
                    return &value.metadata;
                }
            },
            item);
    }

    bool replaceKnownMetadata(typed::ThreadItem& item, typed::ItemMetadata metadata) {
        return std::visit(
            [&metadata](auto& value) {
                using Value = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Value, typed::UnknownItem>) {
                    return false;
                } else {
                    value.metadata = metadata;
                    return true;
                }
            },
            item);
    }

    typed::UnknownItem
    legacyUnknown(const ItemCase& itemCase, const Json& raw, const typed::ThreadId& threadId, const typed::TurnId& turnId) {
        typed::UnknownItem item;
        item.type = itemCase.discriminator;
        item.raw = raw;
        if (const auto id = raw.find("id"); id != raw.end() && id->is_string() && !id->get_ref<const std::string&>().empty()) {
            item.metadata.id = typed::ItemId{id->get<std::string>()};
        }
        item.metadata.threadId = threadId;
        item.metadata.turnId = turnId;
        return item;
    }

    bool translateAndApply(backend::Reducer& reducer,
                           backend::BackendState& state,
                           typed::Event event,
                           backend::ItemLifecycle expectedLifecycle,
                           std::int64_t expectedTimestamp,
                           tests::support::TestResult& result,
                           const std::string& description) {
        const std::vector<backend::BackendEvent> translated = reducer.translate(event);
        const auto* upsert = translated.size() == 1 ? std::get_if<backend::ItemUpserted>(&translated.front()) : nullptr;
        result.expectTrue(upsert && upsert->threadId.value == "thread-parity" && upsert->turnId.value == "turn-parity" &&
                              upsert->lifecycle == expectedLifecycle && upsert->occurredAtMs == expectedTimestamp,
                          description + " translates through the existing canonical ItemUpserted path");
        if (!upsert) {
            return false;
        }
        reducer.apply(state, *upsert);
        return true;
    }

    Json lifecycleEnvelope(const char* method, const Json& item, const char* timestampField, std::int64_t timestamp) {
        const Json params = {{"threadId", "thread-parity"}, {"turnId", "turn-parity"}, {"item", item}, {timestampField, timestamp}};
        return Json{{"method", method}, {"params", params}, {"futureEnvelope", Json{{"retained", true}}}};
    }

    const backend::ItemSnapshot* onlyItem(const backend::Snapshot& snapshot) {
        if (snapshot.threads.size() != 1 || snapshot.threads.front().turns.size() != 1 ||
            snapshot.threads.front().turns.front().items.size() != 1) {
            return nullptr;
        }
        return &snapshot.threads.front().turns.front().items.front();
    }

    backend::ItemSnapshot* onlyItem(backend::Snapshot& snapshot) {
        if (snapshot.threads.size() != 1 || snapshot.threads.front().turns.size() != 1 ||
            snapshot.threads.front().turns.front().items.size() != 1) {
            return nullptr;
        }
        return &snapshot.threads.front().turns.front().items.front();
    }

    bool matchesDedicatedProjection(const ItemCase& itemCase, const Json& fixture, const Json& data) {
        const std::string type = itemCase.discriminator;
        if (type == "collabAgentToolCall") {
            return data.value("tool", "") == fixture.at("tool").get<std::string>() &&
                   data.value("status", "") == fixture.at("status").get<std::string>() &&
                   data.value("senderThreadId", "") == fixture.at("senderThreadId").get<std::string>() &&
                   data.value("receiverCount", std::size_t{0}) == fixture.at("receiverThreadIds").size() &&
                   data.value("agentStateCount", std::size_t{0}) == fixture.at("agentsStates").size() && data.value("hasPrompt", false) &&
                   data.value("promptBytes", std::size_t{0}) == fixture.at("prompt").get_ref<const std::string&>().size();
        }
        if (type == "contextCompaction") {
            return data == Json{{"compacted", true}};
        }
        if (type == "enteredReviewMode" || type == "exitedReviewMode") {
            return data.value("mode", "") == (type == "enteredReviewMode" ? "entered" : "exited") &&
                   data.value("review", "") == fixture.at("review").get<std::string>();
        }
        if (type == "hookPrompt") {
            return data.value("fragmentCount", std::size_t{0}) == fixture.at("fragments").size() &&
                   data.value("firstHookRunId", "") == fixture.at("fragments").front().at("hookRunId").get<std::string>();
        }
        if (type == "imageGeneration") {
            return data.value("status", "") == fixture.at("status").get<std::string>() &&
                   data.value("resultBytes", std::size_t{0}) == fixture.at("result").get_ref<const std::string&>().size() &&
                   data.value("hasRevisedPrompt", false) && data.value("hasSavedPath", false);
        }
        if (type == "imageView") {
            return data.value("path", "") == fixture.at("path").get<std::string>();
        }
        if (type == "plan") {
            return data.value("text", "") == fixture.at("text").get<std::string>() && !data.value("textTruncated", true);
        }
        if (type == "sleep") {
            return data.value("durationMs", std::int64_t{-1}) == fixture.at("durationMs").get<std::int64_t>();
        }
        if (type == "subAgentActivity") {
            return data.value("agentPath", "") == fixture.at("agentPath").get<std::string>() &&
                   data.value("agentThreadId", "") == fixture.at("agentThreadId").get<std::string>() &&
                   data.value("kind", "") == fixture.at("kind").get<std::string>();
        }
        return false;
    }

    void testLifecycleParity(tests::support::TestResult& result) {
        const typed::ThreadId threadId{"thread-parity"};
        const typed::TurnId turnId{"turn-parity"};
        std::size_t exercised = 0;

        for (const ItemCase& itemCase : NewlyTypedItems) {
            const std::string description = std::string(itemCase.discriminator) + " behavior parity";
            const std::optional<Json> fixture = readFixture(itemCase.fixture);
            result.expectTrue(fixture.has_value(), description + " reads its independently generated positive fixture");
            if (!fixture) {
                continue;
            }

            std::string decodingError = "stale";
            const std::optional<typed::ThreadItem> decoded = detail::decodeItem(*fixture, threadId, turnId, decodingError);
            result.expectTrue(decoded && decodingError.empty() && itemCase.holdsExact(*decoded),
                              description + " selects its exact new public alternative");
            if (!decoded || !itemCase.holdsExact(*decoded)) {
                continue;
            }

            const typed::ItemMetadata* metadata = knownMetadata(*decoded);
            result.expectTrue(metadata && metadata->id.value == fixture->at("id").get<std::string>() && metadata->threadId == threadId &&
                                  metadata->turnId == turnId && metadata->raw == *fixture,
                              description + " retains exact raw JSON and all common metadata");

            typed::UnknownItem legacy = legacyUnknown(itemCase, *fixture, threadId, turnId);
            backend::BackendState typedState;
            backend::BackendState legacyState;
            backend::Reducer reducer;
            constexpr std::int64_t StartedAt = 101;
            constexpr std::int64_t CompletedAt = 202;
            const Json startedEnvelope = lifecycleEnvelope("item/started", *fixture, "startedAtMs", StartedAt);
            const Json completedEnvelope = lifecycleEnvelope("item/completed", *fixture, "completedAtMs", CompletedAt);

            const bool typedStarted = translateAndApply(reducer,
                                                        typedState,
                                                        typed::Event{typed::ItemStarted{*decoded, StartedAt, startedEnvelope}},
                                                        backend::ItemLifecycle::Started,
                                                        StartedAt,
                                                        result,
                                                        description + " typed start");
            const bool legacyStarted =
                translateAndApply(reducer,
                                  legacyState,
                                  typed::Event{typed::ItemStarted{typed::ThreadItem{legacy}, StartedAt, startedEnvelope}},
                                  backend::ItemLifecycle::Started,
                                  StartedAt,
                                  result,
                                  description + " legacy start");
            const bool typedCompleted = translateAndApply(reducer,
                                                          typedState,
                                                          typed::Event{typed::ItemCompleted{*decoded, CompletedAt, completedEnvelope}},
                                                          backend::ItemLifecycle::Completed,
                                                          CompletedAt,
                                                          result,
                                                          description + " typed completion");
            const bool legacyCompleted =
                translateAndApply(reducer,
                                  legacyState,
                                  typed::Event{typed::ItemCompleted{typed::ThreadItem{legacy}, CompletedAt, completedEnvelope}},
                                  backend::ItemLifecycle::Completed,
                                  CompletedAt,
                                  result,
                                  description + " legacy completion");
            if (!typedStarted || !legacyStarted || !typedCompleted || !legacyCompleted) {
                continue;
            }

            const backend::ItemState* typedItemState =
                backend::findItem(typedState, threadId, turnId, typed::ItemId{fixture->at("id").get<std::string>()});
            const typed::ItemMetadata* retainedMetadata = typedItemState ? knownMetadata(typedItemState->item) : nullptr;
            result.expectTrue(typedItemState && retainedMetadata && retainedMetadata->raw == *fixture &&
                                  typedItemState->lifecycle == backend::ItemLifecycle::Completed &&
                                  typedItemState->startedAtMs == StartedAt && typedItemState->completedAtMs == CompletedAt &&
                                  typedState.recentExtensions.empty(),
                              description + " remains canonical and observable without a duplicate extension");

            backend::Snapshot typedSnapshot = backend::makeSnapshot(typedState);
            const backend::Snapshot legacySnapshot = backend::makeSnapshot(legacyState);
            const backend::ItemSnapshot* itemSnapshot = onlyItem(typedSnapshot);
            backend::Snapshot metadataNormalizedSnapshot = typedSnapshot;
            backend::ItemSnapshot* normalizedItem = onlyItem(metadataNormalizedSnapshot);
            const backend::ItemSnapshot* legacyItem = onlyItem(legacySnapshot);
            if (normalizedItem && legacyItem) {
                normalizedItem->data = legacyItem->data;
            }
            result.expectTrue(normalizedItem && legacyItem && metadataNormalizedSnapshot == legacySnapshot,
                              description + " preserves lifecycle/location parity while adding only its dedicated safe projection");
            result.expectTrue(
                itemSnapshot && itemSnapshot->id == fixture->at("id").get<std::string>() && itemSnapshot->type == itemCase.discriminator &&
                    itemSnapshot->status == "completed" && itemSnapshot->startedAtMs == StartedAt &&
                    itemSnapshot->completedAtMs == CompletedAt && matchesDedicatedProjection(itemCase, *fixture, itemSnapshot->data) &&
                    !itemSnapshot->data.contains("raw") && !itemSnapshot->data.contains("accessToken") && itemSnapshot->agentText.empty() &&
                    itemSnapshot->reasoningText.empty() && itemSnapshot->reasoningSummary.empty() && itemSnapshot->commandOutput.empty() &&
                    itemSnapshot->extensions.empty(),
                description + " exposes a useful bounded typed backend projection without raw provider JSON");
            ++exercised;
        }

        result.expectTrue(exercised == NewlyTypedItems.size(), "all ten newly typed ThreadItem alternatives prove lifecycle parity");
    }

    void testExtensionFallbackParity(tests::support::TestResult& result) {
        const typed::ThreadId threadId{"thread-parity"};
        const typed::TurnId turnId{"turn-parity"};
        std::size_t redactionCases = 0;
        std::size_t boundedCases = 0;

        for (const ItemCase& itemCase : NewlyTypedItems) {
            const std::string description = std::string(itemCase.discriminator) + " extension fallback";
            const std::optional<Json> fixture = readFixture(itemCase.fixture);
            if (!fixture) {
                result.expectTrue(false, description + " reads its positive fixture");
                continue;
            }

            std::string decodingError;
            std::optional<typed::ThreadItem> decoded = detail::decodeItem(*fixture, threadId, turnId, decodingError);
            if (!decoded || !itemCase.holdsExact(*decoded)) {
                result.expectTrue(false, description + " starts from the exact typed alternative");
                continue;
            }

            const std::string secret = std::string("must-not-leak-") + itemCase.discriminator;
            const Json sensitiveRaw = {
                {"type", itemCase.discriminator}, {"safe", "visible"}, {"accessToken", secret}, {"nested", {{"secret", secret}}}};
            typed::ItemMetadata missingIdMetadata;
            missingIdMetadata.threadId = threadId;
            missingIdMetadata.turnId = turnId;
            missingIdMetadata.raw = sensitiveRaw;
            result.expectTrue(replaceKnownMetadata(*decoded, missingIdMetadata),
                              description + " prepares an intentionally ID-less typed aggregate");

            backend::BackendState sensitiveState;
            backend::BackendState legacySensitiveState;
            backend::Reducer reducer;
            reducer.apply(sensitiveState,
                          backend::ItemUpserted{threadId, turnId, *decoded, backend::ItemLifecycle::Started, std::int64_t{303}});
            typed::UnknownItem sensitiveLegacy = legacyUnknown(itemCase, sensitiveRaw, threadId, turnId);
            reducer.apply(legacySensitiveState,
                          backend::ItemUpserted{
                              threadId, turnId, typed::ThreadItem{sensitiveLegacy}, backend::ItemLifecycle::Started, std::int64_t{303}});
            const backend::ExtensionRecord* retained =
                sensitiveState.recentExtensions.size() == 1 ? &sensitiveState.recentExtensions.front() : nullptr;
            const backend::Snapshot sensitiveSnapshot = backend::makeSnapshot(sensitiveState);
            const backend::Snapshot legacySensitiveSnapshot = backend::makeSnapshot(legacySensitiveState);
            const backend::ExtensionSnapshot* safe =
                sensitiveSnapshot.recentExtensions.size() == 1 ? &sensitiveSnapshot.recentExtensions.front() : nullptr;
            result.expectTrue(sensitiveSnapshot == legacySensitiveSnapshot && sensitiveState.threads.empty() && retained &&
                                  retained->method == "codex/item-without-id" && retained->payload == sensitiveRaw &&
                                  retained->decodingError == "typed item has no stable id" && safe &&
                                  safe->method == "codex/item-without-id" && safe->payload.at("safe") == "visible" &&
                                  safe->sensitiveFieldsRedacted && safe->payload.dump().find(secret) == std::string::npos,
                              description + " matches the legacy bounded, recursively redacted item-without-id contract");
            ++redactionCases;

            const std::string oversizedSecret(backend::MaxSnapshotExtensionPayloadBytes * 3, 's');
            Json oversizedRaw = {{"type", itemCase.discriminator}, {"authorization", oversizedSecret}, {"safe", "would exceed the bound"}};
            missingIdMetadata.raw = oversizedRaw;
            replaceKnownMetadata(*decoded, missingIdMetadata);
            backend::BackendState oversizedState;
            backend::BackendState legacyOversizedState;
            reducer.apply(oversizedState,
                          backend::ItemUpserted{threadId, turnId, *decoded, backend::ItemLifecycle::Completed, std::int64_t{404}});
            typed::UnknownItem oversizedLegacy = legacyUnknown(itemCase, oversizedRaw, threadId, turnId);
            reducer.apply(legacyOversizedState,
                          backend::ItemUpserted{
                              threadId, turnId, typed::ThreadItem{oversizedLegacy}, backend::ItemLifecycle::Completed, std::int64_t{404}});
            const backend::ExtensionRecord* oversizedRetained =
                oversizedState.recentExtensions.size() == 1 ? &oversizedState.recentExtensions.front() : nullptr;
            const backend::Snapshot oversizedSnapshot = backend::makeSnapshot(oversizedState);
            const backend::Snapshot legacyOversizedSnapshot = backend::makeSnapshot(legacyOversizedState);
            const backend::ExtensionSnapshot* bounded =
                oversizedSnapshot.recentExtensions.size() == 1 ? &oversizedSnapshot.recentExtensions.front() : nullptr;
            result.expectTrue(
                oversizedSnapshot == legacyOversizedSnapshot && oversizedState.threads.empty() && oversizedRetained &&
                    oversizedRetained->originalPayloadBytes.has_value() && oversizedRetained->payload.value("truncated", false) &&
                    oversizedRetained->payload.value("omitted", false) && bounded && bounded->payloadTruncated &&
                    bounded->originalPayloadBytes.has_value() && bounded->payload.dump().find(oversizedSecret) == std::string::npos,
                description + " matches legacy original-size accounting and bounded omission for oversized raw JSON");
            ++boundedCases;
        }

        result.expectTrue(redactionCases == NewlyTypedItems.size() && boundedCases == NewlyTypedItems.size(),
                          "all ten newly typed ThreadItem alternatives retain extension redaction and size bounds");
    }
} // namespace

int main() {
    tests::support::TestResult result;

    testLifecycleParity(result);
    testExtensionFallbackParity(result);

    return result.processResult();
}
