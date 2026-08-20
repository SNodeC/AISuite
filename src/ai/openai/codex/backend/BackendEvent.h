/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BACKEND_BACKENDEVENT_H
#define AI_OPENAI_CODEX_BACKEND_BACKENDEVENT_H

#include "ai/openai/codex/backend/BackendCommand.h"
#include "ai/openai/codex/backend/BackendState.h"
#include "ai/openai/codex/typed/Events.h"
#include "ai/openai/codex/typed/Threads.h"
#include "ai/openai/codex/typed/Turns.h"

#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ai::openai::codex::backend {

    enum class EntityLoad { Summary, Full };

    struct ProviderLifecycleChanged {
        ProviderState provider;
    };

    struct ProviderConnectionInvalidated {
        std::uint64_t generation = 0;
        std::string reason;
    };

    enum class CapacityMetric {
        RejectedSessions,
        RejectedObservers,
        RejectedOperations,
        ProviderRequestOverflows,
        EvictedThreads,
        EvictedTurns,
        EvictedItems,
        DroppedContentBytes,
        SnapshotOmissions,
        EvictedNotices,
        EvictedProcesses,
        DroppedProcessOutputBytes,
        EvictedFilesystemWatches,
        EvictedFuzzySearchSessions,
        EvictedActivityRecords
    };

    struct CapacityConfigured {
        BackendCapacityOptions limits;
    };

    struct CapacityChanged {
        CapacityMetric metric = CapacityMetric::RejectedSessions;
        std::uint64_t amount = 1;
        // True only when retention enforcement changed canonical entities
        // outside the nominal event. Frontend bridges must rebase from a
        // snapshot instead of treating this as a counter-only occurrence.
        bool canonicalStateRewritten = false;

        bool operator==(const CapacityChanged&) const = default;
    };

    struct DiagnosticReceived {
        std::string message;
    };

    struct ProviderOperationCompleted {
        std::string method;
        BackendCommand command;
        ProviderOperationValue value;
        std::optional<std::string> resourceReservationKey;
    };

    // ProviderOperationCompleted is an exact reducer input and can contain
    // arbitrarily large, heap-backed typed parameters.  BackendCore publishes
    // this bounded marker after reducing that input so session and observer
    // queues never retain a second copy of the command payload.
    struct ProviderOperationStateChanged {
        std::string method;
    };

    enum class ProviderResourceKind { Process, FilesystemWatch, FuzzySearch };

    struct ProviderResourceAdmissionRequested {
        ProviderResourceKind kind = ProviderResourceKind::Process;
        std::string key;
        std::string resourceId;
    };

    struct ProviderResourceAdmissionReleased {
        ProviderResourceKind kind = ProviderResourceKind::Process;
        std::string key;
    };

    struct ThreadUpserted {
        typed::Thread thread;
        EntityLoad load = EntityLoad::Summary;
    };

    struct ThreadListUpdated {
        typed::ThreadListResponse page;
        std::optional<std::string> requestedCursor;
        bool initialRefresh = false;
    };

    struct ThreadStatusUpdated {
        typed::ThreadId threadId;
        typed::ThreadStatus status;
    };

    struct TurnUpserted {
        typed::Turn turn;
    };

    struct TurnCompleted {
        typed::Turn turn;
    };

    struct TurnFailed {
        typed::Turn turn;
        Json error = nullptr;
    };

    struct TurnErrorUpdated {
        typed::ThreadId threadId;
        typed::TurnId turnId;
        Json error = nullptr;
        bool willRetry = false;
    };

    struct ItemUpserted {
        typed::ThreadId threadId;
        typed::TurnId turnId;
        typed::ThreadItem item;
        ItemLifecycle lifecycle = ItemLifecycle::Unknown;
        std::optional<std::int64_t> occurredAtMs;
    };

    struct ItemContentChanged {
        enum class Kind { AgentText, ReasoningText, ReasoningSummary, CommandOutput };

        typed::ThreadId threadId;
        typed::TurnId turnId;
        typed::ItemId itemId;
        Kind kind = Kind::AgentText;
        std::string delta;
        std::optional<std::int64_t> index;
        // BackendCore records these from the exact canonical item immediately
        // before reducer application. They are intentionally absent on
        // translated, legacy, or otherwise unobserved event paths.
        std::optional<std::size_t> channelBytesBefore = std::nullopt;
        std::optional<std::uint64_t> droppedContentBytesBefore = std::nullopt;
    };

    struct FileChangeUpdated {
        typed::ThreadId threadId;
        typed::TurnId turnId;
        typed::ItemId itemId;
        Json changes = Json::array();
    };

    struct TokenUsageUpdated {
        typed::ThreadId threadId;
        typed::TurnId turnId;
        Json usage = nullptr;
    };

    struct ModelRerouted {
        typed::ThreadId threadId;
        typed::TurnId turnId;
        typed::ModelId from;
        typed::ModelId to;
        std::string reason;
    };

    struct PendingRequestAdded {
        PendingRequestState pending;
    };

    struct PendingRequestRemoved {
        PendingRequestId id;
        std::string reason;
    };

    struct ControllerChanged {
        std::optional<SessionId> controller;
    };

    struct SessionChanged {
        SessionId id;
        bool connected = false;
        SessionRole role = SessionRole::Observer;
    };

    struct CodexExtensionReceived {
        std::string method;
        Json payload = nullptr;
        std::optional<std::string> decodingError;
        std::optional<typed::DecodeDiagnostic> diagnostic = std::nullopt;
        std::optional<typed::Event> typedEvent = std::nullopt;
        bool safeProjection = false;
        bool methodTruncated = false;
        bool payloadTruncated = false;
        bool decodingErrorTruncated = false;
        bool sensitiveFieldsRedacted = false;
        std::uint64_t originalMethodBytes = 0;
        std::optional<std::uint64_t> originalPayloadBytes = std::nullopt;
        std::uint64_t originalDecodingErrorBytes = 0;
    };

    using BackendEvent = std::variant<ProviderLifecycleChanged,
                                      ProviderConnectionInvalidated,
                                      CapacityConfigured,
                                      CapacityChanged,
                                      DiagnosticReceived,
                                      ProviderOperationCompleted,
                                      ProviderOperationStateChanged,
                                      ProviderResourceAdmissionRequested,
                                      ProviderResourceAdmissionReleased,
                                      ThreadUpserted,
                                      ThreadListUpdated,
                                      ThreadStatusUpdated,
                                      TurnUpserted,
                                      TurnCompleted,
                                      TurnFailed,
                                      TurnErrorUpdated,
                                      ItemUpserted,
                                      ItemContentChanged,
                                      FileChangeUpdated,
                                      TokenUsageUpdated,
                                      ModelRerouted,
                                      PendingRequestAdded,
                                      PendingRequestRemoved,
                                      ControllerChanged,
                                      SessionChanged,
                                      CodexExtensionReceived>;

    struct SequencedBackendEvent {
        SequenceNumber sequence;
        BackendEvent event;
    };

} // namespace ai::openai::codex::backend

#endif // AI_OPENAI_CODEX_BACKEND_BACKENDEVENT_H
