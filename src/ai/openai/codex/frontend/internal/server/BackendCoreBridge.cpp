/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/internal/server/BackendCoreBridge.h"

#include "ai/openai/codex/backend/BackendCommand.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/backend/BackendEvent.h"
#include "ai/openai/codex/backend/FrontendSession.h"
#include "ai/openai/codex/backend/Snapshot.h"
#include "ai/openai/codex/frontend/detail/BackendCommandMapper.h"
#include "ai/openai/codex/frontend/detail/ProviderResultProjection.h"
#include "ai/openai/codex/frontend/internal/server/BackendProjection.h"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace ai::openai::codex::frontend::internal::server {

    namespace {

        ErrorCode frontendErrorCode(backend::CommandErrorCode code) noexcept {
            switch (code) {
                case backend::CommandErrorCode::PermissionDenied:
                    return ErrorCode::PermissionDenied;
                case backend::CommandErrorCode::InvalidCommand:
                    return ErrorCode::InvalidCommand;
                case backend::CommandErrorCode::NotFound:
                    return ErrorCode::NotFound;
                case backend::CommandErrorCode::Conflict:
                    return ErrorCode::Conflict;
                case backend::CommandErrorCode::LocalSubmissionFailure:
                    return ErrorCode::LocalSubmissionFailure;
                case backend::CommandErrorCode::TypedDecodingFailure:
                    return ErrorCode::TypedDecodingFailure;
                case backend::CommandErrorCode::RemoteAppServerError:
                    return ErrorCode::RemoteAppServerError;
                case backend::CommandErrorCode::Cancelled:
                    return ErrorCode::Cancelled;
                case backend::CommandErrorCode::BackendUnavailable:
                    return ErrorCode::BackendUnavailable;
            }
            return ErrorCode::InternalError;
        }

        std::string frontendErrorMessage(backend::CommandErrorCode code) {
            switch (code) {
                case backend::CommandErrorCode::PermissionDenied:
                    return "frontend command was denied";
                case backend::CommandErrorCode::InvalidCommand:
                    return "frontend command is invalid";
                case backend::CommandErrorCode::NotFound:
                    return "frontend command target was not found";
                case backend::CommandErrorCode::Conflict:
                    return "frontend command conflicts with current state";
                case backend::CommandErrorCode::LocalSubmissionFailure:
                    return "frontend command could not be submitted";
                case backend::CommandErrorCode::TypedDecodingFailure:
                    return "frontend command result could not be decoded";
                case backend::CommandErrorCode::RemoteAppServerError:
                    return "Codex App Server rejected the command";
                case backend::CommandErrorCode::Cancelled:
                    return "frontend command was cancelled";
                case backend::CommandErrorCode::BackendUnavailable:
                    return "Codex App Server is unavailable";
            }
            return "frontend command failed";
        }

        bool authoritativeThreadReadAbsence(const backend::CommandCompletion& completion,
                                            const backend::ThreadSnapshotAtSequence* captured) noexcept {
            return captured && !captured->thread && completion.result.error &&
                   completion.result.error->code == backend::CommandErrorCode::NotFound;
        }

        bool isFrontendV1MetadataOnlyItem(std::string_view type) noexcept {
            return type == "collabAgentToolCall" || type == "contextCompaction" || type == "enteredReviewMode" ||
                   type == "exitedReviewMode" || type == "hookPrompt" || type == "imageGeneration" || type == "imageView" ||
                   type == "plan" || type == "sleep" || type == "subAgentActivity";
        }

        backend::ItemContentSnapshotChannel contentSnapshotChannel(backend::ItemContentChanged::Kind kind) noexcept {
            switch (kind) {
                case backend::ItemContentChanged::Kind::AgentText:
                    return backend::ItemContentSnapshotChannel::AgentText;
                case backend::ItemContentChanged::Kind::ReasoningText:
                    return backend::ItemContentSnapshotChannel::ReasoningText;
                case backend::ItemContentChanged::Kind::ReasoningSummary:
                    return backend::ItemContentSnapshotChannel::ReasoningSummary;
                case backend::ItemContentChanged::Kind::CommandOutput:
                    return backend::ItemContentSnapshotChannel::CommandOutput;
            }
            return backend::ItemContentSnapshotChannel::AgentText;
        }

        backend::ItemContentSnapshotKey contentSnapshotKey(const backend::ItemContentChanged& content) {
            return {content.threadId, content.turnId, content.itemId, contentSnapshotChannel(content.kind)};
        }

        struct ItemContentSnapshotKeyLess {
            bool operator()(const backend::ItemContentSnapshotKey& left,
                            const backend::ItemContentSnapshotKey& right) const noexcept {
                return std::tie(left.threadId.value, left.turnId.value, left.itemId.value, left.channel) <
                       std::tie(right.threadId.value, right.turnId.value, right.itemId.value, right.channel);
            }
        };

        bool sameItemContentKey(const backend::ItemContentChanged& left,
                                const backend::ItemContentChanged& right) noexcept {
            return left.threadId == right.threadId && left.turnId == right.turnId && left.itemId == right.itemId &&
                   left.kind == right.kind;
        }

        bool itemContentSnapshotIsAhead(backend::SequenceNumber eventSequence,
                                        backend::SequenceNumber snapshotSequence) noexcept {
            return snapshotSequence > eventSequence;
        }

        std::optional<std::vector<backend::SequencedBackendEvent>>
        coalescedItemContentEvents(std::span<const backend::SequencedBackendEvent> events) noexcept {
            try {
                std::vector<backend::SequencedBackendEvent> coalesced;
                coalesced.reserve(events.size());
                for (const backend::SequencedBackendEvent& sequenced : events) {
                    const auto* content = std::get_if<backend::ItemContentChanged>(&sequenced.event);
                    if (content == nullptr) {
                        return std::nullopt;
                    }
                    const auto existing = std::find_if(
                        coalesced.begin(), coalesced.end(), [&](const backend::SequencedBackendEvent& retained) {
                            const auto* prior = std::get_if<backend::ItemContentChanged>(&retained.event);
                            return prior != nullptr && sameItemContentKey(*prior, *content);
                        });
                    if (existing == coalesced.end()) {
                        coalesced.push_back(sequenced);
                        continue;
                    }
                    backend::SequencedBackendEvent combined = std::move(*existing);
                    coalesced.erase(existing);
                    auto& prior = std::get<backend::ItemContentChanged>(combined.event);
                    if (content->delta.size() > std::numeric_limits<std::size_t>::max() - prior.delta.size()) {
                        return std::nullopt;
                    }
                    prior.delta.append(content->delta);
                    // The combined occurrence starts from the first event's
                    // authoritative before-counters and ends at the last
                    // backend sequence. Moving it behind events whose latest
                    // occurrence preceded this one preserves last-update
                    // ordering across interleaved item streams. Missing first
                    // counters deliberately retain conservative replacement
                    // semantics.
                    combined.sequence = sequenced.sequence;
                    coalesced.push_back(std::move(combined));
                }
                return coalesced;
            } catch (...) {
                return std::nullopt;
            }
        }

        Json legacyUserMessageData(const backend::UserMessageSnapshot& message) {
            Json content = Json::array();
            for (const std::string& text : message.textParts) {
                content.push_back(Json{{"type", "text"}, {"text", text}});
            }
            if (content.empty() && !message.text.empty()) {
                content.push_back(Json{{"type", "text"}, {"text", message.text}});
            }
            const auto retainedBytes = static_cast<std::uint64_t>(content.dump().size());
            const auto retainedItems = static_cast<std::uint64_t>(content.size());
            std::uint64_t originalBytes = std::max(message.originalContentBytes, retainedBytes);
            const std::uint64_t originalItems = std::max(message.originalContentItems, retainedItems);
            if (message.textTruncated && originalBytes == retainedBytes &&
                originalBytes != std::numeric_limits<std::uint64_t>::max()) {
                ++originalBytes;
            }
            const bool truncated = originalBytes > retainedBytes || originalItems > retainedItems;
            return Json{{"clientId", message.clientId ? Json(*message.clientId) : Json(nullptr)},
                        {"content", std::move(content)},
                        {"textTruncated", message.textTruncated},
                        {"contentTruncated", truncated},
                        {"originalContentBytes", originalBytes},
                        {"retainedContentBytes", retainedBytes},
                        {"originalContentItems", originalItems},
                        {"retainedContentItems", retainedItems}};
        }

        std::string_view freshnessName(backend::Freshness freshness) noexcept {
            switch (freshness) {
                case backend::Freshness::Unknown:
                    return "unknown";
                case backend::Freshness::Current:
                    return "current";
                case backend::Freshness::Stale:
                    return "stale";
            }
            return "unknown";
        }

        Json sourceStampJson(const backend::SourceStamp& stamp) {
            return Json{{"generation", stamp.generation}, {"freshness", std::string(freshnessName(stamp.freshness))}};
        }

        Json turnPlanJson(const backend::TurnPlanState& plan) {
            Json encoded{{"steps", Json::array()},
                         {"statuses", Json::array()},
                         {"totalSteps", plan.totalSteps},
                         {"truncated", plan.truncated}};
            if (plan.explanation) {
                encoded["explanation"] = *plan.explanation;
            }
            for (const backend::TurnPlanStepState& step : plan.steps) {
                encoded["steps"].push_back(step.step);
                encoded["statuses"].push_back(step.status.value);
            }
            return encoded;
        }

        Json realtimeSnapshotJson(const backend::RealtimeThreadSnapshot& realtime) {
            Json encoded{{"lifecycle", realtime.lifecycle},
                         {"transcript", realtime.transcript},
                         {"itemCount", realtime.itemCount},
                         {"receivedAudioBytes", realtime.receivedAudioBytes},
                         {"droppedAudioBytes", realtime.droppedAudioBytes},
                         {"transcriptTruncated", realtime.transcriptTruncated},
                         {"sourceGeneration", realtime.stamp.generation},
                         {"sourceFreshness", std::string(freshnessName(realtime.stamp.freshness))}};
            if (realtime.lastError) {
                encoded["lastError"] = *realtime.lastError;
                encoded["errorDetailsOmitted"] = false;
            }
            if (realtime.sessionId) {
                encoded["sessionId"] = *realtime.sessionId;
            }
            if (realtime.version) {
                encoded["version"] = *realtime.version;
            }
            if (realtime.lastSdpBytes) {
                encoded["lastSdpBytes"] = *realtime.lastSdpBytes;
            }
            return encoded;
        }

        Json legacyItemSnapshotJson(const backend::ItemSnapshot& item) {
            const Json frontendData = isFrontendV1MetadataOnlyItem(item.type)
                                          ? Json::object({{"codexType", item.type}})
                                      : item.userMessage ? legacyUserMessageData(*item.userMessage)
                                                         : item.data;
            Json encoded{{"id", item.id},
                         {"type", item.type},
                         {"status", item.status},
                         {"agentText", item.agentText},
                         {"reasoningText", item.reasoningText},
                         {"reasoningSummary", item.reasoningSummary},
                         {"commandOutput", item.commandOutput},
                         {"droppedContentBytes", item.droppedContentBytes},
                         {"contentTruncated", item.contentTruncated},
                         {"data", frontendData},
                         {"extensions", item.extensions},
                         {"generation", item.stamp.generation},
                         {"freshness", std::string(freshnessName(item.stamp.freshness))},
                         {"connectionInvalidated", item.connectionInvalidated}};
            if (item.startedAtMs) {
                encoded["startedAtMs"] = *item.startedAtMs;
            }
            if (item.completedAtMs) {
                encoded["completedAtMs"] = *item.completedAtMs;
            }
            return encoded;
        }

        Json turnSnapshotJson(const backend::TurnSnapshot& turn) {
            Json encoded{{"id", turn.id},
                         {"threadId", turn.threadId},
                         {"status", turn.status},
                         {"active", turn.active},
                         {"terminal", turn.terminal},
                         {"items", Json::array()},
                         {"extensions", turn.extensions},
                         {"stamp", sourceStampJson(turn.stamp)},
                         {"connectionInvalidated", turn.connectionInvalidated}};
            if (turn.failure) {
                encoded["failure"] = *turn.failure;
            }
            if (turn.tokenUsage) {
                encoded["tokenUsage"] = *turn.tokenUsage;
            }
            if (turn.plan) {
                encoded["plan"] = turnPlanJson(*turn.plan);
            }
            if (turn.effectiveExecutionConfiguration) {
                encoded["effectiveExecutionConfiguration"] = *turn.effectiveExecutionConfiguration;
            }
            if (turn.effectiveExecutionConfigurationProvenance) {
                encoded["effectiveExecutionConfigurationProvenance"] = *turn.effectiveExecutionConfigurationProvenance;
            }
            for (const backend::ItemSnapshot& item : turn.items) {
                encoded["items"].push_back(legacyItemSnapshotJson(item));
            }
            return encoded;
        }

        Json threadSnapshotJson(const backend::ThreadSnapshot& thread) {
            Json encoded{{"id", thread.id},
                         {"fullyLoaded", thread.fullyLoaded},
                         {"turns", Json::array()},
                         {"extensions", thread.extensions},
                         {"stamp", sourceStampJson(thread.stamp)},
                         {"realtime", realtimeSnapshotJson(thread.realtime)}};
            if (thread.title) {
                encoded["title"] = *thread.title;
            }
            if (thread.cwd) {
                encoded["cwd"] = *thread.cwd;
            }
            if (thread.model) {
                encoded["model"] = *thread.model;
            }
            if (thread.modelProvider) {
                encoded["modelProvider"] = *thread.modelProvider;
            }
            if (thread.preview) {
                encoded["preview"] = *thread.preview;
            }
            if (thread.status) {
                encoded["status"] = *thread.status;
            }
            if (thread.ephemeral) {
                encoded["ephemeral"] = *thread.ephemeral;
            }
            if (thread.archived) {
                encoded["archived"] = *thread.archived;
            }
            if (thread.executionConfiguration) {
                encoded["executionConfiguration"] = *thread.executionConfiguration;
            }
            if (thread.createdAt) {
                encoded["createdAt"] = *thread.createdAt;
            }
            if (thread.updatedAt) {
                encoded["updatedAt"] = *thread.updatedAt;
            }
            for (const backend::TurnSnapshot& turn : thread.turns) {
                encoded["turns"].push_back(turnSnapshotJson(turn));
            }
            return encoded;
        }

        std::uint64_t saturatedCount(std::size_t value) noexcept {
            if constexpr (sizeof(std::size_t) > sizeof(std::uint64_t)) {
                return value > std::numeric_limits<std::uint64_t>::max() ? std::numeric_limits<std::uint64_t>::max()
                                                                         : static_cast<std::uint64_t>(value);
            }
            return static_cast<std::uint64_t>(value);
        }

        std::uint64_t saturatedAdd(std::uint64_t left, std::uint64_t right) noexcept {
            return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max()
                                                                            : left + right;
        }

        struct BoundedThreadReadResult {
            Json value = Json::object();
            ThreadReadStateEffect effect;
        };

        BoundedThreadReadResult boundedThreadReadResult(const typed::ThreadId& id,
                                                        const std::optional<backend::ThreadSnapshot>& source,
                                                        std::size_t maximumBytes) {
            BoundedThreadReadResult projected;
            projected.effect.sourcePartial = source && !source->fullyLoaded;

            const auto finalize = [&](Json value, std::uint64_t omittedTurns, std::uint64_t omittedItems) {
                projected.effect.responseOmittedTurns = omittedTurns;
                projected.effect.responseOmittedItems = omittedItems;
                projected.effect.responseTruncated = omittedTurns != 0 || omittedItems != 0;
                projected.effect.authority = projected.effect.sourcePartial
                                                 ? ThreadReadStateEffectAuthority::Merge
                                                 : projected.effect.responseTruncated
                                                       ? ThreadReadStateEffectAuthority::MergePreserveCompleteness
                                                       : ThreadReadStateEffectAuthority::Replace;
                const std::optional<Json> effect = encodeThreadReadStateEffect(projected.effect);
                if (!effect) {
                    throw std::logic_error("frontend thread-read state effect could not be encoded");
                }
                value["stateEffect"] = *effect;
                return value;
            };
            const auto fits = [&](const Json& value) { return value.dump().size() <= maximumBytes; };

            if (!source) {
                projected.effect.authority = ThreadReadStateEffectAuthority::Absent;
                const std::optional<Json> effect = encodeThreadReadStateEffect(projected.effect);
                if (!effect) {
                    throw std::logic_error("frontend absent thread-read state effect could not be encoded");
                }
                projected.value = Json{{"threadId", id.value}, {"stateEffect", *effect}};
                if (!fits(projected.value)) {
                    throw std::length_error("frontend command result exceeds outbound capacity");
                }
                return projected;
            }

            Json completeThread = threadSnapshotJson(*source);
            Json complete = finalize(Json{{"thread", completeThread}}, 0, 0);
            if (fits(complete)) {
                projected.value = std::move(complete);
                return projected;
            }
            if (source->turns.empty()) {
                throw std::length_error("frontend command result exceeds outbound capacity");
            }

            // Preserve a deterministic newest suffix. Encode every retained
            // entity once; fit probes account for the already-encoded JSON
            // fragments instead of repeatedly materializing the growing body.
            Json encodedTurns = std::move(completeThread["turns"]);
            Json threadHeader = std::move(completeThread);
            threadHeader["fullyLoaded"] = !projected.effect.sourcePartial;
            threadHeader["turns"] = Json::array();
            std::vector<std::uint64_t> itemPrefix(source->turns.size() + 1, 0);
            std::vector<std::size_t> encodedTurnBytes;
            encodedTurnBytes.reserve(encodedTurns.size());
            for (std::size_t index = 0; index < source->turns.size(); ++index) {
                itemPrefix[index + 1] = saturatedAdd(itemPrefix[index], saturatedCount(source->turns[index].items.size()));
                encodedTurnBytes.push_back(encodedTurns[index].dump().size());
            }
            const auto arrayBytes = [](std::span<const std::size_t> elements) {
                if (elements.empty()) {
                    return std::size_t{2};
                }
                std::size_t bytes = elements.size() + 1;
                for (const std::size_t element : elements) {
                    if (element > std::numeric_limits<std::size_t>::max() - bytes) {
                        return std::numeric_limits<std::size_t>::max();
                    }
                    bytes += element;
                }
                return bytes;
            };
            const auto projectedBytes = [&](const Json& header,
                                            std::span<const std::size_t> elements,
                                            std::uint64_t omittedTurns,
                                            std::uint64_t omittedItems) {
                Json shell = finalize(Json{{"thread", header}}, omittedTurns, omittedItems);
                const std::size_t shellBytes = shell.dump().size();
                const std::size_t bodyBytes = arrayBytes(elements);
                return bodyBytes > std::numeric_limits<std::size_t>::max() - (shellBytes - 2)
                           ? std::numeric_limits<std::size_t>::max()
                           : shellBytes - 2 + bodyBytes;
            };

            std::size_t omittedTurnCount = 1;
            for (; omittedTurnCount <= encodedTurns.size(); ++omittedTurnCount) {
                const std::span<const std::size_t> suffix =
                    std::span<const std::size_t>(encodedTurnBytes).subspan(omittedTurnCount);
                if (projectedBytes(threadHeader,
                                   suffix,
                                   saturatedCount(omittedTurnCount),
                                   itemPrefix[omittedTurnCount]) <= maximumBytes) {
                    break;
                }
            }
            Json bounded;

            // If even the newest complete turn is too large, keep that turn's
            // header and the largest newest item suffix instead of dropping
            // the entire active turn.
            if (omittedTurnCount == encodedTurns.size()) {
                Json emptyThread = threadHeader;
                bounded = finalize(Json{{"thread", std::move(emptyThread)}},
                                   saturatedCount(encodedTurns.size()),
                                   itemPrefix.back());
                Json encodedItems = std::move(encodedTurns.back()["items"]);
                Json turnHeader = std::move(encodedTurns.back());
                turnHeader["items"] = Json::array();
                std::vector<std::size_t> encodedItemBytes;
                encodedItemBytes.reserve(encodedItems.size());
                for (const Json& item : encodedItems) {
                    encodedItemBytes.push_back(item.dump().size());
                }
                std::size_t omittedItemCount = 0;
                for (; omittedItemCount <= encodedItems.size(); ++omittedItemCount) {
                    const std::span<const std::size_t> suffix =
                        std::span<const std::size_t>(encodedItemBytes).subspan(omittedItemCount);
                    const std::size_t itemArrayBytes = arrayBytes(suffix);
                    Json candidateTurnHeader = turnHeader;
                    candidateTurnHeader["items"] = Json::array();
                    const std::size_t turnShellBytes = candidateTurnHeader.dump().size();
                    const std::size_t turnBytes = turnShellBytes - 2 + itemArrayBytes;
                    const std::array<std::size_t, 1> turnElementBytes{turnBytes};
                    if (projectedBytes(threadHeader,
                                       turnElementBytes,
                                       saturatedCount(source->turns.size() - 1),
                                       saturatedAdd(itemPrefix[source->turns.size() - 1],
                                                    saturatedCount(omittedItemCount))) <= maximumBytes) {
                        Json turn = std::move(turnHeader);
                        for (std::size_t index = omittedItemCount; index < encodedItems.size(); ++index) {
                            turn["items"].push_back(std::move(encodedItems[index]));
                        }
                        Json thread = threadHeader;
                        thread["turns"].push_back(std::move(turn));
                        bounded = finalize(Json{{"thread", std::move(thread)}},
                                           saturatedCount(source->turns.size() - 1),
                                           saturatedAdd(itemPrefix[source->turns.size() - 1],
                                                        saturatedCount(omittedItemCount)));
                        break;
                    }
                }
            } else if (omittedTurnCount < encodedTurns.size()) {
                Json thread = threadHeader;
                for (std::size_t index = omittedTurnCount; index < encodedTurns.size(); ++index) {
                    thread["turns"].push_back(std::move(encodedTurns[index]));
                }
                bounded = finalize(Json{{"thread", std::move(thread)}},
                                   saturatedCount(omittedTurnCount),
                                   itemPrefix[omittedTurnCount]);
            }
            if (bounded.empty() || !fits(bounded)) {
                throw std::length_error("frontend command result exceeds outbound capacity");
            }
            projected.value = std::move(bounded);
            return projected;
        }

        const backend::ThreadSnapshot* findThread(const backend::Snapshot& snapshot, std::string_view id) noexcept {
            for (const backend::ThreadSnapshot& thread : snapshot.threads) {
                if (thread.id == id) {
                    return &thread;
                }
            }
            return nullptr;
        }

        const backend::TurnSnapshot*
        findTurn(const backend::Snapshot& snapshot, std::string_view threadId, std::string_view turnId) noexcept {
            const backend::ThreadSnapshot* thread = findThread(snapshot, threadId);
            if (thread == nullptr) {
                return nullptr;
            }
            for (const backend::TurnSnapshot& turn : thread->turns) {
                if (turn.id == turnId) {
                    return &turn;
                }
            }
            return nullptr;
        }

        bool controllerResultValid(generated::MethodId method,
                                   backend::SessionId expectedBackendSession,
                                   const backend::ControllerResult& result) noexcept {
            if (method == generated::MethodId::ControllerAcquire) {
                return result.role == backend::SessionRole::Controller && result.controller == expectedBackendSession;
            }
            if (method == generated::MethodId::ControllerRelease) {
                return result.role == backend::SessionRole::Observer && !result.controller;
            }
            return false;
        }

    } // namespace

    class BackendCoreBridge::State : public std::enable_shared_from_this<BackendCoreBridge::State> {
    public:
        struct SessionRecord {
            FrontendSessionToken token;
            backend::SessionId backendId;
            std::shared_ptr<backend::FrontendSession> backendSession;
            std::map<std::string, CommandToken, std::less<>> pending;
        };

        struct ExternalTopology {
            std::vector<model::SessionState> sessions;
            std::optional<model::SessionIdentity> controller;
        };

        struct DeferredControllerCompletion {
            backend::SequenceNumber requiredThrough;
            std::string key;
            backend::CommandCompletion completion;
        };

        struct DeferredThreadReadCompletion {
            backend::SequenceNumber requiredThrough;
            std::string key;
            backend::CommandCompletion completion;
            std::size_t retainedBytes = 0;
            TimerCancellation cancelDeadline;
        };

        struct DeferredSessionClose {
            backend::SequenceNumber requiredThrough;
            std::string reason;
        };

        State(backend::detail::BackendCoreRuntime& runtime,
              std::size_t maximumResultBytes,
              std::size_t maximumThreadReadResultBytes,
              TimerScheduler timerScheduler)
            : runtime(runtime)
            , maximumResultBytes(maximumResultBytes)
            , maximumThreadReadResultBytes(maximumThreadReadResultBytes)
            , maximumDeferredThreadReadBytes(
                  backend::DefaultMaximumBackendSnapshotBytes
                  * MaximumDeferredThreadReadCompletions)
            , timerScheduler(std::move(timerScheduler)) {
        }

        void bind(ServerCore& configuredCore) noexcept {
            if (coreIdentity == nullptr || coreIdentity == &configuredCore) {
                coreIdentity = &configuredCore;
            }
        }

        void unbind(ServerCore& configuredCore) noexcept {
            if (coreIdentity == &configuredCore) {
                coreLifetime.reset();
                coreIdentity = nullptr;
            }
        }

        void bindLifetime(const std::shared_ptr<ServerCore>& configuredCore) {
            if (!configuredCore || coreIdentity != configuredCore.get()) {
                throw std::logic_error("BackendCoreBridge ServerCore lifetime binding is invalid");
            }
            coreLifetime = configuredCore;
        }

        [[nodiscard]] std::shared_ptr<ServerCore> lockCore() const noexcept {
            return coreLifetime.lock();
        }

        [[nodiscard]] bool bindingCurrent(const std::shared_ptr<ServerCore>& expected) const noexcept {
            const std::shared_ptr<ServerCore> current = lockCore();
            return current && current == expected && coreIdentity == expected.get();
        }

        void start() {
            if (!lockCore() || observer.isOpen()) {
                throw std::logic_error("BackendCoreBridge observer lifecycle is invalid");
            }
            const std::weak_ptr<State> weak = weak_from_this();
            observer =
                runtime.subscribe(backend::BackendObserverCallbacks{[weak](const std::vector<backend::SequencedBackendEvent>& events) {
                                                                        if (const std::shared_ptr<State> self = weak.lock()) {
                                                                            self->onEvents(events);
                                                                        }
                                                                    },
                                                                    [weak](const backend::Snapshot& snapshot) {
                                                                        if (const std::shared_ptr<State> self = weak.lock()) {
                                                                            self->onResynchronize(snapshot);
                                                                        }
                                                                    }});
            if (!observer.isOpen()) {
                throw std::runtime_error("BackendCore rejected the shared frontend observer");
            }
            const backend::Snapshot initial = runtime.snapshot();
            if (!reconcileTopology(initial)) {
                observer.close();
                throw std::runtime_error("BackendCore topology could not be mapped into frontend identity space");
            }
            observerProcessedThrough = initial.sequence;
        }

        void close() noexcept {
            observer.close();
            while (!sessions.empty()) {
                auto current = sessions.begin();
                const backend::SessionId backendId = current->second.backendId;
                std::shared_ptr<backend::FrontendSession> session = std::move(current->second.backendSession);
                ownedBackendSessions.erase(backendId);
                sessions.erase(current);
                if (session) {
                    session->close("frontend BackendCore bridge closed");
                }
            }
            coreLifetime.reset();
            coreIdentity = nullptr;
            ownedBackendSessions.clear();
            retiredBackendSessions.clear();
            externalIdentities.clear();
            observedBackendController.reset();
            itemContentCoveredThrough.clear();
            deferredObserverEvents.clear();
            resynchronizationPendingDuringAdmission = false;
            deferredControllerCompletion.reset();
            for (auto& [identity, deferred] : deferredThreadReadCompletions) {
                static_cast<void>(identity);
                if (deferred.cancelDeadline) {
                    deferred.cancelDeadline();
                }
            }
            deferredThreadReadCompletions.clear();
            deferredThreadReadBytes = 0;
            deferredSessionCloses.clear();
        }

        [[nodiscard]] bool openSession(const FrontendSessionToken& token) {
            const std::shared_ptr<ServerCore> target = lockCore();
            if (!target || sessions.contains(token.session.value())) {
                return false;
            }
            const std::string key = token.session.value();
            const std::weak_ptr<State> weak = weak_from_this();
            backend::FrontendSession session;
            std::shared_ptr<backend::FrontendSession> retainedSession;
            std::optional<backend::SessionId> admittedBackendId;
            struct AdmissionScope {
                State& owner;
                bool active = true;

                explicit AdmissionScope(State& owner)
                    : owner(owner) {
                    ++owner.sessionAdmissionDepth;
                }

                void leave() noexcept {
                    if (active) {
                        active = false;
                        if (owner.sessionAdmissionDepth != 0) {
                            --owner.sessionAdmissionDepth;
                        }
                    }
                }

                ~AdmissionScope() {
                    leave();
                }
            } admission(*this);
            try {
                session = runtime.openSession(backend::FrontendSessionCallbacks{{},
                                                                                {},
                                                                                [weak, key](const backend::CommandCompletion& completion) {
                                                                                    if (const std::shared_ptr<State> self = weak.lock()) {
                                                                                        self->onCompletion(key, completion);
                                                                                    }
                                                                                },
                                                                                [weak, key](const std::string& reason) {
                                                                                    if (const std::shared_ptr<State> self = weak.lock()) {
                                                                                        self->onSessionClosed(key, reason);
                                                                                    }
                                                                                }});
                if (!session.isOpen()) {
                    admission.leave();
                    finishSessionAdmission(*target);
                    return false;
                }
                const backend::SessionId backendId = session.id();
                admittedBackendId = backendId;
                ownedBackendSessions.insert_or_assign(backendId, key);
                if (!bindingCurrent(target)) {
                    ownedBackendSessions.erase(backendId);
                    retiredBackendSessions.insert(backendId);
                    session.close("frontend service closed during backend session admission");
                    admission.leave();
                    deferredObserverEvents.clear();
                    resynchronizationPendingDuringAdmission = false;
                    return false;
                }
                retainedSession = std::make_shared<backend::FrontendSession>(std::move(session));
                const bool inserted = sessions.emplace(key, SessionRecord{token, backendId, retainedSession, {}}).second;
                if (!inserted) {
                    ownedBackendSessions.erase(backendId);
                    retiredBackendSessions.insert(backendId);
                    retainedSession->close("duplicate frontend backend session admission");
                    admission.leave();
                    finishSessionAdmission(*target);
                    return false;
                }
                admission.leave();
                finishSessionAdmission(*target);
                return true;
            } catch (...) {
                if (admittedBackendId) {
                    ownedBackendSessions.erase(*admittedBackendId);
                    try {
                        retiredBackendSessions.insert(*admittedBackendId);
                    } catch (...) {
                    }
                }
                if (retainedSession) {
                    retainedSession->close("frontend backend session admission failed");
                } else if (session.isOpen()) {
                    session.close("frontend backend session admission failed");
                }
                // Every event accumulated while admission was ambiguous is
                // covered by the authoritative current snapshot below. Do not
                // risk classifying a partially admitted backend ID as external.
                deferredObserverEvents.clear();
                resynchronizationPendingDuringAdmission = false;
                admission.leave();
                if (bindingCurrent(target)) {
                    recoverCurrentSnapshot(*target);
                }
                return false;
            }
        }

        std::size_t deferredThreadReadBytesFor(const backend::CommandCompletion& completion) const noexcept {
            // BackendCore already admits every immutable capture against its
            // snapshot budget. Charge that structural upper bound directly;
            // exact JSON sizing would encode and allocate the complete thread
            // a second time on the ordering-deferral path.
            static_cast<void>(completion);
            return backend::DefaultMaximumBackendSnapshotBytes;
        }

        void releaseDeferredThreadReadBytes(std::size_t bytes) noexcept {
            deferredThreadReadBytes = bytes >= deferredThreadReadBytes ? 0 : deferredThreadReadBytes - bytes;
        }

        void eraseDeferredThreadReadsForSession(std::string_view key) noexcept {
            for (auto deferred = deferredThreadReadCompletions.begin(); deferred != deferredThreadReadCompletions.end();) {
                if (deferred->first.first != key) {
                    ++deferred;
                    continue;
                }
                if (deferred->second.cancelDeadline) {
                    deferred->second.cancelDeadline();
                }
                releaseDeferredThreadReadBytes(deferred->second.retainedBytes);
                deferred = deferredThreadReadCompletions.erase(deferred);
            }
        }

        void expireDeferredThreadRead(const std::pair<std::string, std::string>& identity) noexcept {
            const auto deferred = deferredThreadReadCompletions.find(identity);
            if (deferred == deferredThreadReadCompletions.end()) {
                return;
            }
            const std::string key = deferred->second.key;
            const std::string requestId = deferred->second.completion.requestId;
            releaseDeferredThreadReadBytes(deferred->second.retainedBytes);
            deferredThreadReadCompletions.erase(deferred);
            valueFailure(key,
                         requestId,
                         ErrorCode::Conflict,
                         "thread-read state effect timed out waiting for observer synchronization");
        }

        void closeSession(const FrontendSessionToken& token) noexcept {
            const auto found = sessions.find(token.session.value());
            if (found == sessions.end() || found->second.token != token) {
                return;
            }
            if (deferredControllerCompletion && deferredControllerCompletion->key == token.session.value()) {
                deferredControllerCompletion.reset();
            }
            eraseDeferredThreadReadsForSession(token.session.value());
            std::shared_ptr<backend::FrontendSession> session = std::move(found->second.backendSession);
            sessions.erase(found);
            if (session) {
                // Retain the already-allocated ownership entry until the
                // disconnect echo is consumed. This keeps the noexcept close
                // path allocation-free and prevents local backend IDs from
                // ever being reclassified as external.
                session->close("frontend session closed");
            }
        }

        [[nodiscard]] BackendSubmitStatus submit(BackendInvocation invocation) {
            const auto found = sessions.find(invocation.session.value());
            if (found == sessions.end() || found->second.token.connection != invocation.token.connection ||
                found->second.token.connectionGeneration != invocation.token.connectionGeneration || !found->second.backendSession ||
                !found->second.backendSession->isOpen()) {
                return BackendSubmitStatus::Unavailable;
            }

            backend::BackendCommand command;
            if (invocation.token.method == generated::MethodId::ControllerAcquire) {
                command = backend::ControllerAcquire{};
            } else if (invocation.token.method == generated::MethodId::ControllerRelease) {
                command = backend::ControllerRelease{};
            } else {
                detail::DefinedCommandMapping mapping = detail::mapDefinedCommand(invocation.command);
                backend::BackendCommand* mapped = std::get_if<backend::BackendCommand>(&mapping);
                if (mapped == nullptr) {
                    return BackendSubmitStatus::Rejected;
                }
                command = std::move(*mapped);
            }

            SessionRecord& record = found->second;
            const bool inserted = record.pending.emplace(invocation.token.requestId, invocation.token).second;
            if (!inserted) {
                return BackendSubmitStatus::Rejected;
            }
            const std::shared_ptr<backend::FrontendSession> submittingSession = record.backendSession;
            backend::CommandSubmission submission = submittingSession->submit(invocation.token.requestId, std::move(command));
            if (submission) {
                return BackendSubmitStatus::Accepted;
            }
            // A synchronous completion is authoritative and has already
            // removed this token. The callback may also have erased the whole
            // session, so revalidate instead of retaining SessionRecord&.
            const auto afterSubmit = sessions.find(invocation.session.value());
            if (afterSubmit == sessions.end() || afterSubmit->second.token.connection != invocation.token.connection ||
                afterSubmit->second.token.connectionGeneration != invocation.token.connectionGeneration ||
                !afterSubmit->second.pending.contains(invocation.token.requestId)) {
                return BackendSubmitStatus::Accepted;
            }
            afterSubmit->second.pending.erase(invocation.token.requestId);
            switch (submission.error) {
                case backend::SubmissionError::None:
                case backend::SubmissionError::Closed:
                    return BackendSubmitStatus::Unavailable;
                case backend::SubmissionError::QueueFull:
                    return BackendSubmitStatus::CapacityExceeded;
                case backend::SubmissionError::EmptyRequestId:
                case backend::SubmissionError::DuplicateRequestId:
                    return BackendSubmitStatus::Rejected;
            }
            return BackendSubmitStatus::Rejected;
        }

        void onCompletion(const std::string& key, const backend::CommandCompletion& completion) noexcept {
            const auto session = sessions.find(key);
            if (session == sessions.end()) {
                return;
            }
            const auto pending = session->second.pending.find(completion.requestId);
            if (pending == session->second.pending.end()) {
                return;
            }
            if (pending->second.method == generated::MethodId::ControllerAcquire ||
                pending->second.method == generated::MethodId::ControllerRelease) {
                // BackendCore drains observer and command callbacks
                // independently. A completion may overtake the corresponding
                // ControllerChanged batch, so wait for the one shared observer
                // to observe the typed result's controller value.
                const std::optional<backend::Snapshot> current = currentSnapshotNoThrow();
                if (!current) {
                    valueFailure(key,
                                 completion.requestId,
                                 ErrorCode::InternalError,
                                 "backend controller completion ordering could not be verified");
                    return;
                }
                if (observerProcessedThrough.value() < current->sequence.value()) {
                    if (deferredControllerCompletion) {
                        valueFailure(key,
                                     completion.requestId,
                                     ErrorCode::InternalError,
                                     "backend controller completion ordering capacity was exceeded");
                        return;
                    }
                    try {
                        deferredControllerCompletion = DeferredControllerCompletion{current->sequence, key, completion};
                    } catch (...) {
                        valueFailure(
                            key, completion.requestId, ErrorCode::InternalError, "backend controller completion could not be retained");
                    }
                    return;
                }
            }
            const bool fullThreadRead =
                pending->second.method == generated::MethodId::ThreadRead && pending->second.threadReadIncludesTurns;
            const bool successfulFullRead =
                !completion.result.error && std::holds_alternative<typed::ThreadReadResponse>(completion.result.value);
            const bool absentFullRead = completion.threadReadSnapshot &&
                                        authoritativeThreadReadAbsence(completion, &*completion.threadReadSnapshot);
            if (fullThreadRead && (successfulFullRead || absentFullRead)) {
                attemptThreadReadCompletion(key, completion);
                return;
            }
            finishCompletion(key, completion, nullptr);
        }

        void attemptThreadReadCompletion(const std::string& key, const backend::CommandCompletion& completion) noexcept {
            const auto session = sessions.find(key);
            if (session == sessions.end()) {
                return;
            }
            const auto pending = session->second.pending.find(completion.requestId);
            if (pending == session->second.pending.end()) {
                return;
            }
            if (!completion.threadReadSnapshot) {
                valueFailure(key,
                             completion.requestId,
                             ErrorCode::InternalError,
                             "backend thread-read completion lacks its requester-local state capture");
                return;
            }
            const backend::ThreadSnapshotAtSequence& captured = *completion.threadReadSnapshot;
            const bool absent = authoritativeThreadReadAbsence(completion, &captured);
            if (pending->second.threadReadStateEffectVersion != 1 && absent) {
                // Only negotiated reads may reinterpret the provider's
                // authoritative NotFound as a State mutation. Legacy reads
                // retain the NotFound command failure without an observer
                // fence.
                finishCompletion(key, completion, &captured);
                return;
            }
            if (!pending->second.threadReadTarget) {
                valueFailure(key,
                             completion.requestId,
                             ErrorCode::InternalError,
                             "backend thread-read completion lacks its requested target");
                return;
            }
            if (!absent) {
                const auto* response = std::get_if<typed::ThreadReadResponse>(&completion.result.value);
                if (completion.result.error || response == nullptr) {
                    valueFailure(key,
                                 completion.requestId,
                                 ErrorCode::InternalError,
                                 "backend thread-read completion lacks its requester-local result");
                    return;
                }
                if (response->thread.id.value != *pending->second.threadReadTarget) {
                    valueFailure(key,
                                 completion.requestId,
                                 ErrorCode::TypedDecodingFailure,
                                 "backend thread-read completion targets a different thread");
                    return;
                }
                if (!captured.thread) {
                    valueFailure(key,
                                 completion.requestId,
                                 ErrorCode::InternalError,
                                 "backend thread-read completion lacks its requester-local state capture");
                    return;
                }
                if (captured.thread->id != response->thread.id.value) {
                    valueFailure(key,
                                 completion.requestId,
                                 ErrorCode::TypedDecodingFailure,
                                 "backend thread-read state capture targets a different thread");
                    return;
                }
            }
            if (pending->second.threadReadStateEffectVersion != 1) {
                // A successful legacy full read is requester-local raw result
                // data and never mutates synchronized State.
                finishCompletion(key, completion, &captured);
                return;
            }
            if (observerProcessedThrough.value() > captured.sequence.value()) {
                // A separately drained observer callback can overtake the
                // command completion after BackendCore captures the private
                // read body. Applying that older body after the already-
                // published suffix could roll the requester's thread State
                // backwards.
                valueFailure(key,
                             completion.requestId,
                             ErrorCode::Conflict,
                             "thread-read state effect was superseded by observer synchronization");
                return;
            }
            if (observerProcessedThrough.value() < captured.sequence.value()) {
                const std::size_t retainedBytes = deferredThreadReadBytesFor(completion);
                if (deferredThreadReadCompletions.size() >= MaximumDeferredThreadReadCompletions ||
                    retainedBytes > maximumDeferredThreadReadBytes ||
                    deferredThreadReadBytes > maximumDeferredThreadReadBytes - retainedBytes) {
                    valueFailure(key,
                                 completion.requestId,
                                 ErrorCode::CapacityExceeded,
                                 "backend thread-read completion ordering capacity was exceeded");
                    return;
                }
                try {
                    const auto identity = std::pair{key, completion.requestId};
                    const auto [entry, inserted] = deferredThreadReadCompletions.emplace(
                        identity, DeferredThreadReadCompletion{captured.sequence, key, completion, retainedBytes, {}});
                    if (!inserted) {
                        valueFailure(key,
                                     completion.requestId,
                                     ErrorCode::InternalError,
                                     "backend thread-read completion ordering identity was duplicated");
                        return;
                    }
                    deferredThreadReadBytes += retainedBytes;
                    if (!timerScheduler) {
                        expireDeferredThreadRead(identity);
                        return;
                    }
                    const std::weak_ptr<State> weak = weak_from_this();
                    TimerCancellation cancellation = timerScheduler(DeferredThreadReadDeadlineMs, [weak, identity] {
                        if (const std::shared_ptr<State> self = weak.lock()) {
                            self->expireDeferredThreadRead(identity);
                        }
                    });
                    const auto retained = deferredThreadReadCompletions.find(identity);
                    if (retained != deferredThreadReadCompletions.end()) {
                        retained->second.cancelDeadline = std::move(cancellation);
                    } else if (cancellation) {
                        cancellation();
                    }
                } catch (...) {
                    valueFailure(key,
                                 completion.requestId,
                                 ErrorCode::InternalError,
                                 "backend thread-read completion ordering could not be retained");
                }
                return;
            }
            finishCompletion(key, completion, &captured);
        }

        void finishCompletion(const std::string& key,
                              const backend::CommandCompletion& completion,
                              const backend::ThreadSnapshotAtSequence* capturedThreadRead) noexcept {
            const auto found = sessions.find(key);
            if (found == sessions.end()) {
                return;
            }
            const auto pending = found->second.pending.find(completion.requestId);
            if (pending == found->second.pending.end()) {
                return;
            }
            CommandToken token = pending->second;
            const bool fullThreadRead = token.method == generated::MethodId::ThreadRead && token.threadReadIncludesTurns;
            found->second.pending.erase(pending);
            const std::shared_ptr<ServerCore> target = lockCore();
            if (!target) {
                return;
            }
            BackendCompletionValue value =
                BackendCommandFailure{ErrorCode::InternalError, "failed to normalize backend command completion", std::nullopt};
            try {
                const bool negotiatedAbsence = fullThreadRead && token.threadReadStateEffectVersion == 1 &&
                                               authoritativeThreadReadAbsence(completion, capturedThreadRead);
                if (completion.result.error && !negotiatedAbsence) {
                    value = BackendCommandFailure{frontendErrorCode(completion.result.error->code),
                                                  frontendErrorMessage(completion.result.error->code),
                                                  std::nullopt};
                } else {
                    Json projected = resultJson(token,
                                                completion.result.value,
                                                token.threadReadStateEffectVersion == 1,
                                                capturedThreadRead);
                    value = BackendCommandSuccess{generated::makeResult(token.method, std::move(projected))};
                }
            } catch (const std::length_error&) {
                value = BackendCommandFailure{
                    ErrorCode::CapacityExceeded, "frontend command result exceeds the configured outbound capacity", std::nullopt};
            } catch (const std::logic_error&) {
                value = BackendCommandFailure{
                    ErrorCode::TypedDecodingFailure, "backend completion violates the generated frontend result authority", std::nullopt};
            } catch (...) {
            }
            if (bindingCurrent(target)) {
                const bool negotiatedStateEffect = token.threadReadStateEffectVersion == 1;
                BackendCompletion normalized{std::move(token), std::move(value)};
                if (fullThreadRead && negotiatedStateEffect &&
                    std::holds_alternative<BackendCommandSuccess>(normalized.value)) {
                    static_cast<void>(target->completeThreadRead(std::move(normalized)));
                } else {
                    static_cast<void>(target->complete(std::move(normalized)));
                }
            }
        }

        void valueFailure(const std::string& key, const std::string& requestId, ErrorCode code, std::string message) noexcept {
            const auto found = sessions.find(key);
            if (found == sessions.end()) {
                return;
            }
            const auto pending = found->second.pending.find(requestId);
            if (pending == found->second.pending.end()) {
                return;
            }
            CommandToken token = pending->second;
            found->second.pending.erase(pending);
            if (const std::shared_ptr<ServerCore> target = lockCore(); bindingCurrent(target)) {
                static_cast<void>(
                    target->complete(BackendCompletion{std::move(token), BackendCommandFailure{code, std::move(message), std::nullopt}}));
            }
        }

        [[nodiscard]] Json resultJson(const CommandToken& token,
                                      const backend::CommandValue& value,
                                      bool threadReadStateEffect,
                                      const backend::ThreadSnapshotAtSequence* capturedThreadRead) const {
            if (token.method == generated::MethodId::ThreadRead && token.threadReadIncludesTurns) {
                if (!capturedThreadRead) {
                    throw std::logic_error("backend thread-read result lacks its ordered state capture");
                }
                if (threadReadStateEffect && !capturedThreadRead->thread) {
                    if (!token.threadReadTarget) {
                        throw std::logic_error("backend absent thread-read result lacks its requested target");
                    }
                    return boundedThreadReadResult(
                               typed::ThreadId{*token.threadReadTarget}, std::nullopt, maximumThreadReadResultBytes)
                        .value;
                }
                const auto* response = std::get_if<typed::ThreadReadResponse>(&value);
                if (response == nullptr || !capturedThreadRead->thread) {
                    throw std::logic_error("backend thread-read result lacks its ordered state capture");
                }
                if (threadReadStateEffect) {
                    return boundedThreadReadResult(response->thread.id,
                                                   capturedThreadRead->thread,
                                                   maximumThreadReadResultBytes)
                        .value;
                }
                Json legacy = capturedThreadRead->thread ? Json{{"thread", threadSnapshotJson(*capturedThreadRead->thread)}}
                                                         : Json{{"threadId", response->thread.id.value}};
                if (legacy.dump().size() > maximumThreadReadResultBytes) {
                    throw std::length_error("frontend command result exceeds outbound capacity");
                }
                return legacy;
            }
            const detail::ProviderResultProjection projected = detail::projectProviderResult(token.method, value, maximumResultBytes);
            switch (projected.status) {
                case detail::ProviderResultProjectionStatus::Success:
                    return projected.value;
                case detail::ProviderResultProjectionStatus::ResultTooLarge:
                    throw std::length_error("frontend command result exceeds outbound capacity");
                case detail::ProviderResultProjectionStatus::LegacyProjectionRequired:
                case detail::ProviderResultProjectionStatus::NotProviderResult:
                    break;
                case detail::ProviderResultProjectionStatus::ResultTypeMismatch:
                case detail::ProviderResultProjectionStatus::InvalidResult:
                    throw std::logic_error("backend result violates generated frontend result authority");
            }

            Json result = Json::object();
            std::optional<backend::Snapshot> capturedSnapshot;
            const auto currentSnapshot = [&]() -> const backend::Snapshot& {
                if (!capturedSnapshot) {
                    capturedSnapshot.emplace(runtime.snapshot());
                }
                return *capturedSnapshot;
            };
            if (const auto* controller = std::get_if<backend::ControllerResult>(&value)) {
                // BackendCore SessionId is deliberately never projected. The
                // wire identity is the semantic ServerCore session identity.
                const auto found = sessions.find(tokenSession(token));
                if (found == sessions.end() || !controllerResultValid(token.method, found->second.backendId, *controller)) {
                    throw std::logic_error("backend controller completion violates the controller transaction");
                }
                if (token.method == generated::MethodId::ControllerAcquire) {
                    result = Json{{"controllerSessionId", found->second.token.session.value()}, {"role", "controller"}};
                } else {
                    result = Json{{"role", "observer"}};
                }
            } else if (std::holds_alternative<std::monostate>(value) || std::holds_alternative<typed::Unit>(value)) {
                result = Json::object();
            } else if (const auto* response = std::get_if<typed::ThreadStartResponse>(&value)) {
                const backend::ThreadSnapshot* thread = findThread(currentSnapshot(), response->thread.id.value);
                result = thread ? Json{{"thread", threadSnapshotJson(*thread)}} : Json{{"threadId", response->thread.id.value}};
            } else if (const auto* response = std::get_if<typed::ThreadResumeResponse>(&value)) {
                const backend::ThreadSnapshot* thread = findThread(currentSnapshot(), response->thread.id.value);
                result = thread ? Json{{"thread", threadSnapshotJson(*thread)}} : Json{{"threadId", response->thread.id.value}};
            } else if (const auto* response = std::get_if<typed::ThreadReadResponse>(&value)) {
                const backend::ThreadSnapshot* thread = findThread(currentSnapshot(), response->thread.id.value);
                result = thread ? Json{{"thread", threadSnapshotJson(*thread)}} : Json{{"threadId", response->thread.id.value}};
            } else if (const auto* response = std::get_if<typed::ThreadListResponse>(&value)) {
                result = Json{{"threads", Json::array()}};
                for (const typed::Thread& item : response->data) {
                    const backend::ThreadSnapshot* thread = findThread(currentSnapshot(), item.id.value);
                    result["threads"].push_back(thread ? threadSnapshotJson(*thread) : Json{{"id", item.id.value}});
                }
                if (response->nextCursor) {
                    result["nextCursor"] = *response->nextCursor;
                }
                if (response->backwardsCursor) {
                    result["backwardsCursor"] = *response->backwardsCursor;
                }
            } else if (const auto* response = std::get_if<typed::TurnStartResponse>(&value)) {
                const backend::TurnSnapshot* turn = findTurn(currentSnapshot(), response->turn.threadId.value, response->turn.id.value);
                result = turn ? Json{{"turn", turnSnapshotJson(*turn)}} : Json{{"turnId", response->turn.id.value}};
            } else {
                throw std::logic_error("backend result lacks a frontend result projection");
            }
            if (result.dump().size() > maximumResultBytes) {
                throw std::length_error("frontend command result exceeds outbound capacity");
            }
            return result;
        }

        [[nodiscard]] std::string tokenSession(const CommandToken& token) const {
            for (const auto& [key, record] : sessions) {
                if (record.token.connection == token.connection && record.token.connectionGeneration == token.connectionGeneration) {
                    return key;
                }
            }
            return {};
        }

        void onSessionClosed(const std::string& key, const std::string& reason) noexcept {
            const auto found = sessions.find(key);
            if (found == sessions.end()) {
                return;
            }
            const backend::SessionId backendId = found->second.backendId;
            const std::optional<backend::Snapshot> current = currentSnapshotNoThrow();
            if (!current) {
                if (const std::shared_ptr<ServerCore> target = lockCore()) {
                    static_cast<void>(target->requireSnapshot(OccurrenceFlushUrgency::Immediate));
                }
                finishSessionClosed(key, reason);
                return;
            }
            // BackendCore publishes ControllerChanged/SessionChanged before it
            // schedules onClosed. Independently drained queues can still run
            // this callback first, so retain it through the authoritative
            // Snapshot sequence captured here. That includes a controller
            // handoff queued after SessionChanged(false), not merely the
            // disconnect echo itself.
            if (backendId && observerProcessedThrough.value() < current->sequence.value()) {
                try {
                    deferredSessionCloses.insert_or_assign(key, DeferredSessionClose{current->sequence, reason});
                } catch (...) {
                    if (const std::shared_ptr<ServerCore> target = lockCore()) {
                        static_cast<void>(target->requireSnapshot(OccurrenceFlushUrgency::Immediate));
                    }
                    finishSessionClosed(key, reason);
                }
                return;
            }
            finishSessionClosed(key, reason);
        }

        void finishSessionClosed(const std::string& key, const std::string& reason) noexcept {
            const auto found = sessions.find(key);
            if (found == sessions.end()) {
                return;
            }
            const FrontendSessionToken token = found->second.token;
            std::optional<backend::SessionId> backendId;
            if (found->second.backendId) {
                backendId = found->second.backendId;
                ownedBackendSessions.erase(*backendId);
                try {
                    retiredBackendSessions.insert(*backendId);
                } catch (...) {
                }
            }
            sessions.erase(found);
            if (deferredControllerCompletion && deferredControllerCompletion->key == key) {
                deferredControllerCompletion.reset();
            }
            eraseDeferredThreadReadsForSession(key);
            deferredSessionCloses.erase(key);
            if (const std::shared_ptr<ServerCore> target = lockCore()) {
                // The tombstone is the ordering fence: any BackendCore
                // controller handoff preceding this close has already updated
                // ServerCore, so closing the semantic frontend session cannot
                // erase or relabel an unrelated external owner.
                target->closeConnection(token.connection, reason.empty() ? "backend frontend session closed" : reason);
            }
            if (backendId) {
                retiredBackendSessions.erase(*backendId);
            }
        }

        void onEvents(const std::vector<backend::SequencedBackendEvent>& events) noexcept {
            if (sessionAdmissionDepth != 0) {
                try {
                    deferredObserverEvents.insert(deferredObserverEvents.end(), events.begin(), events.end());
                } catch (...) {
                    deferredObserverEvents.clear();
                    // Admission has not classified the BackendCore SessionId
                    // yet. Recover from the authoritative post-admission
                    // snapshot instead of allowing a partial suffix to be
                    // interpreted with ambiguous ownership.
                    resynchronizationPendingDuringAdmission = true;
                }
                return;
            }
            processEvents(events);
        }

        void processEvents(std::span<const backend::SequencedBackendEvent> events) noexcept {
            std::size_t begin = 0;
            while (begin < events.size()) {
                drainDeferredThreadReadCompletions();
                std::optional<backend::SequenceNumber> fence;
                for (const auto& [identity, deferred] : deferredThreadReadCompletions) {
                    static_cast<void>(identity);
                    if (deferred.requiredThrough.value() <= observerProcessedThrough.value() ||
                        deferred.requiredThrough.value() > events.back().sequence.value()) {
                        continue;
                    }
                    if (!fence || deferred.requiredThrough < *fence) {
                        fence = deferred.requiredThrough;
                    }
                }

                std::size_t end = events.size();
                if (fence) {
                    end = begin;
                    while (end < events.size() && events[end].sequence.value() <= fence->value()) {
                        ++end;
                    }
                    if (end == begin) {
                        // Observing a later backend sequence proves that an
                        // empty sequence gap has been crossed.  Complete at
                        // the retained fence before staging the suffix.
                        observerProcessedThrough = *fence;
                        drainDeferredThreadReadCompletions();
                        continue;
                    }
                }
                processEventSegment(events.subspan(begin, end - begin), fence.has_value());
                begin = end;
            }
        }

        void processEventSegment(std::span<const backend::SequencedBackendEvent> events,
                                 bool boundedByThreadReadFence) noexcept {
            std::shared_ptr<ServerCore> target = lockCore();
            if (!target) {
                return;
            }
            try {
                std::vector<backend::SequencedBackendEvent> projectedEvents;
                projectedEvents.reserve(events.size());
                std::optional<backend::SequenceNumber> processedThrough;
                enum class ProjectedFlushResult { Staged, SnapshotPublished, Failed };
                const auto flushProjectedEvents = [&]() -> ProjectedFlushResult {
                    if (projectedEvents.empty()) {
                        return ProjectedFlushResult::Staged;
                    }
                    const backend::SequenceNumber projectionThrough = projectedEvents.back().sequence;
                    const auto publishCurrentResynchronization = [&](const backend::Snapshot& current,
                                                                     std::optional<backend::SequenceNumber> minimum = std::nullopt) {
                        projectedEvents.clear();
                        if ((minimum && current.sequence < *minimum) || !applyResynchronization(current, *target)) {
                            return ProjectedFlushResult::Failed;
                        }
                        if (current.sequence.value() > observerProcessedThrough.value()) {
                            observerProcessedThrough = current.sequence;
                        }
                        drainDeferredThreadReadsAfterResynchronization(current.sequence);
                        drainDeferredControllerCompletion();
                        drainDeferredThreadReadCompletions();
                        drainDeferredSessionCloses();
                        return ProjectedFlushResult::SnapshotPublished;
                    };
                    std::optional<ProjectedBackendBatch> projectedBatch;
                    std::optional<backend::SequenceNumber> contentCoverageThrough;
                    const std::optional<std::vector<backend::SequencedBackendEvent>> contentEvents =
                        coalescedItemContentEvents(projectedEvents);
                    std::vector<backend::ItemContentSnapshotKey> contentKeys;
                    contentKeys.reserve(contentEvents ? contentEvents->size() : 0);
                    if (contentEvents) {
                        for (const backend::SequencedBackendEvent& sequenced : *contentEvents) {
                            const auto& content = std::get<backend::ItemContentChanged>(sequenced.event);
                            contentKeys.push_back(contentSnapshotKey(content));
                        }
                        std::optional<backend::ItemContentSnapshotBatch> contentItems = runtime.itemContentSnapshots(contentKeys);
                        if (contentItems && !contentEvents->empty()) {
                            const backend::SequenceNumber eventSequence = contentEvents->back().sequence;
                            const bool snapshotAhead = itemContentSnapshotIsAhead(eventSequence, contentItems->sequence);
                            if (snapshotAhead && boundedByThreadReadFence) {
                                // The exact-item helper is live state, not an
                                // historical view. Even an active item can
                                // already contain a same-entity suffix beyond
                                // this read's fence, so no payload from it may
                                // precede the older command body.
                                const backend::Snapshot current = runtime.snapshot();
                                return publishCurrentResynchronization(current, contentItems->sequence);
                            }
                            if (contentItems->sequence == eventSequence || snapshotAhead) {
                                // A live exact-item view may already include a
                                // later observer drain. Publish that channel as
                                // one authoritative occurrence and remember its
                                // per-key coverage; never advance the unrelated
                                // global observer fence to avoid a full rebase.
                                model::ModelResult<ProjectedBackendBatch> direct = projection.projectItemContentOccurrences(
                                    *contentEvents, contentItems->items, snapshotAhead && !boundedByThreadReadFence);
                                if (direct && !direct.value().snapshotRequired) {
                                    projectedBatch = std::move(direct).value();
                                    if (snapshotAhead && !boundedByThreadReadFence) {
                                        contentCoverageThrough = contentItems->sequence;
                                    }
                                } else if (snapshotAhead && !boundedByThreadReadFence) {
                                    const backend::Snapshot current = runtime.snapshot();
                                    return publishCurrentResynchronization(current, contentItems->sequence);
                                }
                            }
                        }
                    }

                    std::optional<backend::Snapshot> snapshot;
                    if (!projectedBatch) {
                        snapshot = runtime.snapshot();
                        if (boundedByThreadReadFence && snapshot->sequence > projectionThrough) {
                            // projectOccurrences selects family payloads from
                            // this live snapshot. A later snapshot would leak
                            // suffix values into the bounded prefix for any
                            // family, not just item content. Publish the later
                            // authoritative Snapshot and supersede the older
                            // command-local effect instead.
                            return publishCurrentResynchronization(*snapshot);
                        }
                        model::ModelResult<ProjectedBackendBatch> projected =
                            projection.projectOccurrences(projectedEvents, *snapshot);
                        if (!projected) {
                            projectedEvents.clear();
                            return ProjectedFlushResult::Failed;
                        }
                        projectedBatch = std::move(projected).value();
                    }
                    projectedEvents.clear();
                    if (!bindingCurrent(target)) {
                        return ProjectedFlushResult::Failed;
                    }
                    ProjectedBackendBatch batch = std::move(*projectedBatch);
                    if (batch.snapshotRequired) {
                        if (!snapshot || !publishProjectedResynchronization(*snapshot, std::move(batch.snapshot), *target)) {
                            return ProjectedFlushResult::Failed;
                        }
                        if (snapshot->sequence.value() > observerProcessedThrough.value()) {
                            observerProcessedThrough = snapshot->sequence;
                        }
                        drainDeferredThreadReadsAfterResynchronization(snapshot->sequence);
                        drainDeferredControllerCompletion();
                        drainDeferredThreadReadCompletions();
                        drainDeferredSessionCloses();
                        return ProjectedFlushResult::SnapshotPublished;
                    }
                    std::vector<OccurrenceStageRequest> groups;
                    groups.reserve(batch.occurrences.size());
                    for (ProjectedBackendOccurrence& occurrence : batch.occurrences) {
                        groups.push_back({std::move(occurrence.key), std::move(occurrence.occurrence), occurrence.urgency});
                    }
                    if (!target->stageGroups(std::move(groups)).accepted()) {
                        return ProjectedFlushResult::Failed;
                    }
                    if (contentCoverageThrough && contentEvents) {
                        retainItemContentCoverage(*contentEvents, *contentCoverageThrough);
                    }
                    return ProjectedFlushResult::Staged;
                };
                for (const backend::SequencedBackendEvent& event : events) {
                    if (event.sequence.value() <= observerProcessedThrough.value()) {
                        continue;
                    }
                    processedThrough = event.sequence;
                    // An ahead exact-item occurrence already represented only
                    // this composite item/channel through its watermark.
                    if (itemContentEventCovered(event)) {
                        continue;
                    }
                    if (const auto* changed = std::get_if<backend::SessionChanged>(&event.event)) {
                        const ProjectedFlushResult flushed = flushProjectedEvents();
                        if (flushed == ProjectedFlushResult::Failed) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        if (flushed == ProjectedFlushResult::SnapshotPublished) {
                            return;
                        }
                        if (const auto owned = ownedBackendSessions.find(changed->id); owned != ownedBackendSessions.end()) {
                            if (!changed->connected) {
                                if (sessions.contains(owned->second)) {
                                    retiredBackendSessions.insert(changed->id);
                                } else {
                                    ownedBackendSessions.erase(owned);
                                }
                            }
                            continue;
                        }
                        if (retiredBackendSessions.contains(changed->id)) {
                            if (!changed->connected) {
                                // This path belongs to a locally initiated
                                // close: the semantic session has already been
                                // removed and this is the final backend echo.
                                retiredBackendSessions.erase(changed->id);
                            }
                            continue;
                        }
                        if (!changed->connected && !externalIdentities.contains(changed->id)) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        const std::optional<model::SessionIdentity> identity = externalIdentity(changed->id, *target);
                        if (!identity) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        model::SessionState session(*identity);
                        session.role = changed->role == backend::SessionRole::Controller ? SessionRole::Controller : SessionRole::Observer;
                        if (!target->externalSessionChanged(std::move(session), changed->connected)) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        if (!changed->connected) {
                            externalIdentities.erase(changed->id);
                        }
                        continue;
                    }
                    if (const auto* changed = std::get_if<backend::ControllerChanged>(&event.event)) {
                        const ProjectedFlushResult flushed = flushProjectedEvents();
                        if (flushed == ProjectedFlushResult::Failed) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        if (flushed == ProjectedFlushResult::SnapshotPublished) {
                            return;
                        }
                        const std::optional<backend::SessionId> previous = observedBackendController;
                        observedBackendController = changed->controller;
                        if (changed->controller && ownedBackendSessions.contains(*changed->controller)) {
                            continue;
                        }
                        if (changed->controller && retiredBackendSessions.contains(*changed->controller)) {
                            continue;
                        }
                        if (!changed->controller && previous &&
                            (ownedBackendSessions.contains(*previous) || retiredBackendSessions.contains(*previous))) {
                            continue;
                        }
                        std::optional<model::SessionIdentity> identity;
                        if (changed->controller) {
                            identity = externalIdentity(*changed->controller, *target);
                            if (!identity) {
                                recoverCurrentSnapshot(*target);
                                return;
                            }
                        }
                        if (!target->externalControllerChanged(std::move(identity))) {
                            recoverCurrentSnapshot(*target);
                            return;
                        }
                        continue;
                    }
                    projectedEvents.push_back(event);
                }
                const ProjectedFlushResult flushed = flushProjectedEvents();
                if (flushed == ProjectedFlushResult::Failed) {
                    recoverCurrentSnapshot(*target);
                    return;
                }
                if (flushed == ProjectedFlushResult::SnapshotPublished) {
                    return;
                }
                if (processedThrough && processedThrough->value() > observerProcessedThrough.value()) {
                    observerProcessedThrough = *processedThrough;
                }
                pruneItemContentCoverage();
                drainDeferredControllerCompletion();
                drainDeferredThreadReadCompletions();
                drainDeferredSessionCloses();
            } catch (...) {
                if (const std::shared_ptr<ServerCore> current = lockCore()) {
                    recoverCurrentSnapshot(*current);
                }
            }
        }

        void onResynchronize(const backend::Snapshot& backendSnapshot) noexcept {
            if (sessionAdmissionDepth != 0) {
                // A bounded observer overflow can resynchronize synchronously
                // from BackendCore::openSession(), before the returned private
                // SessionId has been classified. Defer only the fact; one
                // authoritative current Snapshot after admission covers the
                // entire suffix without retaining an allocating Snapshot here.
                resynchronizationPendingDuringAdmission = true;
                return;
            }
            const std::shared_ptr<ServerCore> target = lockCore();
            if (!target) {
                return;
            }
            if (!applyResynchronization(backendSnapshot, *target)) {
                static_cast<void>(target->requireSnapshot(OccurrenceFlushUrgency::Immediate));
                return;
            }
            if (backendSnapshot.sequence.value() > observerProcessedThrough.value()) {
                observerProcessedThrough = backendSnapshot.sequence;
            }
            drainDeferredThreadReadsAfterResynchronization(backendSnapshot.sequence);
            drainDeferredControllerCompletion();
            drainDeferredThreadReadCompletions();
            drainDeferredSessionCloses();
        }

        void drainDeferredObserverEvents() noexcept {
            if (sessionAdmissionDepth != 0 || deferredObserverEvents.empty()) {
                return;
            }
            std::vector<backend::SequencedBackendEvent> deferred;
            deferred.swap(deferredObserverEvents);
            processEvents(deferred);
        }

        void finishSessionAdmission(ServerCore& target) noexcept {
            if (resynchronizationPendingDuringAdmission) {
                resynchronizationPendingDuringAdmission = false;
                deferredObserverEvents.clear();
                recoverCurrentSnapshot(target);
                return;
            }
            drainDeferredObserverEvents();
        }

        void drainDeferredControllerCompletion() noexcept {
            if (!deferredControllerCompletion || observerProcessedThrough.value() < deferredControllerCompletion->requiredThrough.value()) {
                return;
            }
            const auto session = sessions.find(deferredControllerCompletion->key);
            if (session == sessions.end() || !session->second.backendSession || !session->second.backendSession->isOpen()) {
                // BackendCore closes its session state before scheduling the
                // close callback. Detect that state directly so an already-
                // queued observer callback cannot publish a completion from
                // the terminated generation first.
                return;
            }
            if (deferredSessionCloses.contains(deferredControllerCompletion->key)) {
                // Once BackendCore has reported this command session closed,
                // every completion from that generation is stale even while
                // the observer is still advancing to the close fence. Let
                // finishSessionClosed cancel it before any transient
                // controller transition can be committed.
                return;
            }
            DeferredControllerCompletion ready = std::move(*deferredControllerCompletion);
            deferredControllerCompletion.reset();
            finishCompletion(ready.key, ready.completion, nullptr);
        }

        void drainDeferredThreadReadCompletions() noexcept {
            for (auto deferred = deferredThreadReadCompletions.begin(); deferred != deferredThreadReadCompletions.end();) {
                if (observerProcessedThrough.value() < deferred->second.requiredThrough.value()) {
                    ++deferred;
                    continue;
                }
                DeferredThreadReadCompletion ready = std::move(deferred->second);
                deferred = deferredThreadReadCompletions.erase(deferred);
                if (ready.cancelDeadline) {
                    ready.cancelDeadline();
                }
                releaseDeferredThreadReadBytes(ready.retainedBytes);
                const auto session = sessions.find(ready.key);
                if (session == sessions.end()) {
                    continue;
                }
                const auto pending = session->second.pending.find(ready.completion.requestId);
                if (pending == session->second.pending.end()) {
                    continue;
                }
                if (pending->second.threadReadStateEffectVersion != 1) {
                    valueFailure(ready.key,
                                 ready.completion.requestId,
                                 ErrorCode::InternalError,
                                 "deferred thread-read completion lacks negotiated state-effect authority");
                    continue;
                }
                if (observerProcessedThrough.value() > ready.requiredThrough.value()) {
                    // The ordinary event path may also overtake a retained
                    // completion through an independent observer drain. As
                    // with Snapshot supersession below, preserve the newer
                    // synchronized State and fail only this command.
                    valueFailure(ready.key,
                                 ready.completion.requestId,
                                 ErrorCode::Conflict,
                                 "thread-read state effect was superseded by observer synchronization");
                    continue;
                }
                // The body and its backend fence were captured atomically once
                // at completion time.  Re-capturing here could chase an
                // indefinitely streaming thread forever; observer batching is
                // split at this exact fence instead.
                finishCompletion(ready.key,
                                 ready.completion,
                                 ready.completion.threadReadSnapshot ? &*ready.completion.threadReadSnapshot : nullptr);
            }
        }

        void drainDeferredThreadReadsAfterResynchronization(backend::SequenceNumber through) noexcept {
            for (auto deferred = deferredThreadReadCompletions.begin(); deferred != deferredThreadReadCompletions.end();) {
                if (deferred->second.requiredThrough.value() > through.value()) {
                    ++deferred;
                    continue;
                }
                DeferredThreadReadCompletion ready = std::move(deferred->second);
                deferred = deferredThreadReadCompletions.erase(deferred);
                if (ready.cancelDeadline) {
                    ready.cancelDeadline();
                }
                releaseDeferredThreadReadBytes(ready.retainedBytes);
                // Only negotiated state effects enter this queue. The
                // authoritative snapshot has advanced the requester beyond
                // the captured effect fence, so returning that older effect as
                // success would violate exact pre-application.
                valueFailure(ready.key,
                             ready.completion.requestId,
                             ErrorCode::CapacityExceeded,
                             "thread-read state effect was superseded by snapshot synchronization");
            }
        }

        void drainDeferredSessionCloses() noexcept {
            for (auto close = deferredSessionCloses.begin(); close != deferredSessionCloses.end();) {
                const auto session = sessions.find(close->first);
                if (session != sessions.end() && observerProcessedThrough.value() < close->second.requiredThrough.value()) {
                    ++close;
                    continue;
                }
                const std::string key = close->first;
                std::string reason = std::move(close->second.reason);
                close = deferredSessionCloses.erase(close);
                finishSessionClosed(key, reason);
            }
        }

        [[nodiscard]] std::optional<backend::Snapshot> currentSnapshotNoThrow() const noexcept {
            try {
                return runtime.snapshot();
            } catch (...) {
                return std::nullopt;
            }
        }

        [[nodiscard]] bool publishProjectedResynchronization(const backend::Snapshot& snapshot,
                                                             model::CanonicalSnapshot projected,
                                                             ServerCore& target) noexcept {
            try {
                if (!reconcileTopology(snapshot)) {
                    return false;
                }
                const std::shared_ptr<ServerCore> current = lockCore();
                if (!bindingCurrent(current) || current.get() != &target) {
                    return false;
                }
                if (!target.publishSnapshot(std::move(projected)).accepted) {
                    return false;
                }
                itemContentCoveredThrough.clear();
                return true;
            } catch (...) {
                return false;
            }
        }

        [[nodiscard]] bool applyResynchronization(const backend::Snapshot& snapshot, ServerCore& target) noexcept {
            try {
                if (!reconcileTopology(snapshot)) {
                    return false;
                }
                model::ModelResult<model::CanonicalSnapshot> projected = projection.projectSnapshot(snapshot);
                if (!bindingCurrent(lockCore()) || !projected) {
                    return false;
                }
                if (!target.publishSnapshot(std::move(projected).value()).accepted) {
                    return false;
                }
                itemContentCoveredThrough.clear();
                return true;
            } catch (...) {
                return false;
            }
        }

        void recoverCurrentSnapshot(ServerCore& target) noexcept {
            try {
                const backend::Snapshot current = runtime.snapshot();
                if (applyResynchronization(current, target)) {
                    if (current.sequence.value() > observerProcessedThrough.value()) {
                        observerProcessedThrough = current.sequence;
                    }
                    drainDeferredThreadReadsAfterResynchronization(current.sequence);
                    drainDeferredControllerCompletion();
                    drainDeferredThreadReadCompletions();
                    drainDeferredSessionCloses();
                    return;
                }
            } catch (...) {
            }
            static_cast<void>(target.requireSnapshot(OccurrenceFlushUrgency::Immediate));
        }

        [[nodiscard]] std::optional<model::SessionIdentity> externalIdentity(backend::SessionId id, ServerCore& target) {
            if (const auto found = externalIdentities.find(id); found != externalIdentities.end()) {
                return found->second;
            }
            std::optional<model::SessionIdentity> identity = target.reserveExternalSessionIdentity();
            while (identity && identity->value() == std::to_string(id.value())) {
                identity = target.reserveExternalSessionIdentity();
            }
            if (!identity) {
                return std::nullopt;
            }
            externalIdentities.emplace(id, *identity);
            return identity;
        }

        [[nodiscard]] bool itemContentEventCovered(const backend::SequencedBackendEvent& sequenced) const {
            const auto* content = std::get_if<backend::ItemContentChanged>(&sequenced.event);
            if (content == nullptr) {
                return false;
            }
            const auto covered = itemContentCoveredThrough.find(contentSnapshotKey(*content));
            return covered != itemContentCoveredThrough.end() && sequenced.sequence <= covered->second;
        }

        void retainItemContentCoverage(std::span<const backend::SequencedBackendEvent> events,
                                       backend::SequenceNumber coveredThrough) {
            for (const backend::SequencedBackendEvent& sequenced : events) {
                const auto* content = std::get_if<backend::ItemContentChanged>(&sequenced.event);
                if (content == nullptr) {
                    continue;
                }
                const backend::ItemContentSnapshotKey key = contentSnapshotKey(*content);
                const auto existing = itemContentCoveredThrough.find(key);
                if (existing == itemContentCoveredThrough.end()) {
                    itemContentCoveredThrough.emplace(std::move(key), coveredThrough);
                } else if (coveredThrough > existing->second) {
                    existing->second = coveredThrough;
                }
            }
        }

        void pruneItemContentCoverage() noexcept {
            for (auto covered = itemContentCoveredThrough.begin(); covered != itemContentCoveredThrough.end();) {
                if (covered->second <= observerProcessedThrough) {
                    covered = itemContentCoveredThrough.erase(covered);
                } else {
                    ++covered;
                }
            }
        }

        [[nodiscard]] bool reconcileTopology(const backend::Snapshot& snapshot) noexcept {
            const std::shared_ptr<ServerCore> target = lockCore();
            if (!target) {
                return false;
            }
            try {
                std::set<backend::SessionId> present;
                ExternalTopology topology;
                topology.sessions.reserve(snapshot.sessions.size());
                for (const backend::SessionSnapshot& backendSession : snapshot.sessions) {
                    present.insert(backendSession.id);
                    if (ownedBackendSessions.contains(backendSession.id) || retiredBackendSessions.contains(backendSession.id)) {
                        continue;
                    }
                    const std::optional<model::SessionIdentity> identity = externalIdentity(backendSession.id, *target);
                    if (!identity) {
                        return false;
                    }
                    model::SessionState session(*identity);
                    session.role =
                        backendSession.role == backend::SessionRole::Controller ? SessionRole::Controller : SessionRole::Observer;
                    topology.sessions.push_back(std::move(session));
                }
                for (auto owned = ownedBackendSessions.begin(); owned != ownedBackendSessions.end();) {
                    const backend::SessionId backendId = owned->first;
                    if (!present.contains(backendId)) {
                        if (sessions.contains(owned->second)) {
                            retiredBackendSessions.insert(backendId);
                            ++owned;
                        } else {
                            // A locally closed session's disconnect echo may
                            // have been replaced by observer resynchronization.
                            // The authoritative Snapshot now makes it safe to
                            // retire that allocation-free ownership marker.
                            owned = ownedBackendSessions.erase(owned);
                        }
                    } else {
                        ++owned;
                    }
                }
                for (auto retired = retiredBackendSessions.begin(); retired != retiredBackendSessions.end();) {
                    if (!present.contains(*retired)) {
                        retired = retiredBackendSessions.erase(retired);
                    } else {
                        ++retired;
                    }
                }
                for (auto external = externalIdentities.begin(); external != externalIdentities.end();) {
                    if (!present.contains(external->first)) {
                        external = externalIdentities.erase(external);
                    } else {
                        ++external;
                    }
                }
                observedBackendController = snapshot.controller;
                if (snapshot.controller && !ownedBackendSessions.contains(*snapshot.controller) &&
                    !retiredBackendSessions.contains(*snapshot.controller)) {
                    topology.controller = externalIdentity(*snapshot.controller, *target);
                    if (!topology.controller) {
                        return false;
                    }
                }
                const bool bridgeControllerPresent = snapshot.controller && ownedBackendSessions.contains(*snapshot.controller) &&
                                                     !retiredBackendSessions.contains(*snapshot.controller);
                return target->replaceExternalTopology(
                    std::move(topology.sessions), std::move(topology.controller), bridgeControllerPresent);
            } catch (...) {
                return false;
            }
        }

        backend::detail::BackendCoreRuntime& runtime;
        static constexpr std::size_t MaximumDeferredThreadReadCompletions = 8;
        static constexpr std::uint64_t DeferredThreadReadDeadlineMs = 5000;
        const std::size_t maximumResultBytes;
        const std::size_t maximumThreadReadResultBytes;
        const std::size_t maximumDeferredThreadReadBytes;
        ServerCore* coreIdentity = nullptr;
        std::weak_ptr<ServerCore> coreLifetime;
        BackendProjection projection;
        backend::BackendObserverSubscription observer;
        std::map<std::string, SessionRecord, std::less<>> sessions;
        std::map<backend::SessionId, std::string> ownedBackendSessions;
        std::set<backend::SessionId> retiredBackendSessions;
        std::map<backend::SessionId, model::SessionIdentity> externalIdentities;
        std::optional<backend::SessionId> observedBackendController;
        backend::SequenceNumber observerProcessedThrough;
        // Bounded by the BackendCore observer backlog and cleared by every
        // authoritative Snapshot/resynchronization.
        std::map<backend::ItemContentSnapshotKey, backend::SequenceNumber, ItemContentSnapshotKeyLess> itemContentCoveredThrough;
        std::size_t sessionAdmissionDepth = 0;
        bool resynchronizationPendingDuringAdmission = false;
        std::vector<backend::SequencedBackendEvent> deferredObserverEvents;
        std::optional<DeferredControllerCompletion> deferredControllerCompletion;
        std::map<std::pair<std::string, std::string>, DeferredThreadReadCompletion> deferredThreadReadCompletions;
        std::size_t deferredThreadReadBytes = 0;
        TimerScheduler timerScheduler;
        std::map<std::string, DeferredSessionClose, std::less<>> deferredSessionCloses;
    };

    BackendCoreBridge::BackendCoreBridge(backend::detail::BackendCoreRuntime& backend,
                                         std::size_t maximumResultBytes,
                                         std::size_t maximumThreadReadResultBytes,
                                         TimerScheduler timerScheduler)
        : state(std::make_shared<State>(backend,
                                        maximumResultBytes,
                                        maximumThreadReadResultBytes,
                                        std::move(timerScheduler))) {
    }

    BackendCoreBridge::~BackendCoreBridge() {
        close();
    }

    void BackendCoreBridge::bind(ServerCore& core) noexcept {
        state->bind(core);
    }

    void BackendCoreBridge::unbind(ServerCore& core) noexcept {
        state->unbind(core);
    }

    void BackendCoreBridge::bindLifetime(const std::shared_ptr<ServerCore>& core) {
        state->bindLifetime(core);
    }

    void BackendCoreBridge::start() {
        state->start();
    }

    void BackendCoreBridge::close() noexcept {
        if (state) {
            state->close();
        }
    }

    bool BackendCoreBridge::providerReady() const noexcept {
        return state->runtime.isReady();
    }

    model::CanonicalSnapshot BackendCoreBridge::snapshot() const {
        model::ModelResult<model::CanonicalSnapshot> projected = state->projection.projectSnapshot(state->runtime.snapshot());
        if (!projected) {
            throw std::runtime_error("BackendCore snapshot violates the canonical frontend model");
        }
        return std::move(projected).value();
    }

    BackendSubmitStatus BackendCoreBridge::submit(BackendInvocation invocation) {
        return state->submit(std::move(invocation));
    }

    bool BackendCoreBridge::performProviderLifecycleAction(ProviderLifecycleAction action) {
        try {
            switch (action) {
                case ProviderLifecycleAction::Start:
                    state->runtime.start();
                    break;
                case ProviderLifecycleAction::Stop:
                    state->runtime.stop();
                    break;
                case ProviderLifecycleAction::Restart:
                    state->runtime.restart();
                    break;
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    bool BackendCoreBridge::sessionOpened(const FrontendSessionToken& token, const FrontendPrincipal&) {
        return state->openSession(token);
    }

    void BackendCoreBridge::sessionClosed(const FrontendSessionToken& token) noexcept {
        state->closeSession(token);
    }

    bool BackendCoreBridgeTestAccess::controllerResultValid(generated::MethodId method,
                                                            std::uint64_t expectedBackendSession,
                                                            std::optional<std::uint64_t> reportedBackendController,
                                                            bool reportedControllerRole) noexcept {
        backend::ControllerResult result;
        if (reportedBackendController) {
            result.controller = backend::SessionId{*reportedBackendController};
        }
        result.role = reportedControllerRole ? backend::SessionRole::Controller : backend::SessionRole::Observer;
        return ::ai::openai::codex::frontend::internal::server::controllerResultValid(
            method, backend::SessionId{expectedBackendSession}, result);
    }

    std::optional<std::vector<backend::SequencedBackendEvent>>
    BackendCoreBridgeTestAccess::coalesceItemContentEvents(
        std::span<const backend::SequencedBackendEvent> events) noexcept {
        return coalescedItemContentEvents(events);
    }

    bool BackendCoreBridgeTestAccess::itemContentSnapshotIsAhead(backend::SequenceNumber eventSequence,
                                                                 backend::SequenceNumber snapshotSequence) noexcept {
        return ::ai::openai::codex::frontend::internal::server::itemContentSnapshotIsAhead(eventSequence, snapshotSequence);
    }

    Json BackendCoreBridgeTestAccess::boundedThreadReadResult(const typed::ThreadId& id,
                                                              const std::optional<backend::ThreadSnapshot>& source,
                                                              std::size_t maximumBytes) {
        return ::ai::openai::codex::frontend::internal::server::boundedThreadReadResult(id, source, maximumBytes).value;
    }

} // namespace ai::openai::codex::frontend::internal::server
