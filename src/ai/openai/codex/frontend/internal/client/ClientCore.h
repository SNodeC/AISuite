/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_CLIENT_CLIENTCORE_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_CLIENT_CLIENTCORE_H

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/internal/model/Model.h"
#include "ai/openai/codex/frontend/internal/model/Occurrence.h"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ai::openai::codex::frontend::internal::client {

    namespace model = ::ai::openai::codex::frontend::internal::model;

    using PhysicalGeneration = std::uint64_t;

    enum class ConnectionState { Disconnected, Connecting, Authenticating, Synchronizing, Ready, Closing, Closed };
    enum class RepresentationMode { Unknown, LegacyV1, ExpandedV1 };
    enum class PublishedFreshness { Current, Stale, Synchronizing };
    enum class ErrorOrigin { Client, Transport, Protocol, Command };
    enum class Availability { Unknown, No, Yes };

    enum class SnapshotDomain {
        Accounts,
        Models,
        Configuration,
        FilesystemWatches,
        FuzzySearches,
        PermissionProfiles,
        Reviews,
        Apps,
        ExternalAgents,
        Hooks,
        Marketplace,
        Plugins,
        Skills,
        Mcp,
        WindowsSandbox,
        Platform,
        RemoteControl,
        Integrations,
        Notices,
        Activities,
        Diagnostics,
    };

    enum class ClientErrorCode {
        InvalidConfiguration,
        AlreadyConnected,
        NotConnected,
        NotReady,
        Closed,
        MethodUnavailable,
        MethodNotPermitted,
        TooManyPendingOperations,
        SynchronizationAlreadyActive,
        SerializationFailed,
        SendRejected,
        TransportFailure,
        DecodeFailure,
        UnexpectedMessage,
        StateDivergence,
        StateCapacityExceeded,
        ResponseTypeMismatch,
        RequestIdExhausted,
        CallbackFailure,
    };

    struct ClientError {
        ErrorOrigin origin = ErrorOrigin::Client;
        std::optional<ClientErrorCode> clientCode;
        std::optional<ErrorCode> protocolCode;
        std::string message;
        std::optional<Json> remoteDetails;
        bool retryable = false;
        // The permanent Core retains its typed P2 error code. The public P3
        // adapter uses this private provenance bit solely to preserve the
        // legacy live-Snapshot StateDivergence surface.
        bool publicLiveSnapshotStateDivergence = false;

        bool operator==(const ClientError&) const = default;
    };

    struct TransportError {
        std::string message;
        bool retryable = true;

        bool operator==(const TransportError&) const = default;
    };

    enum class SendStatus { Accepted, Backpressure, Closed, Failed };

    struct SendResult {
        SendStatus status = SendStatus::Accepted;
        std::optional<TransportError> error;

        bool operator==(const SendResult&) const = default;
    };

    struct OutboundMessage {
        using Value = std::variant<Hello, generated::DefinedCommand>;

        Value value;
        bool sensitive = false;

        [[nodiscard]] bool isHello() const noexcept;
        [[nodiscard]] bool isCommand() const noexcept;
    };

    struct TransportCallbacks {
        std::function<SendResult(OutboundMessage)> send;
        std::function<void(std::string_view)> close;
    };

    struct AuthenticationContext {
        AuthenticationCredential credential = NoCredential{};
        std::optional<std::string> continuityKey;
    };

    using CredentialProvider = std::function<AuthenticationContext()>;

    struct ClientLimits {
        std::size_t maximumInboundMessageBytes = 16U * 1024U * 1024U;
        std::size_t maximumDecodedStateBytes = 64U * 1024U * 1024U;
        std::size_t maximumRetainedEntities = 1U << 20U;
        std::size_t maximumPendingOperations = 256;
        std::size_t maximumRetainedDiagnostics = 64;
        std::size_t maximumLocalDiagnostics = 64;
        std::size_t maximumCompletedRequestIds = 512;

        bool operator==(const ClientLimits&) const = default;
    };

    struct ClientOptions {
        std::vector<FrontendCapability> requestedCapabilities{
            FrontendCapability::CompleteBackendDomains,
            FrontendCapability::DedicatedPendingRequests,
            FrontendCapability::DedicatedNotificationEvents,
            FrontendCapability::CompleteThreadItems,
            FrontendCapability::ScopeProjectedState,
        };
        std::vector<FrontendCapability> requiredCapabilities;
        CredentialProvider credentialProvider;
        ClientLimits limits;
        std::string requestIdPrefix = "c";
        std::uint64_t initialRequestId = 1;
        bool allowLegacyV1 = true;
    };

    struct SessionInfo {
        model::SessionIdentity id;
        SessionRole role = SessionRole::Observer;
        SyncMode synchronizationMode = SyncMode::Snapshot;
        model::FrontendSequence serverCurrentSequence;
        std::optional<std::string> serverVersion;
        std::vector<FrontendCapability> requestedCapabilities;
        std::vector<FrontendCapability> selectedCapabilities;
        std::vector<FrontendCapability> observedCapabilities;
        std::optional<std::vector<generated::MethodId>> availableMethods;
        std::optional<std::vector<generated::MethodId>> permittedMethods;
        std::optional<std::vector<FrontendScope>> permittedScopes;

        explicit SessionInfo(model::SessionIdentity sessionId)
            : id(std::move(sessionId)) {
        }

        bool operator==(const SessionInfo&) const = default;
    };

    struct PublishedState {
        std::uint64_t revision = 0;
        PublishedFreshness freshness = PublishedFreshness::Stale;
        RepresentationMode representation = RepresentationMode::Unknown;
        std::optional<model::FrontendSequence> visibleSequence;
        std::optional<model::FrontendSequence> synchronizedThrough;
        std::optional<SessionInfo> session;
        std::optional<std::string> projectionFingerprint;
        std::shared_ptr<const model::CanonicalSnapshot> snapshot;

        [[nodiscard]] const model::ThreadState* thread(std::string_view id) const noexcept;
        [[nodiscard]] const model::SessionState* sessionById(std::string_view id) const noexcept;
        [[nodiscard]] const model::TurnState* turn(std::string_view id) const noexcept;
        [[nodiscard]] const model::ThreadItem* item(std::string_view id) const noexcept;
        [[nodiscard]] const model::PendingRequest* pendingRequest(std::string_view id) const noexcept;
        [[nodiscard]] const model::ProcessState* process(std::string_view handle) const noexcept;
        [[nodiscard]] const model::FilesystemWatchRecord* filesystemWatch(std::string_view id) const noexcept;
        [[nodiscard]] const model::FuzzySearchRecord* fuzzySearch(std::string_view id) const noexcept;
        [[nodiscard]] const model::ActivityRecord* activity(std::string_view key) const noexcept;
        [[nodiscard]] const model::DomainState* domain(SnapshotDomain domain) const noexcept;
        [[nodiscard]] const model::DomainResultSummary* domainResult(SnapshotDomain domain, std::string_view identity) const noexcept;
        [[nodiscard]] std::optional<std::size_t> measuredBytes() const noexcept;
        [[nodiscard]] Json serializeForTesting() const noexcept;
    };

    struct CapabilityStatus {
        FrontendCapability capability = FrontendCapability::MethodDiscovery;
        Availability defined = Availability::Unknown;
        Availability implemented = Availability::Unknown;
        Availability permitted = Availability::Unknown;

        bool operator==(const CapabilityStatus&) const = default;
    };

    struct MethodStatus {
        generated::MethodId method = generated::MethodId::ControllerAcquire;
        Availability available = Availability::Unknown;
        Availability permitted = Availability::Unknown;
        bool controllerRequired = false;
        bool providerReadyRequired = false;
        bool defaultEnabled = false;
        std::vector<FrontendScope> requiredScopes;

        bool operator==(const MethodStatus&) const = default;
    };

    enum class DiagnosticSeverity { Debug, Information, Warning, Error };

    struct Diagnostic {
        DiagnosticSeverity severity = DiagnosticSeverity::Debug;
        std::string message;
        std::optional<ClientErrorCode> code;
        std::optional<ClientError> error;
        PhysicalGeneration generation = 0;

        bool operator==(const Diagnostic&) const = default;
    };

    struct StateChange {
        ConnectionState previous = ConnectionState::Disconnected;
        ConnectionState current = ConnectionState::Disconnected;
        std::optional<ClientError> error;
        PhysicalGeneration generation = 0;

        bool operator==(const StateChange&) const = default;
    };

    struct SynchronizationInfo {
        SyncMode mode = SyncMode::Snapshot;
        model::FrontendSequence synchronizedThrough;
        std::size_t appliedOccurrences = 0;
        std::size_t ignoredOccurrences = 0;
        bool initial = true;
        bool snapshotFallback = false;
        // Exact physical-generation provenance. Public reconnect reporting is
        // derived from this fact, not from whether canonical state existed.
        PhysicalGeneration generation = 0;

        bool operator==(const SynchronizationInfo&) const = default;
    };

    enum class UpdateCause {
        InitialSnapshot,
        InitialReplay,
        ReconnectReplay,
        ProjectionRefresh,
        SnapshotFallback,
        ExplicitSnapshot,
        ExplicitReplay,
        Live,
        ConnectionBecameStale,
        SynchronizationCompleted,
    };

    struct StateReplacedChange {
        bool operator==(const StateReplacedChange&) const = default;
    };
    struct CursorAdvancedChange {
        model::FrontendSequence sequence;
        bool operator==(const CursorAdvancedChange&) const = default;
    };
    struct CompatibilityExtensionChange {
        std::string type;
        model::SafeDetail extensions;
        bool operator==(const CompatibilityExtensionChange&) const = default;
    };

    using Change = std::variant<StateReplacedChange,
                                CursorAdvancedChange,
                                model::ProviderUpdatedOccurrence,
                                model::ControllerUpdatedOccurrence,
                                model::SessionsUpdatedOccurrence,
                                model::ThreadListUpdatedOccurrence,
                                model::ThreadUpsertedOccurrence,
                                model::ThreadRemovedOccurrence,
                                model::TurnUpsertedOccurrence,
                                model::ItemUpsertedOccurrence,
                                model::ItemContentUpdatedOccurrence,
                                model::PendingRequestsUpdatedOccurrence,
                                model::AccountUpdatedOccurrence,
                                model::ModelsUpdatedOccurrence,
                                model::ConfigurationUpdatedOccurrence,
                                model::ProcessUpdatedOccurrence,
                                model::FilesystemWatchUpdatedOccurrence,
                                model::FuzzySearchUpdatedOccurrence,
                                model::ReviewsUpdatedOccurrence,
                                model::IntegrationsUpdatedOccurrence,
                                model::PluginsUpdatedOccurrence,
                                model::SkillsUpdatedOccurrence,
                                model::McpUpdatedOccurrence,
                                model::PlatformUpdatedOccurrence,
                                model::NoticeAddedOccurrence,
                                model::ActivityUpdatedOccurrence,
                                model::CapacityUpdatedOccurrence,
                                model::DiagnosticsUpdatedOccurrence,
                                CompatibilityExtensionChange>;

    struct StateUpdate {
        std::shared_ptr<const PublishedState> state;
        UpdateCause cause = UpdateCause::Live;
        std::optional<model::FrontendSequence> fromSequence;
        std::optional<model::FrontendSequence> toSequence;
        std::vector<Change> changes;
    };

    struct ClientCallbacks {
        std::function<void(const StateChange&)> onConnectionStateChanged;
        // The public SDK uses this private two-phase border to prepare its
        // immutable State before the canonical publication becomes visible.
        // Preparation must not invoke user callbacks. A reported error leaves
        // both authorities at their previous revision.
        std::function<std::optional<ClientError>(const PublishedState&)> prepareStatePublication;
        // Commit is called immediately after the canonical pointer swap and
        // before any semantic publication callback. Implementations must be
        // noexcept and may only expose the already prepared immutable value.
        std::function<void(const PublishedState&)> commitStatePublication;
        std::function<void(std::shared_ptr<const PublishedState>)> onStatePublished;
        std::function<void(const StateUpdate&)> onStateUpdated;
        std::function<void(model::FrontendSequence)> onCursorAdvanced;
        std::function<void(const SynchronizationInfo&)> onSynchronized;
        std::function<void(const ServerMessage&)> onProtocolMessage;
        std::function<void(const Diagnostic&)> onDiagnostic;
        std::function<void(const ClientError&)> onError;
    };

    struct OperationResult {
        std::string requestId;
        generated::MethodId method = generated::MethodId::ControllerAcquire;
        std::optional<generated::CompleteCommandResult> value;
        std::optional<ClientError> error;
        // Present only for an explicit snapshot/replay operation at its
        // terminal SyncComplete boundary.
        std::optional<SynchronizationInfo> synchronization;
        // The physical generation which submitted this operation. Adapter
        // validation failures must never be redirected to a later attachment.
        PhysicalGeneration generation = 0;

        [[nodiscard]] bool succeeded() const noexcept {
            return value.has_value() && !error.has_value();
        }
    };

    using OperationCompletion = std::function<void(const OperationResult&)>;

    struct Submission {
        std::optional<std::string> requestId;
        std::optional<ClientError> error;

        [[nodiscard]] bool accepted() const noexcept {
            return requestId.has_value() && !error.has_value();
        }

        explicit operator bool() const noexcept {
            return accepted();
        }
    };

    class ClientCore {
    public:
        explicit ClientCore(ClientOptions options = {}, ClientCallbacks callbacks = {});
        ~ClientCore();

        ClientCore(const ClientCore&) = delete;
        ClientCore& operator=(const ClientCore&) = delete;
        ClientCore(ClientCore&&) noexcept;
        ClientCore& operator=(ClientCore&&) noexcept;

        [[nodiscard]] std::optional<PhysicalGeneration> attach(TransportCallbacks callbacks);
        void transportConnected(PhysicalGeneration generation);
        void transportDisconnected(PhysicalGeneration generation);
        void transportDisconnected(PhysicalGeneration generation, TransportError error);
        void detach(PhysicalGeneration generation, std::string_view reason = "connection detached");

        [[nodiscard]] bool receive(PhysicalGeneration generation, const ServerMessage& message);
        [[nodiscard]] bool receiveEncoded(PhysicalGeneration generation, std::string_view message);

        [[nodiscard]] Submission submit(generated::CompleteCommandParameters parameters, OperationCompletion completion = {});
        [[nodiscard]] Submission requestSnapshot(OperationCompletion completion = {});
        [[nodiscard]] Submission requestReplay(model::FrontendSequence after, OperationCompletion completion = {});

        void close(std::string_view reason = "client closed");

        [[nodiscard]] ConnectionState connectionState() const noexcept;
        [[nodiscard]] bool ready() const noexcept;
        [[nodiscard]] std::optional<PhysicalGeneration> activeGeneration() const noexcept;
        [[nodiscard]] std::size_t pendingOperationCount() const noexcept;
        [[nodiscard]] std::optional<SessionInfo> sessionInfo() const;
        [[nodiscard]] std::shared_ptr<const PublishedState> state() const noexcept;
        [[nodiscard]] const std::vector<Diagnostic>& diagnostics() const noexcept;

        [[nodiscard]] bool capabilitySelected(FrontendCapability capability) const noexcept;
        [[nodiscard]] CapabilityStatus capabilityStatus(FrontendCapability capability) const noexcept;
        [[nodiscard]] bool methodAvailable(generated::MethodId method) const noexcept;
        [[nodiscard]] bool methodPermitted(generated::MethodId method) const noexcept;
        [[nodiscard]] MethodStatus methodStatus(generated::MethodId method) const noexcept;

        // Reject a result whose public typed adapter validation failed. The
        // submitting generation is explicit so a late failure cannot affect a
        // newer physical attachment.
        void rejectAdapterResult(PhysicalGeneration generation, std::string_view message) noexcept;

    private:
        friend struct ClientCoreTestAccess;
        class Impl;
        std::unique_ptr<Impl> impl;
    };

    // Non-installed friend access keeps deterministic mutation seams out of
    // ClientCore's production-facing internal surface. Tests-enabled builds
    // still reuse the same hidden object code; a separate test object variant
    // would add disproportionate build topology for no public ABI benefit.
    struct ClientCoreTestAccess {
        static void setNextRequestId(ClientCore& core, std::uint64_t next) noexcept;
        static void setGenerationCounter(ClientCore& core, PhysicalGeneration next) noexcept;
        static void setSynchronizationCounts(ClientCore& core, std::size_t received, std::size_t applied, std::size_t ignored) noexcept;
        [[nodiscard]] static bool
        tryAccumulateSynchronizationCounts(ClientCore& core, std::size_t received, std::size_t applied, std::size_t ignored) noexcept;
        [[nodiscard]] static std::array<std::size_t, 3> synchronizationCounts(const ClientCore& core) noexcept;
        static void failAfterNextDispatch(ClientCore& core) noexcept;
        static void setPublishedRevision(ClientCore& core, std::uint64_t revision) noexcept;
        [[nodiscard]] static bool tryCommitPublishedRevision(ClientCore& core, std::uint64_t revision) noexcept;
        [[nodiscard]] static std::size_t erasedTransientBytes(const ClientCore& core) noexcept;
    };

} // namespace ai::openai::codex::frontend::internal::client

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_CLIENT_CLIENTCORE_H
