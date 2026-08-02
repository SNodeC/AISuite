/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_BACKEND_BACKENDSTATE_H
#define AI_OPENAI_CODEX_BACKEND_BACKENDSTATE_H

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/typed/Conversation.h"
#include "ai/openai/codex/typed/Items.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "ai/openai/codex/typed/Threads.h"
#include "ai/openai/codex/typed/Turns.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace ai::openai::codex::backend {

    class SessionId {
    public:
        constexpr SessionId() noexcept = default;
        explicit constexpr SessionId(std::uint64_t value) noexcept
            : id(value) {
        }

        constexpr std::uint64_t value() const noexcept {
            return id;
        }

        constexpr explicit operator bool() const noexcept {
            return id != 0;
        }

        auto operator<=>(const SessionId&) const = default;

    private:
        std::uint64_t id = 0;
    };

    class PendingRequestId {
    public:
        constexpr PendingRequestId() noexcept = default;
        explicit constexpr PendingRequestId(std::uint64_t value) noexcept
            : id(value) {
        }

        constexpr std::uint64_t value() const noexcept {
            return id;
        }

        constexpr explicit operator bool() const noexcept {
            return id != 0;
        }

        auto operator<=>(const PendingRequestId&) const = default;

    private:
        std::uint64_t id = 0;
    };

    class SequenceNumber {
    public:
        constexpr SequenceNumber() noexcept = default;
        explicit constexpr SequenceNumber(std::uint64_t value) noexcept
            : sequence(value) {
        }

        constexpr std::uint64_t value() const noexcept {
            return sequence;
        }

        auto operator<=>(const SequenceNumber&) const = default;

    private:
        std::uint64_t sequence = 0;
    };

    enum class ProviderLifecycle { Stopped, Starting, Initializing, Ready, Stopping, Failed, Recovering };
    enum class RecoveryStatus { Idle, Waiting, Exhausted };
    enum class Freshness { Unknown, Current, Stale };
    enum class SessionRole { Observer, Controller };
    enum class ItemLifecycle { Unknown, Started, Completed, Failed };

    struct RecoveryState {
        RecoveryStatus status = RecoveryStatus::Idle;
        std::uint32_t attempts = 0;
        std::optional<std::uint64_t> delayMs;

        bool operator==(const RecoveryState&) const = default;
    };

    struct ProviderState {
        ProviderLifecycle lifecycle = ProviderLifecycle::Stopped;
        std::uint64_t generation = 0;
        bool desiredRunning = false;
        std::optional<Error> lastError;
        RecoveryState recovery;
        std::optional<typed::InitializeResponse> initialization;
    };

    struct SourceStamp {
        std::uint64_t generation = 0;
        Freshness freshness = Freshness::Unknown;

        bool operator==(const SourceStamp&) const = default;
    };

    struct BackendCapacityOptions {
        std::size_t maxSessions = 128;
        std::size_t maxObservers = 16;
        std::size_t maxActiveOperations = 4096;
        std::size_t maxPendingRequests = 1024;
        std::size_t maxRetainedThreads = 2048;
        std::size_t maxRetainedTurns = 16384;
        std::size_t maxRetainedItems = 65536;
        std::size_t maxAccumulatedContentBytes = 64U * 1024U * 1024U;
        std::size_t maxSnapshotBytes = 8U * 1024U * 1024U;

        bool operator==(const BackendCapacityOptions&) const = default;
    };

    struct CapacityState {
        std::size_t retainedThreads = 0;
        std::size_t retainedTurns = 0;
        std::size_t retainedItems = 0;
        std::size_t accumulatedContentBytes = 0;
        std::uint64_t rejectedSessions = 0;
        std::uint64_t rejectedObservers = 0;
        std::uint64_t rejectedOperations = 0;
        std::uint64_t providerRequestOverflows = 0;
        std::uint64_t evictedThreads = 0;
        std::uint64_t evictedTurns = 0;
        std::uint64_t evictedItems = 0;
        std::uint64_t droppedContentBytes = 0;
        std::uint64_t snapshotOmissions = 0;
        BackendCapacityOptions limits;

        bool operator==(const CapacityState&) const = default;
    };

    struct DiagnosticSummary {
        std::uint64_t received = 0;
        std::vector<std::string> recent;
    };

    struct ModelRerouteRecord {
        typed::ModelId from;
        typed::ModelId to;
        std::string reason;
    };

    struct ItemState {
        typed::ThreadItem item;
        ItemLifecycle lifecycle = ItemLifecycle::Unknown;
        std::string agentText;
        std::string reasoningText;
        std::string reasoningSummary;
        std::string commandOutput;
        std::uint64_t droppedContentBytes = 0;
        std::optional<std::int64_t> startedAtMs;
        std::optional<std::int64_t> completedAtMs;
        Json extensions = Json::object();
        SourceStamp stamp;
        bool connectionInvalidated = false;
    };

    struct TurnState {
        typed::Turn turn;
        std::map<std::string, ItemState> items;
        std::vector<typed::ItemId> itemOrder;
        bool active = false;
        bool terminal = false;
        std::optional<Json> failure;
        std::optional<Json> tokenUsage;
        std::vector<ModelRerouteRecord> modelReroutes;
        Json extensions = Json::object();
        SourceStamp stamp;
        bool connectionInvalidated = false;
    };

    struct ThreadState {
        typed::Thread thread;
        std::map<std::string, TurnState> turns;
        std::vector<typed::TurnId> turnOrder;
        bool fullyLoaded = false;
        Json extensions = Json::object();
        SourceStamp stamp;
    };

    struct PendingRequestState {
        PendingRequestId id;
        typed::TypedServerRequest request;
        std::uint64_t connectionGeneration = 0;
    };

    struct ConnectedSessionState {
        SessionId id;
        SessionRole role = SessionRole::Observer;
    };

    struct ThreadListState {
        bool hasLoadedPage = false;
        bool complete = false;
        std::optional<std::string> nextCursor;
        std::optional<std::string> backwardsCursor;
        std::size_t pagesLoaded = 0;
        SourceStamp stamp;
    };

    struct ExtensionRecord {
        std::string method;
        Json payload = nullptr;
        std::optional<std::string> decodingError;
        std::optional<std::uint64_t> originalMethodBytes;
        std::optional<std::uint64_t> originalPayloadBytes;
        std::optional<std::uint64_t> originalDecodingErrorBytes;
        std::optional<typed::DecodeDiagnostic> diagnostic = std::nullopt;
        std::optional<std::uint64_t> originalDiagnosticBytes = std::nullopt;
    };

    struct BackendState {
        ProviderState provider;
        CapacityState capacity;
        DiagnosticSummary diagnostics;
        std::map<std::string, ThreadState> threads;
        std::vector<typed::ThreadId> threadOrder;
        std::map<PendingRequestId, PendingRequestState> pendingRequests;
        std::map<SessionId, ConnectedSessionState> sessions;
        std::optional<SessionId> controller;
        SequenceNumber sequence;
        bool sequenceExhausted = false;
        ThreadListState threadList;
        std::vector<ExtensionRecord> recentExtensions;
    };

    ProviderLifecycle toProviderLifecycle(State state) noexcept;
    bool isTerminal(const typed::TurnStatus& status) noexcept;
    std::optional<typed::ItemId> itemId(const typed::ThreadItem& item);
    std::string itemType(const typed::ThreadItem& item);

    ThreadState* findThread(BackendState& state, const typed::ThreadId& threadId) noexcept;
    const ThreadState* findThread(const BackendState& state, const typed::ThreadId& threadId) noexcept;
    TurnState* findTurn(BackendState& state, const typed::ThreadId& threadId, const typed::TurnId& turnId) noexcept;
    const TurnState* findTurn(const BackendState& state, const typed::ThreadId& threadId, const typed::TurnId& turnId) noexcept;
    ItemState*
    findItem(BackendState& state, const typed::ThreadId& threadId, const typed::TurnId& turnId, const typed::ItemId& itemId) noexcept;
    const ItemState*
    findItem(const BackendState& state, const typed::ThreadId& threadId, const typed::TurnId& turnId, const typed::ItemId& itemId) noexcept;

} // namespace ai::openai::codex::backend

#endif // AI_OPENAI_CODEX_BACKEND_BACKENDSTATE_H
