/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/backend/Reducer.h"

#include "ai/openai/codex/backend/detail/PreserveUnmodeledTypedEvent.h"
#include "ai/openai/codex/backend/internal/RetentionCapacityInstrumentation.h"
#include "ai/openai/codex/detail/ConversationCodec.h"
#include "ai/openai/codex/detail/DecodeDiagnostic.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "log/LogScopeOwner.h"
#include "log/Logger.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>

namespace ai::openai::codex::backend {

    namespace detail {
        namespace {
            thread_local RetentionCapacityInstrumentation retentionInstrumentation;
        }

        void resetRetentionCapacityInstrumentation() noexcept {
            retentionInstrumentation = {};
        }

        RetentionCapacityInstrumentation retentionCapacityInstrumentation() noexcept {
            return retentionInstrumentation;
        }

        void recordRetentionCapacitySlowPath() noexcept {
            if (retentionInstrumentation.slowPathEntries != std::numeric_limits<std::size_t>::max()) {
                ++retentionInstrumentation.slowPathEntries;
            }
        }

        void recordPendingReferenceBuild() noexcept {
            if (retentionInstrumentation.pendingReferenceBuilds != std::numeric_limits<std::size_t>::max()) {
                ++retentionInstrumentation.pendingReferenceBuilds;
            }
        }
    } // namespace detail

    namespace {
        template <typename... Visitors>
        struct Overloaded : Visitors... {
            using Visitors::operator()...;
        };

        template <typename... Visitors>
        Overloaded(Visitors...) -> Overloaded<Visitors...>;

        template <typename Unsigned>
        std::uint64_t saturatingUint64(Unsigned value) noexcept {
            static_assert(std::is_unsigned_v<Unsigned>);
            if constexpr (std::numeric_limits<Unsigned>::digits > std::numeric_limits<std::uint64_t>::digits) {
                if (value > static_cast<Unsigned>(std::numeric_limits<std::uint64_t>::max())) {
                    return std::numeric_limits<std::uint64_t>::max();
                }
            }
            return static_cast<std::uint64_t>(value);
        }

        void saturatingAdd(std::uint64_t& value, std::uint64_t amount = 1) noexcept {
            value = amount > std::numeric_limits<std::uint64_t>::max() - value ? std::numeric_limits<std::uint64_t>::max() : value + amount;
        }

        void saturatingAddSize(std::size_t& value, std::size_t amount) noexcept {
            value = amount > std::numeric_limits<std::size_t>::max() - value ? std::numeric_limits<std::size_t>::max() : value + amount;
        }

        void guardedSubtractSize(std::size_t& value, std::size_t amount) noexcept {
            value = amount > value ? 0 : value - amount;
        }

        void replaceAccumulatedContent(CapacityState& capacity, std::size_t before, std::size_t after) noexcept {
            if (after >= before) {
                saturatingAddSize(capacity.accumulatedContentBytes, after - before);
            } else {
                guardedSubtractSize(capacity.accumulatedContentBytes, before - after);
            }
        }

        bool sameError(const std::optional<Error>& left, const std::optional<Error>& right) noexcept {
            return left.has_value() == right.has_value() &&
                   (!left || (left->category == right->category && left->code == right->code && left->message == right->message));
        }

        bool sameInitialization(const std::optional<typed::InitializeResponse>& left,
                                const std::optional<typed::InitializeResponse>& right) noexcept {
            if (left.has_value() != right.has_value()) {
                return false;
            }
            if (!left) {
                return true;
            }
            try {
                return left->codexHome.value == right->codexHome.value && left->platformFamily == right->platformFamily &&
                       left->platformOs == right->platformOs && left->userAgent == right->userAgent && left->raw == right->raw;
            } catch (...) {
                return false;
            }
        }

        bool sameProvider(const ProviderState& left, const ProviderState& right) noexcept {
            return left.lifecycle == right.lifecycle && left.generation == right.generation &&
                   left.desiredRunning == right.desiredRunning && sameError(left.lastError, right.lastError) &&
                   left.recovery == right.recovery && sameInitialization(left.initialization, right.initialization);
        }

        SourceStamp currentStamp(const BackendState& state) noexcept {
            return {state.provider.generation, Freshness::Current};
        }

        using TurnKey = std::tuple<std::string, std::string>;
        using ItemKey = std::tuple<std::string, std::string, std::string>;

        struct RetentionInsertions {
            std::vector<std::string> threads;
            std::vector<TurnKey> turns;
            std::vector<ItemKey> items;
        };

        using ServerNotificationTarget = ::ai::openai::codex::detail::ServerNotificationTarget;

        template <typename Notification>
        std::vector<BackendEvent> preserveTypedNotification(const Notification& value, ServerNotificationTarget target) {
            const ::ai::openai::codex::detail::ProtocolSurfaceEntry& registryEntry = ::ai::openai::codex::detail::entryFor(target);
            const std::optional<typed::DecodeDiagnostic> diagnostic =
                value.diagnostics.empty() ? std::nullopt : std::optional<typed::DecodeDiagnostic>{value.diagnostics.front()};
            return {detail::preserveUnmodeledTypedEvent(
                {std::string(registryEntry.key.name), value.raw.at("params"), std::nullopt, diagnostic})};
        }

        logger::BoundaryLogger lifecycleLog() {
            static const logger::LogScopeOwner scope(
                logger::LogOrigin::Framework, logger::LogBoundary::Connection, "ai.openai.codex.backend");
            return scope.logger(logger::Logger::semanticSink());
        }

        typed::Thread placeholderThread(const typed::ThreadId& id) {
            typed::Thread thread;
            thread.id = id;
            thread.raw = Json::object({{"backendPlaceholder", true}});
            return thread;
        }

        typed::Turn placeholderTurn(const typed::ThreadId& threadId, const typed::TurnId& turnId) {
            typed::Turn turn;
            turn.id = turnId;
            turn.threadId = threadId;
            turn.status = typed::TurnStatus::inProgress();
            turn.itemsView = typed::TurnItemsView::notLoaded();
            turn.raw = Json::object({{"backendPlaceholder", true}});
            return turn;
        }

        ThreadState& ensureThread(BackendState& state, const typed::ThreadId& id, RetentionInsertions* insertions = nullptr) {
            ThreadState initial;
            initial.thread = placeholderThread(id);
            initial.stamp = currentStamp(state);
            auto [iterator, inserted] = state.threads.try_emplace(id.value, std::move(initial));
            if (inserted) {
                state.threadOrder.push_back(id);
                saturatingAddSize(state.capacity.retainedThreads, 1);
                if (insertions != nullptr) {
                    insertions->threads.push_back(id.value);
                }
            }
            return iterator->second;
        }

        TurnState& ensureTurn(BackendState& state,
                              const typed::ThreadId& threadId,
                              const typed::TurnId& turnId,
                              RetentionInsertions* insertions = nullptr) {
            ThreadState& thread = ensureThread(state, threadId, insertions);
            TurnState initial;
            initial.turn = placeholderTurn(threadId, turnId);
            initial.stamp = currentStamp(state);
            auto [iterator, inserted] = thread.turns.try_emplace(turnId.value, std::move(initial));
            if (inserted) {
                thread.turnOrder.push_back(turnId);
                saturatingAddSize(state.capacity.retainedTurns, 1);
                if (insertions != nullptr) {
                    insertions->turns.emplace_back(threadId.value, turnId.value);
                }
            }
            return iterator->second;
        }

        void assignBounded(std::string& target, const std::string& value, std::size_t limit, std::uint64_t& dropped) {
            if (limit == 0) {
                saturatingAdd(dropped, saturatingUint64(value.size()));
                target.clear();
            } else if (value.size() > limit) {
                saturatingAdd(dropped, saturatingUint64(value.size() - limit));
                target.assign(value.end() - static_cast<std::ptrdiff_t>(limit), value.end());
            } else {
                target = value;
            }
        }

        void appendBounded(std::string& target, const std::string& value, std::size_t limit, std::uint64_t& dropped) {
            if (value.empty()) {
                return;
            }
            if (limit == 0) {
                const std::uint64_t targetBytes = saturatingUint64(target.size());
                const std::uint64_t valueBytes = saturatingUint64(value.size());
                saturatingAdd(dropped, targetBytes);
                saturatingAdd(dropped, valueBytes);
                target.clear();
                return;
            }
            if (value.size() >= limit) {
                saturatingAdd(dropped, saturatingUint64(target.size()));
                saturatingAdd(dropped, saturatingUint64(value.size() - limit));
                target.assign(value.end() - static_cast<std::ptrdiff_t>(limit), value.end());
                return;
            }
            if (target.size() > limit - value.size()) {
                const std::size_t remove = target.size() - (limit - value.size());
                saturatingAdd(dropped, saturatingUint64(remove));
                target.erase(0, remove);
            }
            target += value;
        }

        std::optional<std::vector<typed::FileUpdateChange>> decodeFileUpdateChanges(const Json& changes) {
            if (!changes.is_array()) {
                return std::nullopt;
            }

            std::vector<typed::FileUpdateChange> decoded;
            decoded.reserve(changes.size());
            for (const Json& change : changes) {
                if (!change.is_object()) {
                    return std::nullopt;
                }
                const auto diff = change.find("diff");
                const auto kind = change.find("kind");
                const auto path = change.find("path");
                if (diff == change.end() || !diff->is_string() || kind == change.end() || path == change.end() || !path->is_string()) {
                    return std::nullopt;
                }
                ai::openai::codex::detail::ConversationDecodeResult<typed::PatchChangeKind> decodedKind =
                    ai::openai::codex::detail::decodePatchChangeKind(*kind);
                if (!decodedKind.value) {
                    return std::nullopt;
                }
                decoded.push_back({diff->get<std::string>(), std::move(*decodedKind.value), path->get<std::string>()});
            }
            return decoded;
        }

        void initializeVisibleContent(ItemState& state, bool authoritative, std::size_t limit) {
            std::visit(Overloaded{[&state, authoritative, limit](const typed::AgentMessageThreadItem& item) {
                                      if ((!item.text.empty() && authoritative) || state.agentText.empty()) {
                                          assignBounded(state.agentText, item.text, limit, state.droppedContentBytes);
                                      }
                                  },
                                  [&state, authoritative, limit](const typed::ReasoningThreadItem& item) {
                                      const std::vector<std::string>& content = item.contentOrDefault();
                                      const bool hasContent = std::any_of(content.begin(), content.end(), [](const std::string& value) {
                                          return !value.empty();
                                      });
                                      if ((authoritative && hasContent) || state.reasoningText.empty()) {
                                          state.reasoningText.clear();
                                          for (const std::string& value : content) {
                                              appendBounded(state.reasoningText, value, limit, state.droppedContentBytes);
                                          }
                                      }
                                      const std::vector<std::string>& summary = item.summaryOrDefault();
                                      const bool hasSummary = std::any_of(summary.begin(), summary.end(), [](const std::string& value) {
                                          return !value.empty();
                                      });
                                      if ((authoritative && hasSummary) || state.reasoningSummary.empty()) {
                                          state.reasoningSummary.clear();
                                          for (const std::string& value : summary) {
                                              appendBounded(state.reasoningSummary, value, limit, state.droppedContentBytes);
                                          }
                                      }
                                  },
                                  [&state, authoritative, limit](const typed::CommandExecutionThreadItem& item) {
                                      if (item.aggregatedOutput &&
                                          ((!item.aggregatedOutput->empty() && authoritative) || state.commandOutput.empty())) {
                                          assignBounded(state.commandOutput, *item.aggregatedOutput, limit, state.droppedContentBytes);
                                      }
                                  },
                                  [](const auto&) {
                                  }},
                       state.item);
        }

        std::size_t itemContentBytes(const ItemState& item) noexcept {
            std::size_t bytes = 0;
            saturatingAddSize(bytes, item.agentText.size());
            saturatingAddSize(bytes, item.reasoningText.size());
            saturatingAddSize(bytes, item.reasoningSummary.size());
            saturatingAddSize(bytes, item.commandOutput.size());
            return bytes;
        }

        std::optional<std::pair<typed::ThreadId, typed::TurnId>> itemLocation(const typed::ThreadItem& item) {
            return std::visit(
                [](const auto& value) -> std::optional<std::pair<typed::ThreadId, typed::TurnId>> {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, typed::UnknownItem>) {
                        if (value.metadata.threadId && !value.metadata.threadId->value.empty() && value.metadata.turnId &&
                            !value.metadata.turnId->value.empty()) {
                            return std::pair{*value.metadata.threadId, *value.metadata.turnId};
                        }
                    } else {
                        if (value.metadata.threadId && !value.metadata.threadId->value.empty() && value.metadata.turnId &&
                            !value.metadata.turnId->value.empty()) {
                            return std::pair{*value.metadata.threadId, *value.metadata.turnId};
                        }
                    }
                    return std::nullopt;
                },
                item);
        }

        bool upsertItem(BackendState& state,
                        const typed::ThreadId& threadId,
                        const typed::TurnId& turnId,
                        const typed::ThreadItem& item,
                        ItemLifecycle lifecycle,
                        std::optional<std::int64_t> occurredAtMs,
                        std::size_t contentLimit,
                        RetentionInsertions* insertions = nullptr) {
            const std::optional<typed::ItemId> id = itemId(item);
            if (!id) {
                return false;
            }

            TurnState& turn = ensureTurn(state, threadId, turnId, insertions);
            auto iterator = turn.items.find(id->value);
            if (iterator == turn.items.end()) {
                ItemState itemState;
                itemState.item = item;
                itemState.lifecycle = lifecycle;
                itemState.stamp = currentStamp(state);
                itemState.connectionInvalidated = false;
                if (lifecycle == ItemLifecycle::Started) {
                    itemState.startedAtMs = occurredAtMs;
                } else if (lifecycle == ItemLifecycle::Completed) {
                    itemState.completedAtMs = occurredAtMs;
                }
                initializeVisibleContent(itemState, true, contentLimit);
                const std::size_t contentBytes = itemContentBytes(itemState);
                turn.items.emplace(id->value, std::move(itemState));
                turn.itemOrder.push_back(*id);
                saturatingAddSize(state.capacity.retainedItems, 1);
                saturatingAddSize(state.capacity.accumulatedContentBytes, contentBytes);
                if (insertions != nullptr) {
                    insertions->items.emplace_back(threadId.value, turnId.value, id->value);
                }
            } else {
                ItemState& itemState = iterator->second;
                const std::size_t previousContentBytes = itemContentBytes(itemState);
                itemState.item = item;
                itemState.stamp = currentStamp(state);
                itemState.connectionInvalidated = false;
                if (lifecycle != ItemLifecycle::Unknown || itemState.lifecycle == ItemLifecycle::Unknown) {
                    itemState.lifecycle = lifecycle;
                }
                if (lifecycle == ItemLifecycle::Started && occurredAtMs) {
                    itemState.startedAtMs = occurredAtMs;
                } else if (lifecycle == ItemLifecycle::Completed && occurredAtMs) {
                    itemState.completedAtMs = occurredAtMs;
                }
                initializeVisibleContent(itemState, lifecycle == ItemLifecycle::Completed, contentLimit);
                replaceAccumulatedContent(state.capacity, previousContentBytes, itemContentBytes(itemState));
            }
            return true;
        }

        bool
        upsertTurn(BackendState& state, const typed::Turn& value, std::size_t contentLimit, RetentionInsertions* insertions = nullptr) {
            typed::Turn normalized = value;
            normalized.items.clear();
            if (normalized.raw.is_object()) {
                normalized.raw.erase("items");
            }
            ThreadState& thread = ensureThread(state, value.threadId, insertions);
            auto iterator = thread.turns.find(value.id.value);
            if (iterator == thread.turns.end()) {
                TurnState turn;
                turn.turn = std::move(normalized);
                turn.stamp = currentStamp(state);
                turn.connectionInvalidated = false;
                turn.active = !isTerminal(value.status);
                turn.terminal = isTerminal(value.status);
                iterator = thread.turns.emplace(value.id.value, std::move(turn)).first;
                thread.turnOrder.push_back(value.id);
                saturatingAddSize(state.capacity.retainedTurns, 1);
                if (insertions != nullptr) {
                    insertions->turns.emplace_back(value.threadId.value, value.id.value);
                }
            } else {
                iterator->second.turn = std::move(normalized);
                iterator->second.stamp = currentStamp(state);
                iterator->second.connectionInvalidated = false;
                iterator->second.active = !isTerminal(value.status);
                iterator->second.terminal = isTerminal(value.status);
                if (value.error.hasValue()) {
                    iterator->second.failure = value.error->raw;
                }
            }

            for (const typed::ThreadItem& item : value.items) {
                upsertItem(state,
                           value.threadId,
                           value.id,
                           item,
                           iterator->second.terminal ? ItemLifecycle::Completed : ItemLifecycle::Unknown,
                           std::nullopt,
                           contentLimit,
                           insertions);
            }
            return true;
        }

        bool upsertThread(BackendState& state,
                          const typed::Thread& value,
                          EntityLoad load,
                          std::size_t contentLimit,
                          RetentionInsertions* insertions = nullptr) {
            typed::Thread normalized = value;
            normalized.turns.clear();
            if (normalized.raw.is_object()) {
                normalized.raw.erase("turns");
            }
            auto iterator = state.threads.find(value.id.value);
            if (iterator == state.threads.end()) {
                ThreadState thread;
                thread.thread = std::move(normalized);
                thread.fullyLoaded = load == EntityLoad::Full;
                thread.stamp = currentStamp(state);
                iterator = state.threads.emplace(value.id.value, std::move(thread)).first;
                state.threadOrder.push_back(value.id);
                saturatingAddSize(state.capacity.retainedThreads, 1);
                if (insertions != nullptr) {
                    insertions->threads.push_back(value.id.value);
                }
            } else {
                const bool currentGeneration = iterator->second.stamp.generation == state.provider.generation &&
                                               iterator->second.stamp.freshness == Freshness::Current;
                iterator->second.thread = std::move(normalized);
                iterator->second.fullyLoaded =
                    currentGeneration ? iterator->second.fullyLoaded || load == EntityLoad::Full : load == EntityLoad::Full;
                iterator->second.stamp = currentStamp(state);
            }

            for (const typed::Turn& turn : value.turns) {
                upsertTurn(state, turn, contentLimit, insertions);
            }
            return true;
        }

        struct PendingReferences {
            std::set<std::string> threads;
            std::set<TurnKey> turns;
            std::set<ItemKey> items;
        };

        PendingReferences pendingReferences(const BackendState& state) {
            PendingReferences references;
            for (const auto& [id, pending] : state.pendingRequests) {
                (void) id;
                std::visit(
                    [&references](const auto& request) {
                        using Request = std::remove_cvref_t<decltype(request)>;
                        if constexpr (std::is_same_v<Request, typed::ApplyPatchApprovalRequest> ||
                                      std::is_same_v<Request, typed::ExecCommandApprovalRequest>) {
                            references.threads.insert(request.params.conversationId.value);
                        } else if constexpr (std::is_same_v<Request, typed::PermissionsApprovalRequest>) {
                            references.threads.insert(request.params.threadId.value);
                            references.turns.emplace(request.params.threadId.value, request.params.turnId.value);
                            references.items.emplace(
                                request.params.threadId.value, request.params.turnId.value, request.params.itemId.value);
                        } else if constexpr (std::is_same_v<Request, typed::DynamicToolCallRequest>) {
                            references.threads.insert(request.params.threadId.value);
                            references.turns.emplace(request.params.threadId.value, request.params.turnId.value);
                        } else if constexpr (std::is_same_v<Request, typed::McpServerElicitationRequest>) {
                            references.threads.insert(request.params.threadId.value);
                            if (request.params.turnId.hasValue()) {
                                references.turns.emplace(request.params.threadId.value, request.params.turnId.value->value);
                            }
                        } else if constexpr (requires { request.threadId.value; }) {
                            references.threads.insert(request.threadId.value);
                            if constexpr (requires { request.turnId.value; }) {
                                references.turns.emplace(request.threadId.value, request.turnId.value);
                                if constexpr (requires { request.itemId.value; }) {
                                    references.items.emplace(request.threadId.value, request.turnId.value, request.itemId.value);
                                }
                            }
                        }
                    },
                    pending.request);
            }
            return references;
        }

        bool turnHasActiveItem(const TurnState& turn) noexcept {
            return std::any_of(turn.items.begin(), turn.items.end(), [](const auto& entry) {
                return entry.second.lifecycle == ItemLifecycle::Started || entry.second.lifecycle == ItemLifecycle::Unknown;
            });
        }

        bool threadHasActiveTurn(const ThreadState& thread) noexcept {
            return std::any_of(thread.turns.begin(), thread.turns.end(), [](const auto& entry) {
                return entry.second.active || !entry.second.terminal || turnHasActiveItem(entry.second);
            });
        }

        bool threadIsReferenced(const ThreadState& thread, const std::string& threadId, const PendingReferences& referenced) {
            if (referenced.threads.contains(threadId)) {
                return true;
            }
            for (const auto& [turnId, turn] : thread.turns) {
                if (referenced.turns.contains(TurnKey{threadId, turnId})) {
                    return true;
                }
                for (const auto& [itemIdValue, item] : turn.items) {
                    (void) item;
                    if (referenced.items.contains(ItemKey{threadId, turnId, itemIdValue})) {
                        return true;
                    }
                }
            }
            return false;
        }

        bool turnIsReferenced(const TurnState& turn,
                              const std::string& threadId,
                              const std::string& turnId,
                              const PendingReferences& referenced) {
            if (referenced.turns.contains(TurnKey{threadId, turnId})) {
                return true;
            }
            return std::any_of(turn.items.begin(), turn.items.end(), [&](const auto& entry) {
                return referenced.items.contains(ItemKey{threadId, turnId, entry.first});
            });
        }

        void accountTurnRemoval(CapacityState& capacity, const TurnState& turn, bool eviction) noexcept {
            if (eviction) {
                saturatingAdd(capacity.evictedTurns);
                saturatingAdd(capacity.evictedItems, saturatingUint64(turn.items.size()));
            } else {
                saturatingAdd(capacity.snapshotOmissions);
            }
        }

        void accountThreadRemoval(CapacityState& capacity, const ThreadState& thread, bool eviction) noexcept {
            if (!eviction) {
                saturatingAdd(capacity.snapshotOmissions);
                return;
            }
            saturatingAdd(capacity.evictedThreads);
            saturatingAdd(capacity.evictedTurns, saturatingUint64(thread.turns.size()));
            std::size_t itemCount = 0;
            for (const auto& [turnId, turn] : thread.turns) {
                (void) turnId;
                saturatingAddSize(itemCount, turn.items.size());
            }
            saturatingAdd(capacity.evictedItems, saturatingUint64(itemCount));
        }

        void eraseItem(BackendState& state, TurnState& turn, const std::string& id) {
            const auto item = turn.items.find(id);
            if (item == turn.items.end()) {
                return;
            }
            guardedSubtractSize(state.capacity.retainedItems, 1);
            guardedSubtractSize(state.capacity.accumulatedContentBytes, itemContentBytes(item->second));
            turn.items.erase(item);
            std::erase_if(turn.itemOrder, [&id](const typed::ItemId& value) {
                return value.value == id;
            });
        }

        void eraseTurn(BackendState& state, ThreadState& thread, const std::string& id) {
            const auto turn = thread.turns.find(id);
            if (turn == thread.turns.end()) {
                return;
            }
            guardedSubtractSize(state.capacity.retainedTurns, 1);
            guardedSubtractSize(state.capacity.retainedItems, turn->second.items.size());
            for (const auto& [itemIdValue, item] : turn->second.items) {
                (void) itemIdValue;
                guardedSubtractSize(state.capacity.accumulatedContentBytes, itemContentBytes(item));
            }
            thread.turns.erase(turn);
            std::erase_if(thread.turnOrder, [&id](const typed::TurnId& value) {
                return value.value == id;
            });
        }

        void eraseThread(BackendState& state, const std::string& id) {
            const auto thread = state.threads.find(id);
            if (thread == state.threads.end()) {
                return;
            }
            guardedSubtractSize(state.capacity.retainedThreads, 1);
            guardedSubtractSize(state.capacity.retainedTurns, thread->second.turns.size());
            for (const auto& [turnId, turn] : thread->second.turns) {
                (void) turnId;
                guardedSubtractSize(state.capacity.retainedItems, turn.items.size());
                for (const auto& [itemIdValue, item] : turn.items) {
                    (void) itemIdValue;
                    guardedSubtractSize(state.capacity.accumulatedContentBytes, itemContentBytes(item));
                }
            }
            state.threads.erase(thread);
            std::erase_if(state.threadOrder, [&id](const typed::ThreadId& value) {
                return value.value == id;
            });
        }

        void enforceRetentionCapacity(BackendState& state, const RetentionInsertions& insertions) {
            const BackendCapacityOptions& limits = state.capacity.limits;
            if (state.capacity.retainedThreads <= limits.maxRetainedThreads && state.capacity.retainedTurns <= limits.maxRetainedTurns &&
                state.capacity.retainedItems <= limits.maxRetainedItems &&
                state.capacity.accumulatedContentBytes <= limits.maxAccumulatedContentBytes) {
                return;
            }
            detail::recordRetentionCapacitySlowPath();
            const bool structuralCapacityExceeded = state.capacity.retainedThreads > limits.maxRetainedThreads ||
                                                    state.capacity.retainedTurns > limits.maxRetainedTurns ||
                                                    state.capacity.retainedItems > limits.maxRetainedItems;
            if (structuralCapacityExceeded) {
                detail::recordPendingReferenceBuild();
            }
            const PendingReferences referenced = structuralCapacityExceeded ? pendingReferences(state) : PendingReferences{};
            const std::set<std::string> insertedThreads(insertions.threads.begin(), insertions.threads.end());
            const std::set<TurnKey> insertedTurns(insertions.turns.begin(), insertions.turns.end());
            const std::set<ItemKey> insertedItems(insertions.items.begin(), insertions.items.end());

            while (state.capacity.retainedThreads > limits.maxRetainedThreads) {
                auto candidate = state.threadOrder.end();
                for (auto iterator = state.threadOrder.begin(); iterator != state.threadOrder.end(); ++iterator) {
                    const auto thread = state.threads.find(iterator->value);
                    if (!insertedThreads.contains(iterator->value) && thread != state.threads.end() &&
                        !threadHasActiveTurn(thread->second) && !threadIsReferenced(thread->second, iterator->value, referenced)) {
                        candidate = iterator;
                        break;
                    }
                }
                if (candidate == state.threadOrder.end()) {
                    const auto inserted =
                        std::find_if(insertions.threads.rbegin(), insertions.threads.rend(), [&](const std::string& value) {
                            return state.threads.contains(value);
                        });
                    if (inserted != insertions.threads.rend()) {
                        const std::string id = *inserted;
                        const auto thread = state.threads.find(id);
                        accountThreadRemoval(state.capacity, thread->second, false);
                        eraseThread(state, id);
                        continue;
                    }
                    saturatingAdd(state.capacity.snapshotOmissions);
                    break;
                }
                const std::string id = candidate->value;
                const auto thread = state.threads.find(id);
                accountThreadRemoval(state.capacity, thread->second, true);
                eraseThread(state, id);
            }

            while (state.capacity.retainedTurns > limits.maxRetainedTurns) {
                bool evicted = false;
                for (const typed::ThreadId& threadId : state.threadOrder) {
                    auto thread = state.threads.find(threadId.value);
                    if (thread == state.threads.end()) {
                        continue;
                    }
                    for (const typed::TurnId& turnId : thread->second.turnOrder) {
                        const auto turn = thread->second.turns.find(turnId.value);
                        if (!insertedTurns.contains(TurnKey{threadId.value, turnId.value}) && turn != thread->second.turns.end() &&
                            turn->second.terminal && !turn->second.active && !turnHasActiveItem(turn->second) &&
                            !turnIsReferenced(turn->second, threadId.value, turnId.value, referenced)) {
                            accountTurnRemoval(state.capacity, turn->second, true);
                            const std::string id = turnId.value;
                            eraseTurn(state, thread->second, id);
                            evicted = true;
                            break;
                        }
                    }
                    if (evicted) {
                        break;
                    }
                }
                if (!evicted) {
                    for (auto inserted = insertions.turns.rbegin(); inserted != insertions.turns.rend() && !evicted; ++inserted) {
                        const auto& [threadId, turnId] = *inserted;
                        auto thread = state.threads.find(threadId);
                        if (thread == state.threads.end()) {
                            continue;
                        }
                        const auto turn = thread->second.turns.find(turnId);
                        if (turn != thread->second.turns.end()) {
                            accountTurnRemoval(state.capacity, turn->second, false);
                            eraseTurn(state, thread->second, turnId);
                            evicted = true;
                        }
                    }
                }
                if (!evicted) {
                    saturatingAdd(state.capacity.snapshotOmissions);
                    break;
                }
            }

            while (state.capacity.retainedItems > limits.maxRetainedItems) {
                bool evicted = false;
                for (const typed::ThreadId& threadId : state.threadOrder) {
                    auto thread = state.threads.find(threadId.value);
                    if (thread == state.threads.end()) {
                        continue;
                    }
                    for (const typed::TurnId& turnId : thread->second.turnOrder) {
                        auto turn = thread->second.turns.find(turnId.value);
                        if (turn == thread->second.turns.end()) {
                            continue;
                        }
                        for (const typed::ItemId& itemIdValue : turn->second.itemOrder) {
                            const auto item = turn->second.items.find(itemIdValue.value);
                            if (!insertedItems.contains(ItemKey{threadId.value, turnId.value, itemIdValue.value}) &&
                                item != turn->second.items.end() &&
                                (item->second.lifecycle == ItemLifecycle::Completed || item->second.lifecycle == ItemLifecycle::Failed) &&
                                !item->second.connectionInvalidated &&
                                !referenced.items.contains(ItemKey{threadId.value, turnId.value, itemIdValue.value})) {
                                const std::string id = itemIdValue.value;
                                eraseItem(state, turn->second, id);
                                saturatingAdd(state.capacity.evictedItems);
                                evicted = true;
                                break;
                            }
                        }
                        if (evicted) {
                            break;
                        }
                    }
                    if (evicted) {
                        break;
                    }
                }
                if (!evicted) {
                    for (auto inserted = insertions.items.rbegin(); inserted != insertions.items.rend() && !evicted; ++inserted) {
                        const auto& [threadId, turnId, itemIdValue] = *inserted;
                        auto thread = state.threads.find(threadId);
                        if (thread == state.threads.end()) {
                            continue;
                        }
                        auto turn = thread->second.turns.find(turnId);
                        if (turn != thread->second.turns.end() && turn->second.items.contains(itemIdValue)) {
                            eraseItem(state, turn->second, itemIdValue);
                            saturatingAdd(state.capacity.snapshotOmissions);
                            evicted = true;
                        }
                    }
                }
                if (!evicted) {
                    saturatingAdd(state.capacity.snapshotOmissions);
                    break;
                }
            }

            std::size_t excess = state.capacity.accumulatedContentBytes > limits.maxAccumulatedContentBytes
                                     ? state.capacity.accumulatedContentBytes - limits.maxAccumulatedContentBytes
                                     : 0;
            const auto trimPass = [&state, &excess](bool inactiveTerminalOnly) {
                for (const typed::ThreadId& threadId : state.threadOrder) {
                    auto thread = state.threads.find(threadId.value);
                    if (thread == state.threads.end()) {
                        continue;
                    }
                    for (const typed::TurnId& turnId : thread->second.turnOrder) {
                        auto turn = thread->second.turns.find(turnId.value);
                        if (turn == thread->second.turns.end() ||
                            (inactiveTerminalOnly && (turn->second.active || !turn->second.terminal))) {
                            continue;
                        }
                        for (const typed::ItemId& itemIdValue : turn->second.itemOrder) {
                            auto item = turn->second.items.find(itemIdValue.value);
                            if (item == turn->second.items.end()) {
                                continue;
                            }
                            auto trim = [&excess, &item, &state](std::string& content) {
                                const std::size_t removed = std::min(excess, content.size());
                                if (removed == 0) {
                                    return;
                                }
                                content.erase(0, removed);
                                excess -= removed;
                                guardedSubtractSize(state.capacity.accumulatedContentBytes, removed);
                                saturatingAdd(item->second.droppedContentBytes, saturatingUint64(removed));
                                saturatingAdd(state.capacity.droppedContentBytes, saturatingUint64(removed));
                            };
                            trim(item->second.agentText);
                            trim(item->second.reasoningText);
                            trim(item->second.reasoningSummary);
                            trim(item->second.commandOutput);
                            if (excess == 0) {
                                return;
                            }
                        }
                    }
                }
            };
            trimPass(true);
            if (excess != 0) {
                trimPass(false);
            }
        }

        void retainExtension(BackendState& state,
                             ExtensionRecord extension,
                             std::size_t limit,
                             std::size_t methodByteLimit,
                             std::size_t payloadByteLimit,
                             std::size_t decodingErrorByteLimit) {
            if (limit == 0) {
                return;
            }
            if (extension.method.size() > methodByteLimit) {
                extension.originalMethodBytes = static_cast<std::uint64_t>(extension.method.size());
                extension.method.resize(methodByteLimit);
            }
            if (extension.decodingError && extension.decodingError->size() > decodingErrorByteLimit) {
                extension.originalDecodingErrorBytes = static_cast<std::uint64_t>(extension.decodingError->size());
                extension.decodingError->resize(decodingErrorByteLimit);
            }
            if (extension.diagnostic) {
                std::uint64_t originalDiagnosticBytes = 0;
                const auto account = [&originalDiagnosticBytes](std::size_t bytes) {
                    const std::uint64_t value = saturatingUint64(bytes);
                    originalDiagnosticBytes = value > std::numeric_limits<std::uint64_t>::max() - originalDiagnosticBytes
                                                  ? std::numeric_limits<std::uint64_t>::max()
                                                  : originalDiagnosticBytes + value;
                };
                account(extension.diagnostic->surface.size());
                account(extension.diagnostic->fieldPath.size());
                account(extension.diagnostic->message.size());
                bool diagnosticTruncated = false;
                if (extension.diagnostic->surface.size() > methodByteLimit) {
                    extension.diagnostic->surface.resize(methodByteLimit);
                    diagnosticTruncated = true;
                }
                if (extension.diagnostic->fieldPath.size() > decodingErrorByteLimit) {
                    extension.diagnostic->fieldPath.resize(decodingErrorByteLimit);
                    diagnosticTruncated = true;
                }
                if (extension.diagnostic->message.size() > decodingErrorByteLimit) {
                    extension.diagnostic->message.resize(decodingErrorByteLimit);
                    diagnosticTruncated = true;
                }
                if (diagnosticTruncated) {
                    extension.originalDiagnosticBytes = originalDiagnosticBytes;
                }
            }
            try {
                const std::string encoded = extension.payload.dump();
                if (encoded.size() > payloadByteLimit) {
                    extension.originalPayloadBytes = static_cast<std::uint64_t>(encoded.size());
                    extension.payload = Json::object({{"truncated", true},
                                                      {"originalBytes", encoded.size()},
                                                      {"omitted", true},
                                                      {"reason", "extension payload exceeds canonical reducer bound"}});
                }
            } catch (...) {
                extension.payload = Json::object({{"omitted", true}, {"reason", "extension serialization failed"}});
            }
            state.recentExtensions.push_back(std::move(extension));
            if (state.recentExtensions.size() > limit) {
                state.recentExtensions.erase(state.recentExtensions.begin(),
                                             state.recentExtensions.begin() +
                                                 static_cast<std::ptrdiff_t>(state.recentExtensions.size() - limit));
            }
        }
    } // namespace

    CodexExtensionReceived detail::preserveUnmodeledTypedEvent(UnmodeledTypedEvent event) {
        return {.method = std::move(event.surface),
                .payload = std::move(event.rawPayload),
                .decodingError = std::move(event.decodingError),
                .diagnostic = std::move(event.diagnostic)};
    }

    Reducer::Reducer(ReducerOptions options)
        : options(std::move(options)) {
    }

    Reduction Reducer::apply(BackendState& state, const BackendEvent& event) const {
        RetentionInsertions insertions;
        Reduction reduction = std::visit(
            Overloaded{
                [&state](const ProviderLifecycleChanged& value) {
                    const bool changed = !sameProvider(state.provider, value.provider);
                    state.provider = value.provider;
                    return Reduction{changed,
                                     value.provider.lifecycle == ProviderLifecycle::Failed ||
                                         value.provider.lifecycle == ProviderLifecycle::Stopping ||
                                         value.provider.lifecycle == ProviderLifecycle::Recovering};
                },
                [&state](const ProviderConnectionInvalidated& value) {
                    if (value.generation != state.provider.generation) {
                        return Reduction{};
                    }
                    Reduction reduction{true, true};
                    reduction.pendingRequestRemovals.reserve(state.pendingRequests.size());
                    for (const auto& [id, pending] : state.pendingRequests) {
                        (void) pending;
                        reduction.pendingRequestRemovals.push_back({id, value.reason});
                    }
                    for (auto& [threadId, thread] : state.threads) {
                        (void) threadId;
                        thread.stamp.freshness = Freshness::Stale;
                        for (auto& [turnId, turn] : thread.turns) {
                            (void) turnId;
                            turn.stamp.freshness = Freshness::Stale;
                            turn.connectionInvalidated = turn.active || !turn.terminal;
                            for (auto& [itemIdValue, item] : turn.items) {
                                (void) itemIdValue;
                                item.stamp.freshness = Freshness::Stale;
                                item.connectionInvalidated =
                                    item.lifecycle == ItemLifecycle::Started || item.lifecycle == ItemLifecycle::Unknown;
                            }
                        }
                    }
                    state.threadList.stamp.freshness = Freshness::Stale;
                    state.pendingRequests.clear();
                    return reduction;
                },
                [&state](const CapacityConfigured& value) {
                    const bool changed = state.capacity.limits != value.limits;
                    state.capacity.limits = value.limits;
                    return Reduction{changed, false};
                },
                [&state](const CapacityChanged& value) {
                    std::uint64_t* counter = nullptr;
                    switch (value.metric) {
                        case CapacityMetric::RejectedSessions:
                            counter = &state.capacity.rejectedSessions;
                            break;
                        case CapacityMetric::RejectedObservers:
                            counter = &state.capacity.rejectedObservers;
                            break;
                        case CapacityMetric::RejectedOperations:
                            counter = &state.capacity.rejectedOperations;
                            break;
                        case CapacityMetric::ProviderRequestOverflows:
                            counter = &state.capacity.providerRequestOverflows;
                            break;
                        case CapacityMetric::EvictedThreads:
                            counter = &state.capacity.evictedThreads;
                            break;
                        case CapacityMetric::EvictedTurns:
                            counter = &state.capacity.evictedTurns;
                            break;
                        case CapacityMetric::EvictedItems:
                            counter = &state.capacity.evictedItems;
                            break;
                        case CapacityMetric::DroppedContentBytes:
                            counter = &state.capacity.droppedContentBytes;
                            break;
                        case CapacityMetric::SnapshotOmissions:
                            counter = &state.capacity.snapshotOmissions;
                            break;
                    }
                    saturatingAdd(*counter, value.amount);
                    return Reduction{true, true};
                },
                [this, &state](const DiagnosticReceived& value) {
                    ++state.diagnostics.received;
                    if (options.retainedDiagnostics != 0) {
                        std::string message = value.message;
                        if (message.size() > options.maxDiagnosticBytes) {
                            message.erase(0, message.size() - options.maxDiagnosticBytes);
                        }
                        state.diagnostics.recent.push_back(std::move(message));
                        if (state.diagnostics.recent.size() > options.retainedDiagnostics) {
                            state.diagnostics.recent.erase(
                                state.diagnostics.recent.begin(),
                                state.diagnostics.recent.begin() +
                                    static_cast<std::ptrdiff_t>(state.diagnostics.recent.size() - options.retainedDiagnostics));
                        }
                    }
                    return Reduction{true, false};
                },
                [this, &state, &insertions](const ThreadUpserted& value) {
                    return Reduction{upsertThread(state, value.thread, value.load, options.maxAccumulatedItemBytes, &insertions), false};
                },
                [this, &state, &insertions](const ThreadListUpdated& value) {
                    for (const typed::Thread& thread : value.page.data) {
                        upsertThread(state, thread, EntityLoad::Summary, options.maxAccumulatedItemBytes, &insertions);
                    }
                    const bool firstPageForGeneration = state.threadList.stamp.generation != state.provider.generation ||
                                                        state.threadList.stamp.freshness != Freshness::Current;
                    if (value.initialRefresh || firstPageForGeneration) {
                        state.threadList = {};
                    }
                    state.threadList.hasLoadedPage = true;
                    saturatingAddSize(state.threadList.pagesLoaded, 1);
                    state.threadList.nextCursor = value.page.nextCursor.hasValue() ? value.page.nextCursor.value : std::nullopt;
                    state.threadList.backwardsCursor =
                        value.page.backwardsCursor.hasValue() ? value.page.backwardsCursor.value : std::nullopt;
                    state.threadList.complete = !value.page.nextCursor.hasValue();
                    state.threadList.stamp = currentStamp(state);
                    return Reduction{true, false};
                },
                [&state, &insertions](const ThreadStatusUpdated& value) {
                    ThreadState& thread = ensureThread(state, value.threadId, &insertions);
                    thread.thread.status = value.status;
                    thread.stamp = currentStamp(state);
                    if (thread.thread.raw.is_object() && thread.thread.raw.size() == 1 &&
                        thread.thread.raw.value("backendPlaceholder", false)) {
                        thread.thread.raw["backendPlaceholderStatusKnown"] = true;
                    }
                    return Reduction{true, false};
                },
                [this, &state, &insertions](const TurnUpserted& value) {
                    return Reduction{upsertTurn(state, value.turn, options.maxAccumulatedItemBytes, &insertions), false};
                },
                [this, &state, &insertions](const TurnCompleted& value) {
                    const TurnState* prior = findTurn(state, value.turn.threadId, value.turn.id);
                    const bool emitTerminal = prior != nullptr && !prior->terminal;
                    upsertTurn(state, value.turn, options.maxAccumulatedItemBytes, &insertions);
                    TurnState& turn = ensureTurn(state, value.turn.threadId, value.turn.id, &insertions);
                    turn.active = false;
                    turn.terminal = true;
                    if (emitTerminal) {
                        const bool cancelled = value.turn.status.value == "cancelled" || value.turn.status.value == "interrupted";
                        const char* outcome = value.turn.status.value == "failed" ? "failed" : (cancelled ? "cancelled" : "completed");
                        lifecycleLog().debug("turn {}: thread={} turn={}", outcome, value.turn.threadId.value, value.turn.id.value);
                    }
                    return Reduction{true, true};
                },
                [this, &state, &insertions](const TurnFailed& value) {
                    const TurnState* prior = findTurn(state, value.turn.threadId, value.turn.id);
                    const bool emitTerminal = prior != nullptr && !prior->terminal;
                    upsertTurn(state, value.turn, options.maxAccumulatedItemBytes, &insertions);
                    TurnState& turn = ensureTurn(state, value.turn.threadId, value.turn.id, &insertions);
                    turn.active = false;
                    turn.terminal = true;
                    turn.failure = value.error;
                    if (emitTerminal) {
                        lifecycleLog().debug("turn failed: thread={} turn={}", value.turn.threadId.value, value.turn.id.value);
                    }
                    return Reduction{true, true};
                },
                [&state, &insertions](const TurnErrorUpdated& value) {
                    TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                    turn.failure = value.error;
                    turn.stamp = currentStamp(state);
                    turn.connectionInvalidated = false;
                    if (!value.willRetry) {
                        turn.active = false;
                    }
                    return Reduction{true, !value.willRetry};
                },
                [this, &state, &insertions](const ItemUpserted& value) {
                    if (upsertItem(state,
                                   value.threadId,
                                   value.turnId,
                                   value.item,
                                   value.lifecycle,
                                   value.occurredAtMs,
                                   options.maxAccumulatedItemBytes,
                                   &insertions)) {
                        return Reduction{true, value.lifecycle == ItemLifecycle::Completed || value.lifecycle == ItemLifecycle::Failed};
                    }
                    retainExtension(state,
                                    {.method = "codex/item-without-id",
                                     .payload = std::visit(
                                         [](const auto& item) {
                                             using Item = std::decay_t<decltype(item)>;
                                             if constexpr (std::is_same_v<Item, typed::UnknownItem>) {
                                                 return item.raw;
                                             } else {
                                                 return item.metadata.raw;
                                             }
                                         },
                                         value.item),
                                     .decodingError = "typed item has no stable id",
                                     .originalMethodBytes = std::nullopt,
                                     .originalPayloadBytes = std::nullopt,
                                     .originalDecodingErrorBytes = std::nullopt,
                                     .diagnostic = std::nullopt,
                                     .originalDiagnosticBytes = std::nullopt},
                                    options.retainedExtensions,
                                    options.maxExtensionMethodBytes,
                                    options.maxExtensionBytes,
                                    options.maxExtensionDecodingErrorBytes);
                    return Reduction{true, value.lifecycle == ItemLifecycle::Completed};
                },
                [this, &state, &insertions](const ItemContentChanged& value) {
                    TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                    auto iterator = turn.items.find(value.itemId.value);
                    if (iterator == turn.items.end()) {
                        typed::UnknownItem placeholder;
                        placeholder.type = "backend.delta-placeholder";
                        placeholder.raw = Json::object({{"id", value.itemId.value}, {"backendPlaceholder", true}});
                        placeholder.metadata = {value.itemId, value.threadId, value.turnId};
                        upsertItem(state,
                                   value.threadId,
                                   value.turnId,
                                   typed::ThreadItem{std::move(placeholder)},
                                   ItemLifecycle::Started,
                                   std::nullopt,
                                   options.maxAccumulatedItemBytes,
                                   &insertions);
                        iterator = turn.items.find(value.itemId.value);
                    }
                    ItemState& item = iterator->second;
                    const std::size_t previousContentBytes = itemContentBytes(item);
                    item.stamp = currentStamp(state);
                    item.connectionInvalidated = false;
                    switch (value.kind) {
                        case ItemContentChanged::Kind::AgentText:
                            appendBounded(item.agentText, value.delta, options.maxAccumulatedItemBytes, item.droppedContentBytes);
                            break;
                        case ItemContentChanged::Kind::ReasoningText:
                            appendBounded(item.reasoningText, value.delta, options.maxAccumulatedItemBytes, item.droppedContentBytes);
                            break;
                        case ItemContentChanged::Kind::ReasoningSummary:
                            appendBounded(item.reasoningSummary, value.delta, options.maxAccumulatedItemBytes, item.droppedContentBytes);
                            break;
                        case ItemContentChanged::Kind::CommandOutput:
                            appendBounded(item.commandOutput, value.delta, options.maxAccumulatedItemBytes, item.droppedContentBytes);
                            break;
                    }
                    replaceAccumulatedContent(state.capacity, previousContentBytes, itemContentBytes(item));
                    return Reduction{true, false};
                },
                [this, &state, &insertions](const FileChangeUpdated& value) {
                    ItemState* item = findItem(state, value.threadId, value.turnId, value.itemId);
                    if (!item) {
                        typed::UnknownItem placeholder;
                        placeholder.type = "fileChange";
                        placeholder.raw = Json::object({{"id", value.itemId.value}, {"changes", value.changes}});
                        placeholder.metadata = {value.itemId, value.threadId, value.turnId};
                        upsertItem(state,
                                   value.threadId,
                                   value.turnId,
                                   typed::ThreadItem{std::move(placeholder)},
                                   ItemLifecycle::Started,
                                   std::nullopt,
                                   options.maxAccumulatedItemBytes,
                                   &insertions);
                        item = findItem(state, value.threadId, value.turnId, value.itemId);
                    }
                    if (item) {
                        item->stamp = currentStamp(state);
                        item->connectionInvalidated = false;
                        if (auto* fileChange = std::get_if<typed::FileChangeThreadItem>(&item->item)) {
                            fileChange->metadata.raw["changes"] = value.changes;
                            if (std::optional<std::vector<typed::FileUpdateChange>> changes = decodeFileUpdateChanges(value.changes)) {
                                fileChange->changes = std::move(*changes);
                            }
                        }
                        item->extensions["fileChanges"] = value.changes;
                    }
                    return Reduction{true, false};
                },
                [&state, &insertions](const TokenUsageUpdated& value) {
                    TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                    turn.tokenUsage = value.usage;
                    turn.stamp = currentStamp(state);
                    turn.connectionInvalidated = false;
                    return Reduction{true, false};
                },
                [this, &state, &insertions](const ModelRerouted& value) {
                    TurnState& turn = ensureTurn(state, value.threadId, value.turnId, &insertions);
                    turn.stamp = currentStamp(state);
                    turn.connectionInvalidated = false;
                    if (options.maxModelReroutesPerTurn != 0) {
                        turn.modelReroutes.push_back({value.from, value.to, value.reason});
                        if (turn.modelReroutes.size() > options.maxModelReroutesPerTurn) {
                            turn.modelReroutes.erase(
                                turn.modelReroutes.begin(),
                                turn.modelReroutes.begin() +
                                    static_cast<std::ptrdiff_t>(turn.modelReroutes.size() - options.maxModelReroutesPerTurn));
                        }
                    }
                    return Reduction{true, false};
                },
                [&state](const PendingRequestAdded& value) {
                    state.pendingRequests.insert_or_assign(value.pending.id, value.pending);
                    return Reduction{true, true};
                },
                [&state](const PendingRequestRemoved& value) {
                    return Reduction{state.pendingRequests.erase(value.id) != 0, true};
                },
                [&state](const ControllerChanged& value) {
                    const bool changed = state.controller != value.controller;
                    if (state.controller) {
                        const auto previous = state.sessions.find(*state.controller);
                        if (previous != state.sessions.end()) {
                            previous->second.role = SessionRole::Observer;
                        }
                    }
                    state.controller = value.controller;
                    if (state.controller) {
                        const auto current = state.sessions.find(*state.controller);
                        if (current != state.sessions.end()) {
                            current->second.role = SessionRole::Controller;
                        }
                    }
                    return Reduction{changed, true};
                },
                [&state](const SessionChanged& value) {
                    if (value.connected) {
                        state.sessions.insert_or_assign(value.id, ConnectedSessionState{value.id, value.role});
                    } else {
                        state.sessions.erase(value.id);
                        if (state.controller == value.id) {
                            state.controller.reset();
                        }
                    }
                    return Reduction{true, true};
                },
                [this, &state](const CodexExtensionReceived& value) {
                    retainExtension(state,
                                    {.method = value.method,
                                     .payload = value.payload,
                                     .decodingError = value.decodingError,
                                     .originalMethodBytes = std::nullopt,
                                     .originalPayloadBytes = std::nullopt,
                                     .originalDecodingErrorBytes = std::nullopt,
                                     .diagnostic = value.diagnostic,
                                     .originalDiagnosticBytes = std::nullopt},
                                    options.retainedExtensions,
                                    options.maxExtensionMethodBytes,
                                    options.maxExtensionBytes,
                                    options.maxExtensionDecodingErrorBytes);
                    return Reduction{true, false};
                }},
            event);
        const CapacityState capacityBefore = state.capacity;
        enforceRetentionCapacity(state, insertions);
        reduction.changed = reduction.changed || state.capacity != capacityBefore;
        const auto appendCapacityChange = [&reduction](CapacityMetric metric, std::uint64_t before, std::uint64_t after) {
            if (after > before) {
                reduction.capacityChanges.push_back({metric, after - before});
            }
        };
        appendCapacityChange(CapacityMetric::EvictedThreads, capacityBefore.evictedThreads, state.capacity.evictedThreads);
        appendCapacityChange(CapacityMetric::EvictedTurns, capacityBefore.evictedTurns, state.capacity.evictedTurns);
        appendCapacityChange(CapacityMetric::EvictedItems, capacityBefore.evictedItems, state.capacity.evictedItems);
        appendCapacityChange(CapacityMetric::DroppedContentBytes, capacityBefore.droppedContentBytes, state.capacity.droppedContentBytes);
        appendCapacityChange(CapacityMetric::SnapshotOmissions, capacityBefore.snapshotOmissions, state.capacity.snapshotOmissions);
        return reduction;
    }

    std::vector<BackendEvent> Reducer::translate(const typed::Event& event) const {
        return std::visit(
            Overloaded{
                [](const typed::ThreadStarted& value) -> std::vector<BackendEvent> {
                    lifecycleLog().info("thread created: thread={}", value.thread.id.value);
                    return {ThreadUpserted{value.thread, EntityLoad::Summary}};
                },
                [](const typed::ThreadStatusChanged& value) -> std::vector<BackendEvent> {
                    return {ThreadStatusUpdated{value.threadId, value.status}};
                },
                [](const typed::TurnStarted& value) -> std::vector<BackendEvent> {
                    lifecycleLog().debug("turn started: thread={} turn={}", value.turn.threadId.value, value.turn.id.value);
                    return {TurnUpserted{value.turn}};
                },
                [](const typed::TurnCompleted& value) -> std::vector<BackendEvent> {
                    return {TurnCompleted{value.turn}};
                },
                [](const typed::TurnFailed& value) -> std::vector<BackendEvent> {
                    return {TurnFailed{value.turn, value.error}};
                },
                [](const typed::ItemStarted& value) -> std::vector<BackendEvent> {
                    const auto location = itemLocation(value.item);
                    if (!location) {
                        return {CodexExtensionReceived{"item/started", value.raw, "item event omitted threadId or turnId", std::nullopt}};
                    }
                    return {ItemUpserted{location->first, location->second, value.item, ItemLifecycle::Started, value.startedAtMs}};
                },
                [](const typed::ItemCompleted& value) -> std::vector<BackendEvent> {
                    const auto location = itemLocation(value.item);
                    if (!location) {
                        return {CodexExtensionReceived{"item/completed", value.raw, "item event omitted threadId or turnId", std::nullopt}};
                    }
                    return {ItemUpserted{location->first, location->second, value.item, ItemLifecycle::Completed, value.completedAtMs}};
                },
                [](const typed::AgentMessageDelta& value) -> std::vector<BackendEvent> {
                    return {ItemContentChanged{
                        value.threadId, value.turnId, value.itemId, ItemContentChanged::Kind::AgentText, value.text, std::nullopt}};
                },
                [](const typed::ReasoningDelta& value) -> std::vector<BackendEvent> {
                    return {ItemContentChanged{value.threadId,
                                               value.turnId,
                                               value.itemId,
                                               value.kind == typed::ReasoningDelta::Kind::Summary
                                                   ? ItemContentChanged::Kind::ReasoningSummary
                                                   : ItemContentChanged::Kind::ReasoningText,
                                               value.text,
                                               value.index}};
                },
                [](const typed::CommandOutputDelta& value) -> std::vector<BackendEvent> {
                    return {ItemContentChanged{
                        value.threadId, value.turnId, value.itemId, ItemContentChanged::Kind::CommandOutput, value.output, std::nullopt}};
                },
                [](const typed::FileChangeUpdated& value) -> std::vector<BackendEvent> {
                    return {FileChangeUpdated{value.threadId, value.turnId, value.itemId, value.changes}};
                },
                [](const typed::TokenUsageUpdated& value) -> std::vector<BackendEvent> {
                    return {TokenUsageUpdated{value.threadId, value.turnId, value.usage}};
                },
                [](const typed::TerminalInteractionNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::TerminalInteraction);
                },
                [](const typed::FileChangeOutputDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::FileChangeOutputDelta);
                },
                [](const typed::McpToolCallProgressNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::McpToolCallProgress);
                },
                [](const typed::PlanDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::PlanDelta);
                },
                [](const typed::ReasoningSummaryPartAddedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ReasoningSummaryPartAdded);
                },
                [](const typed::ThreadArchivedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadArchived);
                },
                [](const typed::ThreadClosedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadClosed);
                },
                [](const typed::ContextCompactedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ContextCompacted);
                },
                [](const typed::ThreadDeletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadDeleted);
                },
                [](const typed::ThreadGoalClearedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadGoalCleared);
                },
                [](const typed::ThreadGoalUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadGoalUpdated);
                },
                [](const typed::ThreadNameUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadNameUpdated);
                },
                [](const typed::ThreadRealtimeClosedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeClosed);
                },
                [](const typed::ThreadRealtimeErrorNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeError);
                },
                [](const typed::ThreadRealtimeItemAddedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeItemAdded);
                },
                [](const typed::ThreadRealtimeOutputAudioDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeOutputAudioDelta);
                },
                [](const typed::ThreadRealtimeSdpNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeSdp);
                },
                [](const typed::ThreadRealtimeStartedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeStarted);
                },
                [](const typed::ThreadRealtimeTranscriptDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeTranscriptDelta);
                },
                [](const typed::ThreadRealtimeTranscriptDoneNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadRealtimeTranscriptDone);
                },
                [](const typed::ThreadSettingsUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadSettingsUpdated);
                },
                [](const typed::ThreadUnarchivedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ThreadUnarchived);
                },
                [](const typed::TurnDiffUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::TurnDiffUpdated);
                },
                [](const typed::TurnModerationMetadataNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::TurnModerationMetadata);
                },
                [](const typed::TurnPlanUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::TurnPlanUpdated);
                },
                [](const typed::AccountLoginCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::AccountLoginCompleted);
                },
                [](const typed::AccountRateLimitsUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::AccountRateLimitsUpdated);
                },
                [](const typed::AccountUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::AccountUpdated);
                },
                [](const typed::ConfigWarningNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ConfigWarning);
                },
                [](const typed::ModelRerouted& value) -> std::vector<BackendEvent> {
                    return {ModelRerouted{value.threadId, value.turnId, value.from, value.to, value.reason}};
                },
                [](const typed::ModelSafetyBufferingUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ModelSafetyBufferingUpdated);
                },
                [](const typed::ModelVerificationNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ModelVerification);
                },
                [](const typed::TurnErrorEvent& value) -> std::vector<BackendEvent> {
                    return {TurnErrorUpdated{value.threadId, value.turnId, value.error, value.willRetry}};
                },
                [](const typed::UnknownEvent& value) -> std::vector<BackendEvent> {
                    return {detail::preserveUnmodeledTypedEvent({value.method,
                                                                 value.params,
                                                                 ::ai::openai::codex::detail::safeDecodeDiagnosticText(value.diagnostic),
                                                                 value.diagnostic})};
                },
                [](const typed::CommandExecOutputDeltaNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::CommandExecOutputDelta);
                },
                [](const typed::FsChangedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::FsChanged);
                },
                [](const typed::FuzzyFileSearchSessionCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::FuzzyFileSearchSessionCompleted);
                },
                [](const typed::FuzzyFileSearchSessionUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::FuzzyFileSearchSessionUpdated);
                },
                [](const typed::GuardianWarningNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::GuardianWarning);
                },
                [](const typed::ItemGuardianApprovalReviewCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ItemGuardianApprovalReviewCompleted);
                },
                [](const typed::ItemGuardianApprovalReviewStartedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ItemGuardianApprovalReviewStarted);
                },
                [](const typed::AppListUpdatedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::AppListUpdated);
                },
                [](const typed::ExternalAgentConfigImportCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ExternalAgentConfigImportCompleted);
                },
                [](const typed::ExternalAgentConfigImportProgressNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::ExternalAgentConfigImportProgress);
                },
                [](const typed::HookCompletedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::HookCompleted);
                },
                [](const typed::HookStartedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::HookStarted);
                },
                [](const typed::SkillsChangedNotification& value) -> std::vector<BackendEvent> {
                    return preserveTypedNotification(value, ServerNotificationTarget::SkillsChanged);
                },
                [](const typed::McpServerOauthLoginCompletedNotification&) -> std::vector<BackendEvent> {
                    return {};
                },
                [](const typed::McpServerStatusUpdatedNotification&) -> std::vector<BackendEvent> {
                    return {};
                },
                [](const typed::DeprecationNoticeNotification&) -> std::vector<BackendEvent> {
                    return {};
                },
                [](const typed::ProcessExitedNotification&) -> std::vector<BackendEvent> {
                    return {};
                },
                [](const typed::ProcessOutputDeltaNotification&) -> std::vector<BackendEvent> {
                    return {};
                },
                [](const typed::RemoteControlStatusChangedNotification&) -> std::vector<BackendEvent> {
                    return {};
                },
                [](const typed::ServerRequestResolvedNotification&) -> std::vector<BackendEvent> {
                    return {};
                },
                [](const typed::WarningNotification&) -> std::vector<BackendEvent> {
                    return {};
                },
                [](const typed::WindowsWorldWritableWarningNotification&) -> std::vector<BackendEvent> {
                    return {};
                },
                [](const typed::WindowsSandboxSetupCompletedNotification&) -> std::vector<BackendEvent> {
                    return {};
                }},
            event);
    }

} // namespace ai::openai::codex::backend
