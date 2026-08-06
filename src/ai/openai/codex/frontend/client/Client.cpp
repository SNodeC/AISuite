/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/Client.h"

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/client/Accounts.h"
#include "ai/openai/codex/frontend/client/Apps.h"
#include "ai/openai/codex/frontend/client/Commands.h"
#include "ai/openai/codex/frontend/client/Configuration.h"
#include "ai/openai/codex/frontend/client/Controller.h"
#include "ai/openai/codex/frontend/client/ExternalAgents.h"
#include "ai/openai/codex/frontend/client/Feedback.h"
#include "ai/openai/codex/frontend/client/Filesystem.h"
#include "ai/openai/codex/frontend/client/GeneratedBindings.h"
#include "ai/openai/codex/frontend/client/Hooks.h"
#include "ai/openai/codex/frontend/client/Marketplace.h"
#include "ai/openai/codex/frontend/client/Mcp.h"
#include "ai/openai/codex/frontend/client/Models.h"
#include "ai/openai/codex/frontend/client/PermissionProfiles.h"
#include "ai/openai/codex/frontend/client/Plugins.h"
#include "ai/openai/codex/frontend/client/ProjectionFingerprint.h"
#include "ai/openai/codex/frontend/client/Provider.h"
#include "ai/openai/codex/frontend/client/Requests.h"
#include "ai/openai/codex/frontend/client/Reviews.h"
#include "ai/openai/codex/frontend/client/Skills.h"
#include "ai/openai/codex/frontend/client/Synchronization.h"
#include "ai/openai/codex/frontend/client/Threads.h"
#include "ai/openai/codex/frontend/client/Turns.h"
#include "ai/openai/codex/frontend/client/WindowsSandbox.h"
#include "ai/openai/codex/frontend/client/detail/BoundOperation.h"
#include "ai/openai/codex/frontend/client/detail/ClientTestAccess.h"
#include "ai/openai/codex/frontend/client/detail/OperationCodecs.h"
#include "ai/openai/codex/frontend/client/detail/StateReducer.h"

#include <algorithm>
#include <any>
#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <map>
#include <ostream>
#include <set>
#include <streambuf>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace ai::openai::codex::frontend::client {
    namespace {
        constexpr std::size_t MaximumContinuityKeyBytes = 256;
        constexpr std::array AllRepresentationCapabilities{
            frontend::FrontendCapability::CompleteBackendDomains,
            frontend::FrontendCapability::DedicatedPendingRequests,
            frontend::FrontendCapability::DedicatedNotificationEvents,
            frontend::FrontendCapability::CompleteThreadItems,
            frontend::FrontendCapability::ScopeProjectedState,
        };

        template <typename Callback>
        class ScopeExit final {
        public:
            explicit ScopeExit(Callback callback) noexcept(std::is_nothrow_move_constructible_v<Callback>)
                : callback(std::move(callback)) {
            }

            ScopeExit(const ScopeExit&) = delete;
            ScopeExit& operator=(const ScopeExit&) = delete;

            ~ScopeExit() noexcept {
                run();
            }

            void run() noexcept {
                if (active) {
                    active = false;
                    callback();
                }
            }

            void release() noexcept {
                active = false;
            }

        private:
            Callback callback;
            bool active = true;
        };

        template <typename Callback>
        ScopeExit(Callback) -> ScopeExit<Callback>;

        Error clientError(ClientErrorCode code, std::string message, bool retryable = false) {
            return Error{ErrorOrigin::Client, code, std::nullopt, std::move(message), std::nullopt, std::nullopt, retryable};
        }

        Error transportError(std::string message, bool retryable) {
            return Error{ErrorOrigin::Transport,
                         ClientErrorCode::TransportFailure,
                         std::nullopt,
                         std::move(message),
                         std::nullopt,
                         std::nullopt,
                         retryable};
        }

        Error protocolError(ClientErrorCode code, std::string message, std::optional<frontend::ErrorCode> protocolCode = std::nullopt) {
            return Error{ErrorOrigin::Protocol, code, protocolCode, std::move(message), std::nullopt, std::nullopt, false};
        }

        bool isRepresentationCapability(frontend::FrontendCapability capability) noexcept {
            switch (capability) {
                case frontend::FrontendCapability::CompleteBackendDomains:
                case frontend::FrontendCapability::DedicatedPendingRequests:
                case frontend::FrontendCapability::DedicatedNotificationEvents:
                case frontend::FrontendCapability::CompleteThreadItems:
                case frontend::FrontendCapability::ScopeProjectedState:
                    return true;
                default:
                    return false;
            }
        }

        bool validCapability(frontend::FrontendCapability capability) noexcept {
            return std::ranges::any_of(
                frontend::generated::AllCapabilities, [capability](const frontend::generated::CapabilityMetadata& metadata) {
                    return metadata.id == static_cast<frontend::generated::Capability>(capability) && metadata.defined;
                });
        }

        const frontend::generated::CapabilityMetadata* capabilityMetadata(frontend::FrontendCapability capability) noexcept {
            const auto found = std::ranges::find_if(frontend::generated::AllCapabilities,
                                                    [capability](const frontend::generated::CapabilityMetadata& metadata) {
                                                        return metadata.id == static_cast<frontend::generated::Capability>(capability);
                                                    });
            return found == frontend::generated::AllCapabilities.end() ? nullptr : &*found;
        }

        const frontend::generated::MethodMetadata* methodMetadata(frontend::generated::MethodId method) noexcept {
            const auto found = std::ranges::find_if(frontend::generated::AllMethods,
                                                    [method](const frontend::generated::MethodMetadata& metadata) {
                                                        return metadata.id == method;
                                                    });
            return found == frontend::generated::AllMethods.end() ? nullptr : &*found;
        }

        class BoundedJsonStreamBuffer final : public std::streambuf {
        public:
            explicit BoundedJsonStreamBuffer(std::size_t maximum) noexcept
                : maximum(maximum) {
            }

            [[nodiscard]] bool exceeded() const noexcept {
                return overCapacity;
            }

        protected:
            std::streamsize xsputn(const char*, std::streamsize count) override {
                if (count <= 0) {
                    return count;
                }
                const auto bytes = static_cast<std::uintmax_t>(count);
                const std::size_t remaining = maximum - retained;
                if (bytes > remaining) {
                    retained = maximum;
                    overCapacity = true;
                } else {
                    retained += static_cast<std::size_t>(bytes);
                }
                return count;
            }

            int_type overflow(int_type character) override {
                if (traits_type::eq_int_type(character, traits_type::eof())) {
                    return traits_type::not_eof(character);
                }
                if (retained == maximum) {
                    overCapacity = true;
                } else {
                    ++retained;
                }
                return character;
            }

        private:
            std::size_t maximum;
            std::size_t retained = 0;
            bool overCapacity = false;
        };

        std::optional<bool> jsonFitsBound(const frontend::Json& message, std::size_t maximum) noexcept {
            try {
                BoundedJsonStreamBuffer buffer(maximum);
                std::ostream stream(&buffer);
                stream << message;
                if (!stream.good()) {
                    return std::nullopt;
                }
                return !buffer.exceeded();
            } catch (...) {
                return std::nullopt;
            }
        }

        template <typename T>
        bool contains(const std::vector<T>& values, T value) {
            return std::find(values.begin(), values.end(), value) != values.end();
        }

        std::size_t securelyErase(std::string& value, bool* completeStorageWasZeroed = nullptr) noexcept {
            const std::size_t bytes = value.capacity();
            bool zeroed = false;
            try {
                // A moved-from or cleared short string may retain bytes beyond
                // size() in its inline storage. Growing only to the existing
                // capacity cannot allocate and makes that complete storage a
                // writable character range before the volatile overwrite.
                value.resize(bytes, '\0');
                volatile char* data = value.empty() ? nullptr : value.data();
                for (std::size_t index = 0; index < value.size(); ++index) {
                    data[index] = '\0';
                }
                zeroed = std::ranges::all_of(value, [](char character) { return character == '\0'; });
                value.clear();
            } catch (...) {
                // No allocation is expected when resizing to capacity, but a
                // defensive fallback still wipes every currently addressable
                // character without allowing an exception to escape.
                volatile char* data = value.empty() ? nullptr : value.data();
                for (std::size_t index = 0; index < value.size(); ++index) {
                    data[index] = '\0';
                }
                zeroed = value.empty() || std::ranges::all_of(value, [](char character) { return character == '\0'; });
                value.clear();
            }
            if (completeStorageWasZeroed != nullptr) {
                *completeStorageWasZeroed = zeroed;
            }
            return bytes;
        }

        std::size_t securelyErase(frontend::Json& value) noexcept {
            std::size_t bytes = 0;
            try {
                if (value.is_string()) {
                    bytes = securelyErase(value.get_ref<std::string&>());
                } else if (value.is_array() || value.is_object()) {
                    for (frontend::Json& member : value) {
                        const std::size_t erased = securelyErase(member);
                        bytes = erased > std::numeric_limits<std::size_t>::max() - bytes
                                    ? std::numeric_limits<std::size_t>::max()
                                    : bytes + erased;
                    }
                }
                value = nullptr;
            } catch (...) {
                // Wiping is a best-effort containment boundary and must never
                // escape through the event loop.
            }
            return bytes;
        }

        std::size_t securelyErase(frontend::generated::DefinedCommand& command) noexcept {
            std::size_t bytes = std::visit([](auto& parameters) { return securelyErase(parameters.value); }, command.parameters);
            for (frontend::Json* value : {&command.extensions, &command.parameterExtensions}) {
                const std::size_t erased = securelyErase(*value);
                bytes = erased > std::numeric_limits<std::size_t>::max() - bytes ? std::numeric_limits<std::size_t>::max()
                                                                                : bytes + erased;
            }
            return bytes;
        }

        std::size_t securelyErase(frontend::AuthenticationCredential& credential) noexcept {
            std::size_t bytes = 0;
            if (auto* bearer = std::get_if<frontend::BearerCredential>(&credential)) {
                bytes = securelyErase(bearer->token);
            }
            credential = frontend::NoCredential{};
            return bytes;
        }

        std::size_t securelyErase(frontend::ClientMessage& message) noexcept {
            auto* hello = std::get_if<frontend::Hello>(&message);
            if (hello == nullptr) {
                return 0;
            }
            std::size_t bytes = securelyErase(hello->extensions);
            if (hello->authentication) {
                const std::size_t erased = securelyErase(*hello->authentication);
                bytes = erased > std::numeric_limits<std::size_t>::max() - bytes ? std::numeric_limits<std::size_t>::max()
                                                                                : bytes + erased;
                hello->authentication.reset();
            }
            return bytes;
        }
    } // namespace

    struct Connection::Control {
        Client* owner = nullptr;
        TransportCallbacks transport;
        std::uint64_t generation = 0;
        bool open = true;
        bool connected = false;
        bool connectedReported = false;
        bool closeRequested = false;
    };

    struct Client::Impl {
        struct PendingOperation {
            RequestId requestId;
            frontend::generated::MethodId method;
            detail::BoundOperationCompletion completion;
        };

        struct DeferredCommand {
            std::string requestId;
            OutboundMessage message;
        };

        struct ExplicitSynchronization {
            frontend::SyncMode requestedMode = frontend::SyncMode::Snapshot;
            frontend::SyncMode streamMode = frontend::SyncMode::Snapshot;
            CompletionHandler<SynchronizationResult> handler;
            GeneratedCompletionHandler generatedHandler;
            std::optional<frontend::generated::CompleteCommandResult> generatedResult;
            std::optional<RequestId> requestId;
            bool responseAccepted = false;
            bool snapshotFallback = false;
            bool streamCompleted = false;
        };

        explicit Impl(Client& client, ClientOptions clientOptions, ClientCallbacks clientCallbacks)
            : owner(&client)
            , options(std::move(clientOptions))
            , callbacks(std::move(clientCallbacks)) {
        }

        Client* owner;
        ClientOptions options;
        ClientCallbacks callbacks;
        ConnectionState connectionState = ConnectionState::Disconnected;
        std::shared_ptr<Connection::Control> active;
        std::uint64_t nextConnectionGeneration = 1;
        std::uint64_t nextRequest = 1;
        std::map<std::string, PendingOperation, std::less<>> pending;
        std::deque<DeferredCommand> deferredCommands;
        std::size_t dispatchDepth = 0;
        bool flushingDeferredCommands = false;
        bool clientCloseInProgress = false;
        bool detachCallbacksInProgress = false;
        std::size_t erasedTransientBytes = 0;
        std::size_t verifiedMovedFromStringScrubs = 0;
        bool failNextHelloConstructionForTesting = false;
        bool failAfterNextDispatchForTesting = false;
        std::optional<SessionInfo> currentSession;
        std::optional<frontend::CapabilityAdvertisement> currentCapabilities;
        std::optional<frontend::SequenceNumber> currentVisibleSequence;
        std::optional<frontend::SequenceNumber> currentSynchronizedThrough;
        State currentState;
        std::optional<std::string> retainedContinuityKey;
        std::optional<std::string> activeContinuityKey;
        std::optional<frontend::SequenceNumber> helloResumeAfterSent;
        bool initialSnapshotFallback = false;
        std::optional<std::string> retainedProjectionFingerprint;
        std::optional<std::string> activeProjectionFingerprint;
        bool sawSnapshot = false;
        bool sawEvents = false;
        std::size_t synchronizationReceivedEvents = 0;
        std::size_t synchronizationAppliedEvents = 0;
        std::size_t synchronizationIgnoredEvents = 0;
        std::optional<ExplicitSynchronization> explicitSynchronization;
        bool projectionRefreshRequired = false;
        bool projectionSnapshotStreaming = false;
        std::optional<std::string> projectionSnapshotRequestId;
        std::optional<State> projectionValidationState;

        std::unique_ptr<Controller> controller;
        std::unique_ptr<Provider> provider;
        std::unique_ptr<Synchronization> synchronization;
        std::unique_ptr<Accounts> accounts;
        std::unique_ptr<Apps> apps;
        std::unique_ptr<Commands> commands;
        std::unique_ptr<Configuration> configuration;
        std::unique_ptr<ExternalAgents> externalAgents;
        std::unique_ptr<Feedback> feedback;
        std::unique_ptr<Filesystem> filesystem;
        std::unique_ptr<Hooks> hooks;
        std::unique_ptr<Marketplace> marketplace;
        std::unique_ptr<Mcp> mcp;
        std::unique_ptr<Models> models;
        std::unique_ptr<PermissionProfiles> permissionProfiles;
        std::unique_ptr<Plugins> plugins;
        std::unique_ptr<Requests> requests;
        std::unique_ptr<Reviews> reviews;
        std::unique_ptr<Skills> skills;
        std::unique_ptr<Threads> threads;
        std::unique_ptr<Turns> turns;
        std::unique_ptr<WindowsSandbox> windowsSandbox;

        [[nodiscard]] bool owns(const Connection::Control& control) const noexcept {
            return active && active.get() == &control && control.owner == owner && control.open;
        }

        void accountErasedTransientBytes(std::size_t bytes) noexcept {
            erasedTransientBytes = bytes > std::numeric_limits<std::size_t>::max() - erasedTransientBytes
                                       ? std::numeric_limits<std::size_t>::max()
                                       : erasedTransientBytes + bytes;
        }

        void eraseTransientString(std::string& value, bool movedFrom = false) noexcept {
            bool completeStorageWasZeroed = false;
            accountErasedTransientBytes(securelyErase(value, &completeStorageWasZeroed));
            if (movedFrom && completeStorageWasZeroed && verifiedMovedFromStringScrubs != std::numeric_limits<std::size_t>::max()) {
                ++verifiedMovedFromStringScrubs;
            }
        }

        void eraseOwnedOutbound(OutboundMessage& message, bool movedFrom = false) noexcept {
            eraseTransientString(message.compactJson, movedFrom);
            message.serializedBytes = 0;
        }

        [[nodiscard]] bool tryAccumulateSynchronizationCounts(std::size_t received,
                                                              std::size_t applied,
                                                              std::size_t ignored) noexcept {
            if (received > std::numeric_limits<std::size_t>::max() - synchronizationReceivedEvents ||
                applied > std::numeric_limits<std::size_t>::max() - synchronizationAppliedEvents ||
                ignored > std::numeric_limits<std::size_t>::max() - synchronizationIgnoredEvents) {
                return false;
            }
            synchronizationReceivedEvents += received;
            synchronizationAppliedEvents += applied;
            synchronizationIgnoredEvents += ignored;
            return true;
        }

        [[nodiscard]] std::optional<ProjectionFingerprintMetadata> activeProjectionMetadata() const {
            if (!activeProjectionFingerprint)
                return std::nullopt;
            return ProjectionFingerprintMetadata{*activeProjectionFingerprint};
        }

        [[nodiscard]] std::size_t outstandingOperationCount() const noexcept {
            std::size_t count = pending.size();
            if (explicitSynchronization && explicitSynchronization->responseAccepted && count != std::numeric_limits<std::size_t>::max()) {
                ++count;
            }
            if ((projectionSnapshotRequestId || projectionSnapshotStreaming) && count != std::numeric_limits<std::size_t>::max()) {
                ++count;
            }
            return count;
        }

        void diagnostic(Diagnostic::Severity severity, std::string message, std::optional<Error> error = std::nullopt) noexcept {
            if (!callbacks.onDiagnostic) {
                return;
            }
            try {
                callbacks.onDiagnostic(Diagnostic{severity, std::move(message), std::move(error)});
            } catch (...) {
            }
        }

        void notifyConnectionStateChanged(ConnectionState previous,
                                          ConnectionState next,
                                          std::optional<Error> error = std::nullopt) noexcept {
            if (!callbacks.onConnectionStateChanged) {
                return;
            }
            try {
                callbacks.onConnectionStateChanged(ConnectionStateChange{previous, next, std::move(error)});
            } catch (...) {
                diagnostic(Diagnostic::Severity::Warning,
                           "frontend client connection-state callback threw",
                           clientError(ClientErrorCode::CallbackFailure, "connection-state callback failed"));
            }
        }

        void transition(ConnectionState next, std::optional<Error> error = std::nullopt) noexcept {
            if (connectionState == next || (connectionState == ConnectionState::Closed && next != ConnectionState::Closed)) {
                return;
            }
            const ConnectionState previous = connectionState;
            connectionState = next;
            notifyConnectionStateChanged(previous, next, std::move(error));
        }

        void requestTransportClose(Connection::Control& control, std::string reason) noexcept {
            if (control.closeRequested) {
                return;
            }
            control.closeRequested = true;
            try {
                control.transport.close(std::move(reason));
            } catch (...) {
                diagnostic(Diagnostic::Severity::Warning,
                           "frontend client transport close callback threw",
                           clientError(ClientErrorCode::CallbackFailure, "transport close callback failed"));
            }
        }

        [[nodiscard]] std::map<std::string, PendingOperation, std::less<>> takePending() noexcept {
            for (DeferredCommand& deferred : deferredCommands) {
                eraseOwnedOutbound(deferred.message);
            }
            deferredCommands.clear();
            auto operations = std::move(pending);
            pending.clear();
            return operations;
        }

        void completeOperations(std::map<std::string, PendingOperation, std::less<>> operations, const Error& error) noexcept {
            for (auto& [ignored, operation] : operations) {
                (void) ignored;
                if (!operation.completion.fail) {
                    continue;
                }
                try {
                    operation.completion.fail(operation.requestId, error);
                } catch (...) {
                    diagnostic(Diagnostic::Severity::Warning,
                               "frontend client operation callback threw",
                               clientError(ClientErrorCode::CallbackFailure, "operation callback failed"));
                }
            }
        }

        void completePending(const Error& error) noexcept {
            completeOperations(takePending(), error);
        }

        void invokeExplicitSynchronizationFailure(ExplicitSynchronization operation, const Error& error) noexcept {
            const RequestId requestId = operation.requestId.value_or(RequestId{});
            if (operation.handler) {
                try {
                    operation.handler(OperationResult<SynchronizationResult>{requestId, std::nullopt, error});
                } catch (...) {
                    diagnostic(Diagnostic::Severity::Warning,
                               "frontend synchronization callback threw",
                               clientError(ClientErrorCode::CallbackFailure, "synchronization callback failed"));
                }
            }
            if (operation.generatedHandler) {
                try {
                    operation.generatedHandler(GeneratedOperationResult{requestId, std::nullopt, error});
                } catch (...) {
                    diagnostic(Diagnostic::Severity::Warning,
                               "frontend synchronization callback threw",
                               clientError(ClientErrorCode::CallbackFailure, "synchronization callback failed"));
                }
            }
        }

        void invokeExplicitSynchronizationSuccess(ExplicitSynchronization operation,
                                                  SynchronizationResult result) noexcept {
            const RequestId requestId = operation.requestId.value_or(RequestId{});
            if (operation.handler) {
                try {
                    operation.handler(OperationResult<SynchronizationResult>{requestId, result, std::nullopt});
                } catch (...) {
                    diagnostic(Diagnostic::Severity::Warning,
                               "frontend synchronization callback threw",
                               clientError(ClientErrorCode::CallbackFailure, "synchronization callback failed"));
                }
            }
            if (operation.generatedHandler) {
                try {
                    if (operation.generatedResult) {
                        operation.generatedHandler(
                            GeneratedOperationResult{requestId, std::move(operation.generatedResult), std::nullopt});
                    } else {
                        operation.generatedHandler(GeneratedOperationResult{
                            requestId,
                            std::nullopt,
                            protocolError(ClientErrorCode::StateDivergence,
                                          "generated synchronization completed without its validated command result")});
                    }
                } catch (...) {
                    diagnostic(Diagnostic::Severity::Warning,
                               "frontend synchronization callback threw",
                               clientError(ClientErrorCode::CallbackFailure, "synchronization callback failed"));
                }
            }
        }

        void failExplicitSynchronization(const Error& error, bool restoreReady = false) noexcept {
            if (!explicitSynchronization) {
                return;
            }
            ExplicitSynchronization operation = std::move(*explicitSynchronization);
            explicitSynchronization.reset();
            if (restoreReady && active && active->connected && connectionState == ConnectionState::Synchronizing) {
                transition(ConnectionState::Ready);
            }
            invokeExplicitSynchronizationFailure(std::move(operation), error);
        }

        void detach(Connection::Control& control, std::optional<Error> error, bool terminal) noexcept {
            if (!active || active.get() != &control) {
                return;
            }
            const ConnectionState previousState = connectionState;
            const ConnectionState detachedState = terminal ? ConnectionState::Closed : ConnectionState::Disconnected;
            control.open = false;
            control.connected = false;
            control.owner = nullptr;
            currentSession.reset();
            currentCapabilities.reset();
            helloResumeAfterSent.reset();
            initialSnapshotFallback = false;
            projectionRefreshRequired = false;
            projectionSnapshotStreaming = false;
            projectionSnapshotRequestId.reset();
            projectionValidationState.reset();
            const Error disconnectError = error.value_or(
                terminal ? clientError(ClientErrorCode::Closed, "frontend client closed")
                         : clientError(ClientErrorCode::NotConnected, "frontend client connection closed", true));

            std::optional<detail::StateReduction> staleReduction;
            std::optional<Error> staleCapacityError;
            if (currentState.revision() != 0 || currentState.visibleSequence() || currentState.synchronizedThrough() ||
                currentState.session() || currentState.freshness() != StateFreshness::Stale ||
                currentState.representationMode() != RepresentationMode::Unknown) {
                std::string staleError;
                auto staleState = detail::StateReducer::stale(currentState, options.maximumDecodedStateBytes, staleError);
                if (!staleState) {
                    // The reducer already attempted a bounded empty fallback.
                    // Keep disconnect/close noexcept even for an impossible
                    // configuration below the minimum empty-State footprint.
                    staleState = detail::StateReducer::initial();
                    staleCapacityError = clientError(
                        ClientErrorCode::StateCapacityExceeded,
                        staleError.empty() ? "stale frontend state exceeded capacity" : std::move(staleError));
                }
                staleReduction = detail::StateReduction{std::move(*staleState), {}, 0, 0, 0};
                currentState = staleReduction->state;
                currentVisibleSequence = currentState.visibleSequence();
                currentSynchronizedThrough = currentState.synchronizedThrough();
            }
            auto operations = takePending();
            std::optional<ExplicitSynchronization> synchronizationFailure;
            if (explicitSynchronization) {
                synchronizationFailure = std::move(*explicitSynchronization);
                explicitSynchronization.reset();
            }
            active.reset();
            connectionState = detachedState;

            detachCallbacksInProgress = true;
            if (staleCapacityError) {
                diagnostic(Diagnostic::Severity::Warning,
                           "frontend retained state was discarded while marking the connection stale",
                           std::move(staleCapacityError));
            }
            if (staleReduction) {
                notifyStateUpdated({}, UpdateCause::ConnectionBecameStale);
            }
            completeOperations(std::move(operations), disconnectError);
            if (synchronizationFailure) {
                invokeExplicitSynchronizationFailure(std::move(*synchronizationFailure), disconnectError);
            }
            detachCallbacksInProgress = false;
            if (connectionState == detachedState) {
                notifyConnectionStateChanged(previousState, detachedState, std::move(error));
            }
        }

        void fail(Connection::Control& control, Error error, std::string closeReason) noexcept {
            if (!owns(control)) {
                return;
            }
            transition(ConnectionState::Closing, error);
            if (!owns(control)) {
                return;
            }
            requestTransportClose(control, std::move(closeReason));
            detach(control, std::move(error), false);
        }

        void invokeProtocolObservation(const frontend::ServerMessage& message) noexcept {
            if (!callbacks.onProtocolMessage) {
                return;
            }
            try {
                callbacks.onProtocolMessage(message);
            } catch (...) {
                diagnostic(Diagnostic::Severity::Warning,
                           "frontend client protocol-observation callback threw",
                           clientError(ClientErrorCode::CallbackFailure, "protocol-observation callback failed"));
            }
        }

        void notifyStateUpdated(std::vector<Change> changes,
                                UpdateCause cause,
                                std::optional<frontend::SequenceNumber> from = std::nullopt,
                                std::optional<frontend::SequenceNumber> to = std::nullopt) noexcept {
            if (!callbacks.onStateUpdated) {
                return;
            }
            try {
                callbacks.onStateUpdated(StateUpdate{currentState, cause, from, to, std::move(changes)});
            } catch (...) {
                diagnostic(Diagnostic::Severity::Warning,
                           "frontend client state-update callback threw",
                           clientError(ClientErrorCode::CallbackFailure, "state-update callback failed"));
            }
        }

        void stateUpdated(detail::StateReduction reduction,
                          UpdateCause cause,
                          std::optional<frontend::SequenceNumber> from = std::nullopt,
                          std::optional<frontend::SequenceNumber> to = std::nullopt) {
            currentState = std::move(reduction.state);
            currentVisibleSequence = currentState.visibleSequence();
            currentSynchronizedThrough = currentState.synchronizedThrough();
            notifyStateUpdated(std::move(reduction.changes), cause, from, to);
        }

        void sendHelloWithAuthentication(Connection::Control& control, AuthenticationContext&& authentication) {
            auto credentialGuard = ScopeExit([this, &authentication]() noexcept {
                accountErasedTransientBytes(securelyErase(authentication.credential));
            });
            if (!owns(control) || !control.connected) {
                return;
            }
            if (authentication.continuityKey && authentication.continuityKey->size() > MaximumContinuityKeyBytes) {
                fail(control,
                     clientError(ClientErrorCode::InvalidConfiguration, "continuity key exceeds its resource bound"),
                     "frontend continuity key rejected");
                return;
            }
            try {
                std::optional<frontend::SequenceNumber> resumeAfter;
                if (currentSynchronizedThrough && authentication.continuityKey && retainedContinuityKey == authentication.continuityKey) {
                    resumeAfter = currentSynchronizedThrough;
                }
                activeContinuityKey = authentication.continuityKey;
                helloResumeAfterSent = resumeAfter;
                const bool sensitive = std::holds_alternative<frontend::BearerCredential>(authentication.credential);
                frontend::ClientMessage wireHello{
                    frontend::Hello{resumeAfter, frontend::Json::object(), options.requestedCapabilities, std::nullopt}};
                auto helloGuard = ScopeExit([this, &wireHello]() noexcept {
                    accountErasedTransientBytes(securelyErase(wireHello));
                });
                if (sensitive) {
                    // Copy directly into the final Hello object, then wipe the
                    // still-sized provider result. Moving a short-string token
                    // first would leave its SSO bytes behind a size-zero source.
                    std::get<frontend::Hello>(wireHello).authentication = authentication.credential;
                }
                credentialGuard.run();
                if (failNextHelloConstructionForTesting) {
                    failNextHelloConstructionForTesting = false;
                    throw std::runtime_error("injected frontend Hello construction failure");
                }
                auto serialized = frontend::Codec::serializeClient(wireHello);
                helloGuard.run();
                if (!serialized) {
                    fail(control,
                         clientError(ClientErrorCode::SerializationFailed, "failed to serialize frontend Hello"),
                         "frontend Hello serialization failed");
                    return;
                }
                std::string&& serializedStorage = std::move(serialized).value();
                std::string serializedHello(std::move(serializedStorage));
                eraseTransientString(serializedStorage, true);
                OutboundMessage message{OutboundKind::Hello, std::move(serializedHello), 0, sensitive};
                eraseTransientString(serializedHello, true);
                message.serializedBytes = message.compactJson.size();
                SendResult sendResult;
                try {
                    sendResult = control.transport.send(std::move(message));
                } catch (...) {
                    eraseOwnedOutbound(message, true);
                    fail(control, transportError("transport send callback failed", true), "frontend transport send failed");
                    return;
                }
                eraseOwnedOutbound(message, true);
                if (!owns(control) || !control.connected) {
                    return;
                }
                if (sendResult.status != SendStatus::Accepted) {
                    const bool retryable = sendResult.error && sendResult.error->retryable;
                    fail(control, transportError("transport rejected frontend Hello", retryable), "frontend Hello rejected");
                    return;
                }
                transition(ConnectionState::Authenticating);
            } catch (...) {
                credentialGuard.run();
                if (owns(control)) {
                    fail(control,
                         clientError(ClientErrorCode::SerializationFailed, "frontend Hello construction failed"),
                         "frontend Hello construction failed");
                }
            }
        }

        void sendHello(Connection::Control& control) noexcept {
            try {
                // Bind the provider result directly for the duration of the
                // guarded handoff. A default construction followed by move
                // assignment can leave short credential bytes in the prior
                // inline string storage.
                sendHelloWithAuthentication(control, options.credentialProvider());
            } catch (...) {
                if (owns(control)) {
                    fail(control,
                         clientError(ClientErrorCode::InvalidConfiguration, "credential provider failed"),
                         "frontend credential provider failed");
                }
            }
        }

        void handleWelcome(Connection::Control& control, const frontend::Welcome& welcome, bool& semanticallyAccepted) {
            if (connectionState != ConnectionState::Authenticating || currentSession) {
                fail(control,
                     protocolError(ClientErrorCode::UnexpectedMessage, "unexpected or duplicate Welcome"),
                     "frontend protocol violation");
                return;
            }
            SessionInfo info;
            info.sessionId = welcome.sessionId;
            info.role = welcome.role;
            info.syncMode = welcome.syncMode;
            info.serverCurrentSequence = welcome.currentSequence;
            info.serverVersion = welcome.serverVersion;
            info.requestedRepresentationCapabilities = options.requestedCapabilities;
            if (welcome.capabilities) {
                const auto validUniqueCapabilities = [](const std::vector<frontend::FrontendCapability>& capabilities) {
                    std::set<frontend::FrontendCapability> unique;
                    return std::ranges::all_of(capabilities, [&unique](frontend::FrontendCapability capability) {
                        return validCapability(capability) && unique.insert(capability).second;
                    });
                };
                const bool inconsistentAdvertisement =
                    !validUniqueCapabilities(welcome.capabilities->defined) ||
                    !validUniqueCapabilities(welcome.capabilities->implemented) ||
                    !validUniqueCapabilities(welcome.capabilities->permitted) ||
                    std::ranges::any_of(welcome.capabilities->implemented, [&](frontend::FrontendCapability capability) {
                        return !contains(welcome.capabilities->defined, capability);
                    }) ||
                    std::ranges::any_of(welcome.capabilities->permitted, [&](frontend::FrontendCapability capability) {
                        return !contains(welcome.capabilities->implemented, capability);
                    });
                if (inconsistentAdvertisement) {
                    fail(control,
                         protocolError(ClientErrorCode::UnexpectedMessage,
                                       "frontend capability advertisement has inconsistent defined/implemented/permitted sets"),
                         "frontend capability advertisement rejected");
                    return;
                }
                currentCapabilities = *welcome.capabilities;
                for (const frontend::FrontendCapability requested : options.requestedCapabilities) {
                    if (contains(welcome.capabilities->implemented, requested) && contains(welcome.capabilities->permitted, requested)) {
                        info.selectedRepresentationCapabilities.push_back(requested);
                    }
                }
                for (const frontend::FrontendCapability capability : welcome.capabilities->implemented) {
                    const auto* metadata = capabilityMetadata(capability);
                    if (metadata == nullptr || metadata->category == frontend::generated::CapabilityCategory::StaticMechanism) {
                        info.observedMechanismCapabilities.push_back(capability);
                    } else if (metadata->category == frontend::generated::CapabilityCategory::ConditionalTopology) {
                        info.observedTopologyCapabilities.push_back(capability);
                    } else {
                        info.observedProductCapabilities.push_back(capability);
                    }
                }
                for (const frontend::FrontendCapability required : options.requiredCapabilities) {
                    if (!contains(welcome.capabilities->implemented, required) ||
                        !contains(welcome.capabilities->permitted, required)) {
                        fail(control,
                             protocolError(ClientErrorCode::UnexpectedMessage, "required frontend capability is unavailable"),
                             "required frontend capability unavailable");
                        return;
                    }
                }
            } else {
                currentCapabilities.reset();
                if (!options.requiredCapabilities.empty() || !options.allowLegacyV1) {
                    fail(control,
                         protocolError(ClientErrorCode::UnexpectedMessage,
                                       options.allowLegacyV1 ? "required capability advertisement is absent"
                                                             : "legacy Frontend Protocol v1 is disabled"),
                         "required frontend capability unavailable");
                    return;
                }
            }
            if (!options.allowLegacyV1 &&
                !std::ranges::all_of(AllRepresentationCapabilities, [&info](frontend::FrontendCapability capability) {
                    return contains(info.selectedRepresentationCapabilities, capability);
                })) {
                fail(control,
                     protocolError(ClientErrorCode::UnexpectedMessage, "expanded Frontend Protocol v1 representation is unavailable"),
                     "expanded frontend representation unavailable");
                return;
            }
            const auto decodeMethodSet = [&](const std::optional<std::vector<frontend::FrontendMethod>>& encoded,
                                             std::optional<std::vector<frontend::generated::MethodId>>& decoded) {
                if (!encoded) {
                    return true;
                }
                decoded.emplace();
                std::set<frontend::generated::MethodId> unique;
                for (const std::string& method : *encoded) {
                    const auto id = frontend::generated::definedMethodFromString(method);
                    if (!id || !unique.insert(*id).second) {
                        fail(control,
                             protocolError(ClientErrorCode::UnexpectedMessage,
                                           "frontend method discovery contains an unknown or duplicate method"),
                             "frontend method discovery rejected");
                        return false;
                    }
                    decoded->push_back(*id);
                }
                return true;
            };
            if (!decodeMethodSet(welcome.availableMethods, info.availableMethods) ||
                !decodeMethodSet(welcome.permittedMethods, info.permittedMethods)) {
                return;
            }
            if (const auto scopes = welcome.extensions.find("permittedScopes"); scopes != welcome.extensions.end()) {
                if (!scopes->is_array()) {
                    fail(control,
                         protocolError(ClientErrorCode::UnexpectedMessage, "permittedScopes projection metadata must be an array"),
                         "frontend projection metadata rejected");
                    return;
                }
                info.permittedScopes.emplace();
                std::set<frontend::FrontendScope> uniqueScopes;
                for (const frontend::Json& scope : *scopes) {
                    if (!scope.is_string()) {
                        fail(control,
                             protocolError(ClientErrorCode::UnexpectedMessage,
                                           "permittedScopes projection metadata contains a non-string value"),
                             "frontend projection metadata rejected");
                        return;
                    }
                    const auto parsed = frontend::frontendScopeFromString(scope.get<std::string>());
                    if (!parsed || !uniqueScopes.insert(*parsed).second) {
                        fail(control,
                             protocolError(ClientErrorCode::UnexpectedMessage,
                                           "permittedScopes projection metadata contains an unknown or duplicate scope"),
                             "frontend projection metadata rejected");
                        return;
                    }
                    info.permittedScopes->push_back(*parsed);
                }
            }
            std::optional<frontend::Json> projectionMetadata;
            if (const auto projection = welcome.extensions.find("projection"); projection != welcome.extensions.end()) {
                projectionMetadata = *projection;
            }
            activeProjectionFingerprint = projectionFingerprint(ProjectionFingerprintInput{info.requestedRepresentationCapabilities,
                                                                                           info.selectedRepresentationCapabilities,
                                                                                           activeContinuityKey,
                                                                                           info.permittedScopes,
                                                                                           info.permittedMethods,
                                                                                           info.availableMethods,
                                                                                           std::move(projectionMetadata)});
            const bool completeProjectionContinuityMetadata =
                info.permittedScopes.has_value() && info.permittedMethods.has_value() && info.availableMethods.has_value();
            projectionRefreshRequired = currentState.synchronizedThrough() && welcome.syncMode == frontend::SyncMode::Replay &&
                                        (!helloResumeAfterSent || !completeProjectionContinuityMetadata || !retainedProjectionFingerprint ||
                                         *retainedProjectionFingerprint != *activeProjectionFingerprint);
            initialSnapshotFallback = helloResumeAfterSent.has_value() && welcome.syncMode == frontend::SyncMode::Snapshot;
            projectionSnapshotStreaming = false;
            projectionSnapshotRequestId.reset();
            projectionValidationState.reset();
            if (projectionRefreshRequired) {
                std::string stagingError;
                projectionValidationState = detail::StateReducer::synchronizationStaging(
                    info,
                    helloResumeAfterSent,
                    options.maximumDecodedStateBytes,
                    options.allowLegacyV1,
                    stagingError,
                    activeProjectionMetadata());
                if (!projectionValidationState) {
                    fail(control,
                         protocolError(ClientErrorCode::StateDivergence, std::move(stagingError)),
                         "frontend projection validation state could not be established");
                    return;
                }
            } else if (welcome.syncMode == frontend::SyncMode::Replay && !currentState.synchronizedThrough()) {
                std::string stagingError;
                auto initialReplayState = detail::StateReducer::synchronizationStaging(
                    info,
                    helloResumeAfterSent,
                    options.maximumDecodedStateBytes,
                    options.allowLegacyV1,
                    stagingError,
                    activeProjectionMetadata());
                if (!initialReplayState) {
                    fail(control,
                         protocolError(ClientErrorCode::StateDivergence, std::move(stagingError)),
                         "frontend initial replay state could not be established");
                    return;
                }
                currentState = std::move(*initialReplayState);
                currentVisibleSequence = currentState.visibleSequence();
                currentSynchronizedThrough = currentState.synchronizedThrough();
            } else if (welcome.syncMode == frontend::SyncMode::Replay && currentState.synchronizedThrough()) {
                std::string rebindError;
                auto reboundState = detail::StateReducer::beginSynchronization(
                    currentState, info, options.maximumDecodedStateBytes, rebindError, activeProjectionMetadata());
                if (!reboundState) {
                    fail(control,
                         protocolError(ClientErrorCode::StateDivergence, std::move(rebindError)),
                         "frontend retained replay state could not be rebound to its new session");
                    return;
                }
                currentState = std::move(*reboundState);
                currentVisibleSequence = currentState.visibleSequence();
                currentSynchronizedThrough = currentState.synchronizedThrough();
            }
            currentSession = std::move(info);
            sawSnapshot = false;
            sawEvents = false;
            synchronizationReceivedEvents = 0;
            synchronizationAppliedEvents = 0;
            synchronizationIgnoredEvents = 0;
            transition(ConnectionState::Synchronizing);
            semanticallyAccepted = true;
        }

        void failOperation(PendingOperation operation, const Error& error) noexcept {
            if (!operation.completion.fail) {
                return;
            }
            try {
                operation.completion.fail(operation.requestId, error);
            } catch (...) {
                diagnostic(Diagnostic::Severity::Warning,
                           "frontend client operation callback threw",
                           clientError(ClientErrorCode::CallbackFailure, "operation callback failed"));
            }
        }

        bool succeedOperation(Connection::Control& control,
                              PendingOperation operation,
                              frontend::generated::CompleteCommandResult generatedResult) {
            std::any decodedResult;
            std::string decodeError;
            bool decoded = false;
            try {
                decoded = operation.completion.decode && operation.completion.decode(generatedResult, decodedResult, decodeError);
            } catch (...) {
                decoded = false;
            }
            if (!decoded) {
                const Error error = protocolError(ClientErrorCode::ResponseTypeMismatch, "frontend response failed typed result decoding");
                failOperation(std::move(operation), error);
                fail(control, error, "frontend response typed decoding failed");
                return false;
            }
            if (!operation.completion.succeed) {
                return true;
            }
            try {
                operation.completion.succeed(operation.requestId, std::move(decodedResult));
            } catch (...) {
                diagnostic(Diagnostic::Severity::Warning,
                           "frontend client operation callback threw",
                           clientError(ClientErrorCode::CallbackFailure, "operation callback failed"));
            }
            return true;
        }

        bool requestProjectionSnapshot(Connection::Control& control) {
            if (outstandingOperationCount() >= options.maximumPendingOperations) {
                fail(control,
                     clientError(ClientErrorCode::TooManyPendingOperations,
                                 "frontend pending-operation capacity exhausted by projection refresh"),
                     "projection snapshot capacity exhausted");
                return false;
            }
            if (nextRequest == std::numeric_limits<std::uint64_t>::max()) {
                fail(control,
                     clientError(ClientErrorCode::RequestIdExhausted, "frontend request IDs exhausted"),
                     "projection snapshot request ID exhausted");
                return false;
            }
            const std::string requestId = "c" + std::to_string(control.generation) + "-r" + std::to_string(nextRequest++);
            frontend::generated::DefinedCommand command{
                requestId,
                frontend::generated::CompleteCommandParameters{
                    frontend::generated::MethodParameters<frontend::generated::MethodId::SnapshotGet>{frontend::Json::object()}}};
            auto serialized = frontend::Codec::serializeDefinedCommand(command);
            accountErasedTransientBytes(securelyErase(command));
            if (!serialized) {
                fail(control,
                     clientError(ClientErrorCode::SerializationFailed, "projection snapshot serialization failed"),
                     "projection snapshot serialization failed");
                return false;
            }
            std::string&& serializedStorage = std::move(serialized).value();
            std::string serializedCommand(std::move(serializedStorage));
            eraseTransientString(serializedStorage, true);
            OutboundMessage message{OutboundKind::Command, std::move(serializedCommand), 0, false};
            eraseTransientString(serializedCommand, true);
            message.serializedBytes = message.compactJson.size();
            projectionSnapshotRequestId = requestId;
            SendResult result;
            try {
                result = control.transport.send(std::move(message));
            } catch (...) {
                eraseOwnedOutbound(message, true);
                projectionSnapshotRequestId.reset();
                fail(control, transportError("projection snapshot send callback failed", true), "projection snapshot send failed");
                return false;
            }
            eraseOwnedOutbound(message, true);
            if (result.status != SendStatus::Accepted) {
                projectionSnapshotRequestId.reset();
                fail(control,
                     transportError("transport rejected projection snapshot request", result.error && result.error->retryable),
                     "projection snapshot request rejected");
                return false;
            }
            return true;
        }

        void handleResponse(Connection::Control& control, const frontend::Response& response, bool& semanticallyAccepted) {
            if (projectionSnapshotRequestId && response.requestId == *projectionSnapshotRequestId) {
                if (!response.ok || !response.result ||
                    !frontend::Codec::decodeDefinedResult(frontend::generated::MethodId::SnapshotGet, *response.result)) {
                    fail(control,
                         protocolError(ClientErrorCode::ResponseTypeMismatch, "projection snapshot command failed"),
                         "projection snapshot command failed");
                    return;
                }
                projectionSnapshotRequestId.reset();
                projectionRefreshRequired = false;
                projectionSnapshotStreaming = true;
                projectionValidationState.reset();
                sawSnapshot = false;
                sawEvents = false;
                semanticallyAccepted = true;
                return;
            }
            const auto found = pending.find(response.requestId);
            if (found == pending.end()) {
                fail(control,
                     protocolError(ClientErrorCode::UnexpectedMessage, "unsolicited or duplicate frontend response"),
                     "frontend response correlation failed");
                return;
            }
            PendingOperation operation = std::move(found->second);
            pending.erase(found);
            if (!response.ok) {
                Error error{ErrorOrigin::Command,
                            std::nullopt,
                            response.error ? std::optional(response.error->code) : std::nullopt,
                            response.error ? response.error->message : "frontend command failed",
                            std::nullopt,
                            response.error ? response.error->details : std::nullopt,
                            false};
                semanticallyAccepted = true;
                failOperation(std::move(operation), error);
                return;
            }
            if (!response.result) {
                const Error error = protocolError(ClientErrorCode::ResponseTypeMismatch, "successful frontend response has no result");
                failOperation(std::move(operation), error);
                fail(control, error, "frontend response result missing");
                return;
            }
            auto decoded = frontend::Codec::decodeDefinedResult(operation.method, *response.result);
            if (!decoded) {
                const Error error = protocolError(ClientErrorCode::ResponseTypeMismatch, "frontend response result does not match request");
                failOperation(std::move(operation), error);
                fail(control, error, "frontend response type mismatch");
                return;
            }
            if (succeedOperation(control, std::move(operation), std::move(decoded).value())) {
                semanticallyAccepted = true;
            }
        }

        void handleSyncMessage(Connection::Control& control, const frontend::ServerMessage& message, bool& semanticallyAccepted) {
            if (connectionState != ConnectionState::Synchronizing || !currentSession) {
                fail(control,
                     protocolError(ClientErrorCode::UnexpectedMessage, "synchronization message outside synchronization"),
                     "frontend synchronization protocol violation");
                return;
            }
            if (projectionSnapshotRequestId && !projectionSnapshotStreaming) {
                fail(control,
                     protocolError(ClientErrorCode::UnexpectedMessage,
                                   "projection-refresh synchronization data preceded its successful snapshot response"),
                     "frontend projection-refresh ordering violation");
                return;
            }
            if (explicitSynchronization && !explicitSynchronization->responseAccepted && !projectionSnapshotStreaming) {
                fail(control,
                     protocolError(ClientErrorCode::UnexpectedMessage,
                                   "explicit synchronization data preceded its successful command response"),
                     "frontend explicit synchronization ordering violation");
                return;
            }
            const frontend::SyncMode streamMode =
                projectionSnapshotStreaming ? frontend::SyncMode::Snapshot
                                            : (explicitSynchronization ? explicitSynchronization->streamMode : currentSession->syncMode);
            if (const auto* snapshot = std::get_if<frontend::Snapshot>(&message)) {
                if (streamMode != frontend::SyncMode::Snapshot || sawSnapshot || sawEvents) {
                    fail(control,
                         protocolError(ClientErrorCode::UnexpectedMessage, "invalid mixed or duplicate snapshot synchronization"),
                         "frontend synchronization mode violation");
                    return;
                }
                std::string reductionError;
                auto reduction = detail::StateReducer::snapshot(currentState,
                                                                *snapshot,
                                                                *currentSession,
                                                                options.maximumDecodedStateBytes,
                                                                options.maximumRetainedDiagnostics,
                                                                options.allowLegacyV1,
                                                                reductionError,
                                                                activeProjectionMetadata());
                if (!reduction) {
                    fail(control,
                         protocolError(ClientErrorCode::StateDivergence, std::move(reductionError)),
                         "frontend snapshot reduction failed");
                    return;
                }
                sawSnapshot = true;
                const UpdateCause cause = projectionSnapshotStreaming
                                              ? UpdateCause::ProjectionRefresh
                                              : (explicitSynchronization
                                                     ? UpdateCause::ExplicitSnapshot
                                                     : (initialSnapshotFallback ? UpdateCause::SnapshotFallback
                                                                                : UpdateCause::InitialSnapshot));
                stateUpdated(std::move(*reduction), cause, snapshot->sequence, snapshot->sequence);
                semanticallyAccepted = true;
                return;
            }
            if (const auto* batch = std::get_if<frontend::EventBatch>(&message)) {
                if (streamMode != frontend::SyncMode::Replay || sawSnapshot) {
                    fail(control,
                         protocolError(ClientErrorCode::UnexpectedMessage, "event batch mixed into snapshot synchronization"),
                         "frontend synchronization mode violation");
                    return;
                }
                std::string reductionError;
                std::optional<detail::StateReduction> reduction;
                if (projectionRefreshRequired) {
                    if (!projectionValidationState) {
                        fail(control,
                             protocolError(ClientErrorCode::StateDivergence,
                                           "projection replay validation state is unavailable"),
                             "frontend projection replay validation failed");
                        return;
                    }
                    reduction = detail::StateReducer::validateSynchronizationEvents(*projectionValidationState,
                                                                                   *batch,
                                                                                   options.maximumDecodedStateBytes,
                                                                                   options.allowLegacyV1,
                                                                                   reductionError);
                } else {
                    reduction = detail::StateReducer::events(currentState,
                                                             *batch,
                                                             true,
                                                             options.maximumDecodedStateBytes,
                                                             options.maximumRetainedDiagnostics,
                                                             options.allowLegacyV1,
                                                             reductionError);
                }
                if (!reduction) {
                    fail(control,
                         protocolError(ClientErrorCode::StateDivergence, std::move(reductionError)),
                         "frontend replay reduction failed");
                    return;
                }
                if (!tryAccumulateSynchronizationCounts(reduction->receivedEvents,
                                                        reduction->appliedEvents,
                                                        reduction->ignoredAlreadyAppliedEvents)) {
                    fail(control,
                         protocolError(ClientErrorCode::StateCapacityExceeded,
                                       "frontend synchronization event counters exhausted"),
                         "frontend synchronization event counters exhausted");
                    return;
                }
                sawEvents = true;
                if (projectionRefreshRequired) {
                    projectionValidationState = std::move(reduction->state);
                } else {
                    const UpdateCause cause = explicitSynchronization
                                                  ? UpdateCause::ExplicitReplay
                                                  : (active && active->generation > 1 && helloResumeAfterSent
                                                         ? UpdateCause::ReconnectReplay
                                                         : UpdateCause::InitialReplay);
                    stateUpdated(std::move(*reduction),
                                 cause,
                                 batch->fromSequence,
                                 batch->toSequence);
                }
                semanticallyAccepted = true;
                return;
            }
            const auto* complete = std::get_if<frontend::SyncComplete>(&message);
            if (complete == nullptr) {
                fail(control,
                     protocolError(ClientErrorCode::UnexpectedMessage, "unexpected synchronization message"),
                     "frontend synchronization protocol violation");
                return;
            }
            if (streamMode == frontend::SyncMode::Snapshot && !sawSnapshot) {
                fail(control,
                     protocolError(ClientErrorCode::UnexpectedMessage, "snapshot synchronization completed without a snapshot"),
                     "frontend synchronization incomplete");
                return;
            }
            if (projectionRefreshRequired && !projectionSnapshotStreaming) {
                if (!projectionValidationState) {
                    fail(control,
                         protocolError(ClientErrorCode::StateDivergence,
                                       "projection replay validation state is unavailable at synchronization completion"),
                         "frontend projection replay validation failed");
                    return;
                }
                std::string stagingError;
                auto stagedBoundary = detail::StateReducer::synchronized(*projectionValidationState,
                                                                         complete->sequence,
                                                                         *currentSession,
                                                                         options.maximumDecodedStateBytes,
                                                                         stagingError,
                                                                         activeProjectionMetadata());
                if (!stagedBoundary) {
                    fail(control,
                         protocolError(ClientErrorCode::StateDivergence, std::move(stagingError)),
                         "frontend projection replay cursor validation failed");
                    return;
                }
                projectionValidationState = std::move(stagedBoundary->state);
                semanticallyAccepted = true;
                (void) requestProjectionSnapshot(control);
                return;
            }
            std::string reductionError;
            auto reduction = detail::StateReducer::synchronized(currentState,
                                                                complete->sequence,
                                                                *currentSession,
                                                                options.maximumDecodedStateBytes,
                                                                reductionError,
                                                                activeProjectionMetadata());
            if (!reduction) {
                fail(control,
                     protocolError(ClientErrorCode::StateDivergence, std::move(reductionError)),
                     "frontend synchronization cursor failed");
                return;
            }
            currentState = std::move(reduction->state);
            currentVisibleSequence = currentState.visibleSequence();
            currentSynchronizedThrough = currentState.synchronizedThrough();
            std::vector<Change> synchronizationChanges = std::move(reduction->changes);

            std::optional<RequestId> completedExplicitRequestId;
            if (explicitSynchronization) {
                explicitSynchronization->streamCompleted = true;
                completedExplicitRequestId = explicitSynchronization->requestId;
            }
            const bool synchronizationWasSnapshotFallback =
                projectionSnapshotStreaming || initialSnapshotFallback ||
                (explicitSynchronization && explicitSynchronization->snapshotFallback);
            std::optional<SynchronizationResult> completedSynchronizationResult;
            if (explicitSynchronization) {
                std::string synchronizationDecodeError;
                detail::SynchronizationDecodeInput input{streamMode,
                                                         complete->sequence,
                                                         currentState,
                                                         synchronizationReceivedEvents,
                                                         synchronizationAppliedEvents,
                                                         synchronizationIgnoredEvents,
                                                         synchronizationWasSnapshotFallback};
                completedSynchronizationResult =
                    explicitSynchronization->requestedMode == frontend::SyncMode::Snapshot
                        ? detail::decodeSnapshotSynchronizationResult(std::move(input), synchronizationDecodeError)
                        : detail::decodeReplaySynchronizationResult(std::move(input), synchronizationDecodeError);
                if (!completedSynchronizationResult) {
                    fail(control,
                         protocolError(ClientErrorCode::StateDivergence,
                                       synchronizationDecodeError.empty() ? "explicit synchronization result is inconsistent"
                                                                          : std::move(synchronizationDecodeError)),
                         "frontend explicit synchronization result decoding failed");
                    return;
                }
            }
            retainedContinuityKey = activeContinuityKey;
            retainedProjectionFingerprint = activeProjectionFingerprint;
            semanticallyAccepted = true;
            // SyncComplete is the terminal boundary for the internal
            // projection-refresh operation. Retire its pending-operation slot
            // before publishing Ready or invoking completion callbacks so a
            // callback may use the newly available capacity. The cached
            // synchronizationWasSnapshotFallback value above remains the
            // callback-visible result.
            projectionSnapshotStreaming = false;
            initialSnapshotFallback = false;
            helloResumeAfterSent.reset();
            transition(ConnectionState::Ready);
            if (!owns(control) || connectionState != ConnectionState::Ready) {
                return;
            }
            notifyStateUpdated(
                std::move(synchronizationChanges), UpdateCause::SynchronizationCompleted, std::nullopt, complete->sequence);
            if (!owns(control) || connectionState != ConnectionState::Ready) {
                return;
            }
            std::optional<ExplicitSynchronization> completedExplicitSynchronization;
            if (completedExplicitRequestId && explicitSynchronization && explicitSynchronization->streamCompleted &&
                explicitSynchronization->requestId == completedExplicitRequestId) {
                completedExplicitSynchronization = std::move(*explicitSynchronization);
                explicitSynchronization.reset();
            }
            if (completedExplicitSynchronization) {
                invokeExplicitSynchronizationSuccess(std::move(*completedExplicitSynchronization),
                                                     std::move(*completedSynchronizationResult));
            }
            if (!owns(control) || connectionState != ConnectionState::Ready) {
                return;
            }
            if (callbacks.onCursorAdvanced) {
                try {
                    callbacks.onCursorAdvanced(complete->sequence);
                } catch (...) {
                    diagnostic(Diagnostic::Severity::Warning,
                               "frontend client cursor callback threw",
                               clientError(ClientErrorCode::CallbackFailure, "cursor callback failed"));
                }
            }
            if (!owns(control) || connectionState != ConnectionState::Ready) {
                return;
            }
            if (callbacks.onSynchronized) {
                try {
                    callbacks.onSynchronized(SynchronizationInfo{
                        streamMode,
                        complete->sequence,
                        currentState,
                        active && active->generation > 1,
                        synchronizationWasSnapshotFallback});
                } catch (...) {
                    diagnostic(Diagnostic::Severity::Warning,
                               "frontend client synchronization callback threw",
                               clientError(ClientErrorCode::CallbackFailure, "synchronization callback failed"));
                }
            }
            if (!owns(control) || connectionState != ConnectionState::Ready) {
                return;
            }
        }

        void handleLiveEvents(Connection::Control& control, const frontend::EventBatch& batch, bool& semanticallyAccepted) {
            std::string reductionError;
            auto reduction = detail::StateReducer::events(currentState,
                                                          batch,
                                                          false,
                                                          options.maximumDecodedStateBytes,
                                                          options.maximumRetainedDiagnostics,
                                                          options.allowLegacyV1,
                                                          reductionError);
            if (!reduction) {
                fail(control,
                     protocolError(ClientErrorCode::StateDivergence, std::move(reductionError)),
                     "frontend live-event reduction failed");
                return;
            }
            const std::optional<frontend::SequenceNumber> cursor = reduction->state.synchronizedThrough();
            stateUpdated(std::move(*reduction), UpdateCause::Live, batch.fromSequence, batch.toSequence);
            semanticallyAccepted = true;
            if (owns(control) && cursor && callbacks.onCursorAdvanced) {
                try {
                    callbacks.onCursorAdvanced(*cursor);
                } catch (...) {
                    diagnostic(Diagnostic::Severity::Warning,
                               "frontend client cursor callback threw",
                               clientError(ClientErrorCode::CallbackFailure, "cursor callback failed"));
                }
            }
        }

        void handleProtocolError(Connection::Control& control,
                                 const frontend::ProtocolErrorMessage& message,
                                 bool& semanticallyAccepted,
                                 bool& observeAfterProtocolErrorClosure) {
            const Error error{ErrorOrigin::Protocol, std::nullopt, message.code, message.message, std::nullopt, message.details, false};
            if (message.closeConnection) {
                semanticallyAccepted = true;
                observeAfterProtocolErrorClosure = true;
                fail(control, error, "frontend server requested closure after a protocol error");
                return;
            }
            if (!message.requestId) {
                semanticallyAccepted = true;
                diagnostic(Diagnostic::Severity::Warning, "frontend server reported a non-closing protocol error", error);
                return;
            }
            if (projectionSnapshotRequestId && *message.requestId == *projectionSnapshotRequestId) {
                semanticallyAccepted = true;
                observeAfterProtocolErrorClosure = true;
                fail(control, error, "frontend projection refresh was rejected");
                return;
            }
            const auto found = pending.find(*message.requestId);
            if (found == pending.end()) {
                fail(control,
                     protocolError(ClientErrorCode::UnexpectedMessage, "uncorrelated frontend protocol error response", message.code),
                     "frontend protocol-error correlation failed");
                return;
            }
            PendingOperation operation = std::move(found->second);
            pending.erase(found);
            semanticallyAccepted = true;
            failOperation(std::move(operation), error);
        }

        void handle(Connection::Control& control, const frontend::ServerMessage& message) {
            bool semanticallyAccepted = false;
            bool observeAfterProtocolErrorClosure = false;
            if (const auto* welcome = std::get_if<frontend::Welcome>(&message)) {
                handleWelcome(control, *welcome, semanticallyAccepted);
            } else if (const auto* response = std::get_if<frontend::Response>(&message)) {
                if (connectionState != ConnectionState::Ready && !pending.contains(response->requestId) &&
                    (!projectionSnapshotRequestId || *projectionSnapshotRequestId != response->requestId)) {
                    fail(control,
                         protocolError(ClientErrorCode::UnexpectedMessage, "response received before frontend client was ready"),
                         "frontend response ordering violation");
                } else {
                    handleResponse(control, *response, semanticallyAccepted);
                }
            } else if (const auto* batch = std::get_if<frontend::EventBatch>(&message);
                       batch && connectionState == ConnectionState::Ready) {
                handleLiveEvents(control, *batch, semanticallyAccepted);
            } else if (const auto* error = std::get_if<frontend::ProtocolErrorMessage>(&message)) {
                handleProtocolError(control, *error, semanticallyAccepted, observeAfterProtocolErrorClosure);
            } else {
                handleSyncMessage(control, message, semanticallyAccepted);
            }
            const bool stillAttached = owns(control) && connectionState != ConnectionState::Closed;
            if (semanticallyAccepted && (stillAttached || observeAfterProtocolErrorClosure)) {
                invokeProtocolObservation(message);
            }
        }

        ReceiveResult dispatchMessage(Connection::Control& control, const frontend::ServerMessage& message) noexcept {
            if (!owns(control) || !control.connected) {
                return {false, TransportError{"frontend connection is closed", false}};
            }
            if (dispatchDepth == std::numeric_limits<std::size_t>::max()) {
                fail(control,
                     protocolError(ClientErrorCode::StateCapacityExceeded, "frontend dispatch depth exhausted"),
                     "frontend dispatch depth exhausted");
                return {false, TransportError{"frontend protocol rejected message", false}};
            }
            ++dispatchDepth;
            auto depthGuard = ScopeExit([this]() noexcept {
                --dispatchDepth;
            });
            try {
                handle(control, message);
                if (failAfterNextDispatchForTesting) {
                    failAfterNextDispatchForTesting = false;
                    throw std::runtime_error("injected frontend dispatch failure");
                }
                depthGuard.run();
                if (dispatchDepth == 0) {
                    flushDeferredCommands();
                }
            } catch (...) {
                depthGuard.run();
                if (owns(control)) {
                    fail(control,
                         protocolError(ClientErrorCode::UnexpectedMessage, "frontend internal message dispatch failed"),
                         "frontend internal message dispatch failed");
                }
            }
            const bool accepted = owns(control);
            return {accepted, accepted ? std::nullopt : std::optional(TransportError{"frontend protocol rejected message", false})};
        }

        ReceiveResult rejectOversizedInbound(Connection::Control& control) noexcept {
            fail(control,
                 protocolError(
                     ClientErrorCode::DecodeFailure, "frontend inbound message exceeds capacity", frontend::ErrorCode::FrameTooLarge),
                 "frontend inbound message too large");
            return {false, TransportError{"frontend inbound message exceeds capacity", false}};
        }

        void flushDeferredCommands() {
            if (dispatchDepth != 0 || flushingDeferredCommands) {
                return;
            }
            flushingDeferredCommands = true;
            auto flushingGuard = ScopeExit([this]() noexcept {
                flushingDeferredCommands = false;
            });
            while (!deferredCommands.empty()) {
                DeferredCommand& queued = deferredCommands.front();
                DeferredCommand deferred = std::move(queued);
                eraseOwnedOutbound(queued.message, true);
                deferredCommands.pop_front();
                auto deferredGuard = ScopeExit([this, &deferred]() noexcept {
                    eraseOwnedOutbound(deferred.message);
                });
                auto operationFound = pending.find(deferred.requestId);
                if (operationFound == pending.end()) {
                    eraseOwnedOutbound(deferred.message);
                    continue;
                }
                const bool synchronizationCommand =
                    operationFound->second.method == frontend::generated::MethodId::SnapshotGet ||
                    operationFound->second.method == frontend::generated::MethodId::EventsReplay;
                const bool activatesSynchronization =
                    synchronizationCommand && explicitSynchronization && explicitSynchronization->requestId &&
                    explicitSynchronization->requestId->value() == deferred.requestId && !explicitSynchronization->responseAccepted &&
                    connectionState == ConnectionState::Ready;
                if (activatesSynchronization) {
                    transition(ConnectionState::Synchronizing);
                    operationFound = pending.find(deferred.requestId);
                    if (operationFound == pending.end()) {
                        eraseOwnedOutbound(deferred.message);
                        continue;
                    }
                }
                const bool sendableState = connectionState == ConnectionState::Ready ||
                                           (connectionState == ConnectionState::Synchronizing && synchronizationCommand &&
                                            explicitSynchronization.has_value());
                if (!active || !active->connected || !sendableState) {
                    PendingOperation operation = std::move(operationFound->second);
                    pending.erase(operationFound);
                    eraseOwnedOutbound(deferred.message);
                    failOperation(
                        std::move(operation),
                        clientError(ClientErrorCode::NotConnected, "frontend connection closed before deferred command send", true));
                    continue;
                }
                const std::shared_ptr<Connection::Control> control = active;
                SendResult sendResult;
                bool sendThrew = false;
                try {
                    sendResult = control->transport.send(std::move(deferred.message));
                } catch (...) {
                    sendThrew = true;
                }
                eraseOwnedOutbound(deferred.message, true);
                if (sendThrew || sendResult.status != SendStatus::Accepted) {
                    const bool retryable = !sendThrew && sendResult.error && sendResult.error->retryable;
                    const auto failedFound = pending.find(deferred.requestId);
                    if (failedFound != pending.end()) {
                        PendingOperation operation = std::move(failedFound->second);
                        pending.erase(failedFound);
                        failOperation(std::move(operation),
                                      clientError(ClientErrorCode::SendRejected,
                                                  sendThrew ? "frontend transport send failed" : "frontend transport rejected command",
                                                  sendThrew || retryable));
                    }
                    if (owns(*control)) {
                        fail(*control,
                             transportError(sendThrew ? "transport send callback failed" : "transport rejected frontend command",
                                            sendThrew || retryable),
                             "frontend deferred command send failed");
                    }
                    break;
                }
            }
            flushingGuard.run();
        }

        Submission submitBound(frontend::generated::CompleteCommandParameters parameters, detail::BoundOperationCompletion completion) {
            if (connectionState == ConnectionState::Closed) {
                return {std::nullopt, clientError(ClientErrorCode::Closed, "frontend client is closed")};
            }
            if (connectionState != ConnectionState::Ready || !active || !active->connected) {
                return {std::nullopt, clientError(ClientErrorCode::NotReady, "frontend client is not ready")};
            }
            const frontend::generated::MethodId method = frontend::generated::commandMethod(parameters);
            if (explicitSynchronization && !explicitSynchronization->streamCompleted) {
                const frontend::generated::MethodId synchronizationMethod =
                    explicitSynchronization->requestedMode == frontend::SyncMode::Snapshot
                        ? frontend::generated::MethodId::SnapshotGet
                        : frontend::generated::MethodId::EventsReplay;
                if (method != synchronizationMethod) {
                    return {std::nullopt,
                            clientError(ClientErrorCode::NotReady,
                                        "frontend client is synchronizing and does not accept application operations")};
                }
            }
            const MethodStatus status = owner->methodStatus(method);
            if (status.available == Availability::No) {
                return {std::nullopt, clientError(ClientErrorCode::MethodUnavailable, "frontend method is unavailable")};
            }
            if (status.permitted == Availability::No) {
                return {std::nullopt, clientError(ClientErrorCode::MethodNotPermitted, "frontend method is not permitted")};
            }
            if (!completion.decode) {
                return {std::nullopt, clientError(ClientErrorCode::InvalidConfiguration, "frontend result decoder is missing")};
            }
            if (outstandingOperationCount() >= options.maximumPendingOperations) {
                return {std::nullopt,
                        clientError(ClientErrorCode::TooManyPendingOperations, "frontend pending-operation capacity exhausted")};
            }
            if (nextRequest == std::numeric_limits<std::uint64_t>::max()) {
                return {std::nullopt, clientError(ClientErrorCode::RequestIdExhausted, "frontend request IDs exhausted")};
            }
            RequestId requestId{"c" + std::to_string(active->generation) + "-r" + std::to_string(nextRequest++)};
            frontend::generated::DefinedCommand command{requestId.value(), std::move(parameters)};
            auto serialized = frontend::Codec::serializeDefinedCommand(command);
            if (!serialized) {
                accountErasedTransientBytes(securelyErase(command));
                return {std::nullopt, clientError(ClientErrorCode::SerializationFailed, "frontend command serialization failed")};
            }
            std::string&& serializedStorage = std::move(serialized).value();
            std::string serializedCommand(std::move(serializedStorage));
            eraseTransientString(serializedStorage, true);
            const frontend::generated::MethodMetadata* metadata = methodMetadata(method);
            if (metadata != nullptr && metadata->category == frontend::generated::MethodCategory::ReverseResponse) {
                const frontend::Json& validatedParameters =
                    std::visit([](const auto& value) -> const frontend::Json& { return value.value; }, command.parameters);
                const auto pendingRequestId = validatedParameters.find("pendingRequestId");
                if (pendingRequestId != validatedParameters.end() && pendingRequestId->is_string()) {
                    const PendingRequestState* pendingRequest =
                        currentState.pendingRequest(PendingRequestId{pendingRequestId->get<std::string>()});
                    if (pendingRequest != nullptr && pendingRequest->connectionInvalidated) {
                        accountErasedTransientBytes(securelyErase(command));
                        accountErasedTransientBytes(securelyErase(serializedCommand));
                        return {std::nullopt,
                                clientError(ClientErrorCode::MethodNotPermitted,
                                            "frontend pending request belongs to an inactive connection session")};
                    }
                }
            }
            accountErasedTransientBytes(securelyErase(command));
            const bool sensitive = generated::bindingIsSensitive(method);
            OutboundMessage message{OutboundKind::Command, std::move(serializedCommand), 0, sensitive};
            eraseTransientString(serializedCommand, true);
            message.serializedBytes = message.compactJson.size();
            const std::string requestKey = requestId.value();
            pending.emplace(requestKey, PendingOperation{requestId, method, std::move(completion)});
            const bool explicitSynchronizationCommand =
                explicitSynchronization &&
                (method == frontend::generated::MethodId::SnapshotGet || method == frontend::generated::MethodId::EventsReplay) &&
                !explicitSynchronization->requestId;
            if (explicitSynchronizationCommand) {
                explicitSynchronization->requestId = requestId;
            }
            if (dispatchDepth != 0 || flushingDeferredCommands) {
                deferredCommands.emplace_back(requestKey, std::move(message));
                eraseOwnedOutbound(message, true);
                return {requestId, std::nullopt};
            }
            const std::shared_ptr<Connection::Control> control = active;
            SendResult sendResult;
            try {
                sendResult = control->transport.send(std::move(message));
            } catch (...) {
                eraseOwnedOutbound(message, true);
                pending.erase(requestKey);
                if (explicitSynchronizationCommand && explicitSynchronization &&
                    explicitSynchronization->requestId == std::optional(requestId) && !explicitSynchronization->responseAccepted) {
                    explicitSynchronization.reset();
                }
                if (owns(*control)) {
                    fail(*control, transportError("transport send callback failed", true), "frontend command send failed");
                }
                return {std::nullopt, clientError(ClientErrorCode::SendRejected, "frontend transport send failed", true)};
            }
            eraseOwnedOutbound(message, true);
            if (sendResult.status != SendStatus::Accepted) {
                const bool retryable = sendResult.error && sendResult.error->retryable;
                pending.erase(requestKey);
                if (explicitSynchronizationCommand && explicitSynchronization &&
                    explicitSynchronization->requestId == std::optional(requestId) && !explicitSynchronization->responseAccepted) {
                    explicitSynchronization.reset();
                }
                if (owns(*control)) {
                    fail(*control, transportError("transport rejected frontend command", retryable), "frontend command rejected");
                }
                return {std::nullopt, clientError(ClientErrorCode::SendRejected, "frontend transport rejected command", retryable)};
            }
            return {requestId, std::nullopt};
        }

        Submission submit(frontend::generated::CompleteCommandParameters parameters, GeneratedCompletionHandler handler) {
            detail::BoundOperationCompletion completion;
            completion.decode = [](const frontend::generated::CompleteCommandResult& result, std::any& decoded, std::string&) {
                decoded = result;
                return true;
            };
            completion.succeed = [handler](const RequestId& requestId, std::any&& decoded) {
                if (handler) {
                    handler(GeneratedOperationResult{
                        requestId, std::any_cast<frontend::generated::CompleteCommandResult>(std::move(decoded)), std::nullopt});
                }
            };
            completion.fail = [handler](const RequestId& requestId, const Error& error) {
                if (handler) {
                    handler(GeneratedOperationResult{requestId, std::nullopt, error});
                }
            };
            return submitBound(std::move(parameters), std::move(completion));
        }

        void acceptExplicitSynchronizationResponse(const GeneratedOperationResult& response) {
            if (!explicitSynchronization) {
                return;
            }
            explicitSynchronization->requestId = response.requestId;
            if (!response) {
                failExplicitSynchronization(
                    response.error.value_or(clientError(ClientErrorCode::StateDivergence, "explicit synchronization command failed")),
                    true);
                return;
            }
            explicitSynchronization->responseAccepted = true;
            explicitSynchronization->generatedResult = *response.value;
            const frontend::Json commandResult = std::visit(
                [](const auto& value) {
                    return value.value;
                },
                *response.value);
            if (explicitSynchronization->requestedMode == frontend::SyncMode::Replay &&
                commandResult.value("syncMode", "replay") == "snapshot") {
                explicitSynchronization->streamMode = frontend::SyncMode::Snapshot;
                explicitSynchronization->snapshotFallback = true;
            }
            sawSnapshot = false;
            sawEvents = false;
            synchronizationReceivedEvents = 0;
            synchronizationAppliedEvents = 0;
            synchronizationIgnoredEvents = 0;
            transition(ConnectionState::Synchronizing);
        }

        Submission beginSynchronization(frontend::SyncMode mode,
                                        std::optional<frontend::SequenceNumber> after,
                                        CompletionHandler<SynchronizationResult> handler) {
            if (explicitSynchronization) {
                return {
                    std::nullopt,
                    clientError(ClientErrorCode::SynchronizationAlreadyActive, "one explicit frontend synchronization is already active")};
            }
            if (mode == frontend::SyncMode::Replay && !after) {
                return {std::nullopt, clientError(ClientErrorCode::InvalidConfiguration, "explicit replay requires a global cursor")};
            }
            explicitSynchronization.emplace();
            explicitSynchronization->requestedMode = mode;
            explicitSynchronization->streamMode = mode;
            explicitSynchronization->handler = std::move(handler);
            std::string encodingError;
            std::optional<frontend::Json> encodedParameters =
                mode == frontend::SyncMode::Snapshot
                    ? detail::encodeUnitParams(typed::Unit{}, encodingError)
                    : detail::encodeEventsReplayParams(*after, encodingError);
            if (!encodedParameters) {
                explicitSynchronization.reset();
                return {std::nullopt,
                        clientError(ClientErrorCode::SerializationFailed,
                                    encodingError.empty() ? "explicit synchronization parameters could not be encoded"
                                                          : std::move(encodingError))};
            }
            frontend::generated::CompleteCommandParameters parameters =
                mode == frontend::SyncMode::Snapshot
                    ? frontend::generated::CompleteCommandParameters{frontend::generated::MethodParameters<
                          frontend::generated::MethodId::SnapshotGet>{std::move(*encodedParameters)}}
                    : frontend::generated::CompleteCommandParameters{
                          frontend::generated::MethodParameters<frontend::generated::MethodId::EventsReplay>{
                              std::move(*encodedParameters)}};
            Submission submission = submit(std::move(parameters), [this](const GeneratedOperationResult& response) {
                acceptExplicitSynchronizationResponse(response);
            });
            if (!submission) {
                explicitSynchronization.reset();
            } else if (explicitSynchronization) {
                explicitSynchronization->requestId = submission.requestId;
                if (dispatchDepth == 0 && !flushingDeferredCommands) {
                    transition(ConnectionState::Synchronizing);
                }
            }
            return submission;
        }

        Submission beginGeneratedSynchronization(frontend::generated::CompleteCommandParameters parameters,
                                                  GeneratedCompletionHandler handler) {
            if (explicitSynchronization) {
                return {
                    std::nullopt,
                    clientError(ClientErrorCode::SynchronizationAlreadyActive, "one explicit frontend synchronization is already active")};
            }
            const frontend::generated::MethodId method = frontend::generated::commandMethod(parameters);
            if (method != frontend::generated::MethodId::SnapshotGet && method != frontend::generated::MethodId::EventsReplay) {
                return {std::nullopt,
                        clientError(ClientErrorCode::InvalidConfiguration, "generated synchronization method is invalid")};
            }
            const frontend::SyncMode mode = method == frontend::generated::MethodId::SnapshotGet ? frontend::SyncMode::Snapshot
                                                                                                  : frontend::SyncMode::Replay;
            explicitSynchronization.emplace();
            explicitSynchronization->requestedMode = mode;
            explicitSynchronization->streamMode = mode;
            explicitSynchronization->generatedHandler = std::move(handler);
            Submission submission = submit(std::move(parameters), [this](const GeneratedOperationResult& response) {
                acceptExplicitSynchronizationResponse(response);
            });
            if (!submission) {
                explicitSynchronization.reset();
            } else if (explicitSynchronization) {
                explicitSynchronization->requestId = submission.requestId;
                if (dispatchDepth == 0 && !flushingDeferredCommands) {
                    transition(ConnectionState::Synchronizing);
                }
            }
            return submission;
        }
    };

    Client::Client(ClientOptions options, ClientCallbacks callbacks)
        : impl(std::make_unique<Impl>(*this, std::move(options), std::move(callbacks))) {
        if (!impl->options.credentialProvider) {
            throw std::invalid_argument("frontend client requires a credential provider");
        }
        std::set<frontend::FrontendCapability> requested;
        for (const frontend::FrontendCapability capability : impl->options.requestedCapabilities) {
            if (!validCapability(capability) || !isRepresentationCapability(capability) || !requested.insert(capability).second) {
                throw std::invalid_argument("requested capabilities must be unique representation capabilities");
            }
        }
        if (!impl->options.allowLegacyV1 &&
            (requested.size() != AllRepresentationCapabilities.size() ||
             !std::ranges::all_of(AllRepresentationCapabilities, [&requested](frontend::FrontendCapability capability) {
                 return requested.contains(capability);
             }))) {
            throw std::invalid_argument("disabling legacy v1 requires all expanded representation capabilities");
        }
        std::set<frontend::FrontendCapability> required;
        for (const frontend::FrontendCapability capability : impl->options.requiredCapabilities) {
            if (!validCapability(capability) || !required.insert(capability).second) {
                throw std::invalid_argument("required capabilities must be valid and unique");
            }
            if (isRepresentationCapability(capability) && !requested.contains(capability)) {
                throw std::invalid_argument("a required representation capability must also be requested");
            }
        }
        impl->controller = std::unique_ptr<Controller>(new Controller(*this));
        impl->provider = std::unique_ptr<Provider>(new Provider(*this));
        impl->synchronization = std::unique_ptr<Synchronization>(new Synchronization(*this));
        impl->accounts = std::unique_ptr<Accounts>(new Accounts(*this));
        impl->apps = std::unique_ptr<Apps>(new Apps(*this));
        impl->commands = std::unique_ptr<Commands>(new Commands(*this));
        impl->configuration = std::unique_ptr<Configuration>(new Configuration(*this));
        impl->externalAgents = std::unique_ptr<ExternalAgents>(new ExternalAgents(*this));
        impl->feedback = std::unique_ptr<Feedback>(new Feedback(*this));
        impl->filesystem = std::unique_ptr<Filesystem>(new Filesystem(*this));
        impl->hooks = std::unique_ptr<Hooks>(new Hooks(*this));
        impl->marketplace = std::unique_ptr<Marketplace>(new Marketplace(*this));
        impl->mcp = std::unique_ptr<Mcp>(new Mcp(*this));
        impl->models = std::unique_ptr<Models>(new Models(*this));
        impl->permissionProfiles = std::unique_ptr<PermissionProfiles>(new PermissionProfiles(*this));
        impl->plugins = std::unique_ptr<Plugins>(new Plugins(*this));
        impl->requests = std::unique_ptr<Requests>(new Requests(*this));
        impl->reviews = std::unique_ptr<Reviews>(new Reviews(*this));
        impl->skills = std::unique_ptr<Skills>(new Skills(*this));
        impl->threads = std::unique_ptr<Threads>(new Threads(*this));
        impl->turns = std::unique_ptr<Turns>(new Turns(*this));
        impl->windowsSandbox = std::unique_ptr<WindowsSandbox>(new WindowsSandbox(*this));
    }

    Client::~Client() {
        close("frontend client destroyed");
    }

    Connection Client::openConnection(TransportCallbacks callbacks) {
        if (impl->connectionState != ConnectionState::Disconnected || impl->active || impl->detachCallbacksInProgress || !callbacks.send ||
            !callbacks.close ||
            impl->nextConnectionGeneration == std::numeric_limits<std::uint64_t>::max()) {
            return {};
        }
        auto control = std::make_shared<Connection::Control>();
        control->owner = this;
        control->transport = std::move(callbacks);
        control->generation = impl->nextConnectionGeneration++;
        impl->active = control;
        impl->transition(ConnectionState::Connecting);
        return Connection{std::move(control)};
    }

    void Client::setCallbacks(ClientCallbacks callbacks) {
        impl->callbacks = std::move(callbacks);
    }

    void Client::close(std::string reason) noexcept {
        if (impl->connectionState == ConnectionState::Closed || impl->clientCloseInProgress) {
            return;
        }
        impl->clientCloseInProgress = true;
        const std::shared_ptr<Connection::Control> control = impl->active;
        impl->transition(ConnectionState::Closing);
        const Error closeError = clientError(ClientErrorCode::Closed, "frontend client closed");
        impl->completePending(closeError);
        impl->failExplicitSynchronization(closeError);
        if (control && impl->owns(*control)) {
            impl->requestTransportClose(*control, std::move(reason));
        }
        if (control && impl->owns(*control)) {
            impl->detach(*control, closeError, true);
        }
        impl->currentSession.reset();
        impl->currentCapabilities.reset();
        impl->projectionRefreshRequired = false;
        impl->projectionSnapshotStreaming = false;
        impl->projectionSnapshotRequestId.reset();
        impl->projectionValidationState.reset();
        impl->transition(ConnectionState::Closed);
    }

    bool Client::isOpen() const noexcept {
        return impl->connectionState != ConnectionState::Closed;
    }
    bool Client::hasActiveConnection() const noexcept {
        return static_cast<bool>(impl->active);
    }
    bool Client::isReady() const noexcept {
        return impl->connectionState == ConnectionState::Ready;
    }
    ConnectionState Client::connectionState() const noexcept {
        return impl->connectionState;
    }
    State Client::state() const noexcept {
        return impl->currentState;
    }
    std::optional<SessionInfo> Client::session() const {
        return impl->currentSession;
    }
    std::optional<frontend::SequenceNumber> Client::visibleSequence() const {
        return impl->currentVisibleSequence;
    }
    std::optional<frontend::SequenceNumber> Client::synchronizedThrough() const {
        return impl->currentSynchronizedThrough;
    }
    std::size_t Client::pendingOperationCount() const noexcept {
        return impl->outstandingOperationCount();
    }

    MethodStatus Client::methodStatus(frontend::generated::MethodId method) const {
        const auto metadata = std::ranges::find_if(frontend::generated::AllMethods, [method](const auto& candidate) {
            return candidate.id == method;
        });
        if (metadata == frontend::generated::AllMethods.end()) {
            return MethodStatus{method,
                                Availability::Unknown,
                                Availability::Unknown,
                                false,
                                false,
                                false,
                                {}};
        }
        MethodStatus status{method,
                            Availability::Unknown,
                            Availability::Unknown,
                            metadata->controllerRequired,
                            metadata->providerReadyRequired,
                            metadata->defaultEnabled,
                            {metadata->requiredScopes.begin(), metadata->requiredScopes.end()}};
        if (impl->currentSession) {
            if (impl->currentSession->availableMethods) {
                status.available = contains(*impl->currentSession->availableMethods, method) ? Availability::Yes : Availability::No;
            }
            if (impl->currentSession->permittedMethods) {
                status.permitted = contains(*impl->currentSession->permittedMethods, method) ? Availability::Yes : Availability::No;
            }
        }
        return status;
    }

    CapabilityStatus Client::capabilityStatus(frontend::FrontendCapability capability) const {
        CapabilityStatus status{capability};
        const auto* metadata = capabilityMetadata(capability);
        if (metadata == nullptr || !metadata->defined) {
            return status;
        }
        status.defined = Availability::Yes;
        if (!impl->currentCapabilities) {
            return status;
        }
        status.implemented = contains(impl->currentCapabilities->implemented, capability) ? Availability::Yes : Availability::No;
        status.permitted = contains(impl->currentCapabilities->permitted, capability) ? Availability::Yes : Availability::No;
        return status;
    }

    Submission Client::submit(frontend::generated::CompleteCommandParameters parameters, GeneratedCompletionHandler handler) {
        const frontend::generated::MethodId method = frontend::generated::commandMethod(parameters);
        if (method == frontend::generated::MethodId::SnapshotGet || method == frontend::generated::MethodId::EventsReplay) {
            return impl->beginGeneratedSynchronization(std::move(parameters), std::move(handler));
        }
        return impl->submit(std::move(parameters), std::move(handler));
    }

    Submission Client::submitBound(frontend::generated::CompleteCommandParameters parameters, detail::BoundOperationCompletion completion) {
        return impl->submitBound(std::move(parameters), std::move(completion));
    }

    Submission Client::submitReverseBound(const PendingRequestId& pendingRequestId,
                                          frontend::generated::CompleteCommandParameters parameters,
                                          detail::BoundOperationCompletion completion) {
        const PendingRequestState* pendingRequest = impl->currentState.pendingRequest(pendingRequestId);
        if (pendingRequest != nullptr && pendingRequest->connectionInvalidated) {
            return {std::nullopt,
                    clientError(ClientErrorCode::MethodNotPermitted,
                                "frontend pending request belongs to an inactive connection session")};
        }
        return impl->submitBound(std::move(parameters), std::move(completion));
    }

    Submission Client::beginSynchronization(frontend::SyncMode mode,
                                            std::optional<frontend::SequenceNumber> after,
                                            CompletionHandler<SynchronizationResult> handler) {
        return impl->beginSynchronization(mode, after, std::move(handler));
    }

    Submission Synchronization::snapshot(CompletionHandler<SynchronizationResult> handler) {
        return client->beginSynchronization(frontend::SyncMode::Snapshot, std::nullopt, std::move(handler));
    }

    Submission Synchronization::replay(frontend::SequenceNumber after, CompletionHandler<SynchronizationResult> handler) {
        return client->beginSynchronization(frontend::SyncMode::Replay, after, std::move(handler));
    }

    namespace {
        template <frontend::generated::MethodId Method>
        std::optional<frontend::generated::CompleteCommandParameters> unitParameters(std::string& error) noexcept {
            std::optional<frontend::Json> encoded = detail::encodeUnitParams(typed::Unit{}, error);
            if (!encoded) {
                return std::nullopt;
            }
            return frontend::generated::CompleteCommandParameters{
                frontend::generated::MethodParameters<Method>{std::move(*encoded)}};
        }

        template <frontend::generated::MethodId Method>
        detail::BoundOperationCompletion unitCompletion(DoneHandler handler) {
            return detail::bindCompletion<typed::Unit>(
                std::move(handler),
                [](const frontend::generated::CompleteCommandResult& result, std::string& error) noexcept {
                    const auto* generated = std::get_if<frontend::generated::MethodResult<Method>>(&result);
                    if (generated == nullptr) {
                        error = "frontend unit result did not match the submitted generated MethodId";
                        return std::optional<typed::Unit>{};
                    }
                    return detail::decodeUnitResult(generated->value, error);
                });
        }

        template <frontend::generated::MethodId Method>
        detail::BoundOperationCompletion controllerCompletion(Client& client, CompletionHandler<ControllerResult> handler) {
            return detail::bindCompletion<ControllerResult>(
                std::move(handler),
                [&client](const frontend::generated::CompleteCommandResult& result, std::string& error) noexcept {
                    const auto* generated = std::get_if<frontend::generated::MethodResult<Method>>(&result);
                    if (generated == nullptr) {
                        error = "frontend controller result did not match the submitted generated MethodId";
                        return std::optional<ControllerResult>{};
                    }
                    const std::optional<SessionInfo> session = client.session();
                    const std::optional<std::string_view> sessionId =
                        session ? std::optional<std::string_view>(session->sessionId) : std::nullopt;
                    return detail::decodeControllerResult(generated->value, sessionId, error);
                });
        }
    } // namespace

    Submission Controller::acquire(CompletionHandler<ControllerResult> handler) {
        std::string error;
        auto parameters = unitParameters<frontend::generated::MethodId::ControllerAcquire>(error);
        if (!parameters) {
            return {std::nullopt,
                    clientError(ClientErrorCode::SerializationFailed,
                                error.empty() ? "controller.acquire parameters could not be encoded" : std::move(error))};
        }
        return client->submitBound(
            std::move(*parameters),
            controllerCompletion<frontend::generated::MethodId::ControllerAcquire>(*client, std::move(handler)));
    }

    Submission Controller::release(CompletionHandler<ControllerResult> handler) {
        std::string error;
        auto parameters = unitParameters<frontend::generated::MethodId::ControllerRelease>(error);
        if (!parameters) {
            return {std::nullopt,
                    clientError(ClientErrorCode::SerializationFailed,
                                error.empty() ? "controller.release parameters could not be encoded" : std::move(error))};
        }
        return client->submitBound(
            std::move(*parameters),
            controllerCompletion<frontend::generated::MethodId::ControllerRelease>(*client, std::move(handler)));
    }

    bool Controller::ownedByThisClient() const noexcept {
        const State state = client->state();
        return state.controller().value && state.controller().value->ownedByThisClient;
    }

    Submission Provider::start(DoneHandler handler) {
        std::string error;
        auto parameters = unitParameters<frontend::generated::MethodId::ProviderStart>(error);
        if (!parameters) {
            return {std::nullopt,
                    clientError(ClientErrorCode::SerializationFailed,
                                error.empty() ? "provider.start parameters could not be encoded" : std::move(error))};
        }
        return client->submitBound(std::move(*parameters),
                                   unitCompletion<frontend::generated::MethodId::ProviderStart>(std::move(handler)));
    }

    Submission Provider::stop(DoneHandler handler) {
        std::string error;
        auto parameters = unitParameters<frontend::generated::MethodId::ProviderStop>(error);
        if (!parameters) {
            return {std::nullopt,
                    clientError(ClientErrorCode::SerializationFailed,
                                error.empty() ? "provider.stop parameters could not be encoded" : std::move(error))};
        }
        return client->submitBound(std::move(*parameters),
                                   unitCompletion<frontend::generated::MethodId::ProviderStop>(std::move(handler)));
    }

    Submission Provider::restart(DoneHandler handler) {
        std::string error;
        auto parameters = unitParameters<frontend::generated::MethodId::ProviderRestart>(error);
        if (!parameters) {
            return {std::nullopt,
                    clientError(ClientErrorCode::SerializationFailed,
                                error.empty() ? "provider.restart parameters could not be encoded" : std::move(error))};
        }
        return client->submitBound(std::move(*parameters),
                                   unitCompletion<frontend::generated::MethodId::ProviderRestart>(std::move(handler)));
    }

#define AISUITE_CLIENT_FACADE_GETTER(Type, name)                                                                                           \
    Type& Client::name() noexcept {                                                                                                        \
        return *impl->name;                                                                                                                \
    }
    AISUITE_CLIENT_FACADE_GETTER(Controller, controller)
    AISUITE_CLIENT_FACADE_GETTER(Provider, provider)
    AISUITE_CLIENT_FACADE_GETTER(Synchronization, synchronization)
    AISUITE_CLIENT_FACADE_GETTER(Accounts, accounts)
    AISUITE_CLIENT_FACADE_GETTER(Apps, apps)
    AISUITE_CLIENT_FACADE_GETTER(Commands, commands)
    AISUITE_CLIENT_FACADE_GETTER(Configuration, configuration)
    AISUITE_CLIENT_FACADE_GETTER(ExternalAgents, externalAgents)
    AISUITE_CLIENT_FACADE_GETTER(Feedback, feedback)
    AISUITE_CLIENT_FACADE_GETTER(Filesystem, filesystem)
    AISUITE_CLIENT_FACADE_GETTER(Hooks, hooks)
    AISUITE_CLIENT_FACADE_GETTER(Marketplace, marketplace)
    AISUITE_CLIENT_FACADE_GETTER(Mcp, mcp)
    AISUITE_CLIENT_FACADE_GETTER(Models, models)
    AISUITE_CLIENT_FACADE_GETTER(PermissionProfiles, permissionProfiles)
    AISUITE_CLIENT_FACADE_GETTER(Plugins, plugins)
    AISUITE_CLIENT_FACADE_GETTER(Requests, requests)
    AISUITE_CLIENT_FACADE_GETTER(Reviews, reviews)
    AISUITE_CLIENT_FACADE_GETTER(Skills, skills)
    AISUITE_CLIENT_FACADE_GETTER(Threads, threads)
    AISUITE_CLIENT_FACADE_GETTER(Turns, turns)
    AISUITE_CLIENT_FACADE_GETTER(WindowsSandbox, windowsSandbox)
#undef AISUITE_CLIENT_FACADE_GETTER

    Connection::Connection() noexcept = default;
    Connection::Connection(std::shared_ptr<Control> connectionControl) noexcept
        : control(std::move(connectionControl)) {
    }
    Connection::Connection(Connection&&) noexcept = default;
    Connection& Connection::operator=(Connection&& other) noexcept {
        if (this == &other) {
            return *this;
        }
        close("frontend transport attachment replaced");
        control = std::move(other.control);
        return *this;
    }
    Connection::~Connection() {
        if (control && control->owner && control->open) {
            close("frontend transport attachment destroyed");
        }
    }

    void Connection::transportConnected() noexcept {
        if (!control || !control->owner || !control->open || control->connectedReported) {
            return;
        }
        control->connectedReported = true;
        control->connected = true;
        control->owner->impl->sendHello(*control);
    }

    ReceiveResult Connection::receive(std::string_view compactJson) noexcept {
        if (!control || !control->owner || !control->open || !control->connected) {
            return {false, TransportError{"frontend connection is closed", false}};
        }
        Client::Impl& implementation = *control->owner->impl;
        if (compactJson.size() > implementation.options.maximumInboundMessageBytes) {
            return implementation.rejectOversizedInbound(*control);
        }
        auto decoded = frontend::Codec::decodeServer(compactJson);
        if (!decoded) {
            implementation.fail(
                *control,
                protocolError(ClientErrorCode::DecodeFailure, "failed to decode frontend server message", decoded.error().code),
                "frontend server message decode failed");
            return {false, TransportError{"failed to decode frontend server message", false}};
        }
        return implementation.dispatchMessage(*control, decoded.value());
    }

    ReceiveResult Connection::receive(const frontend::Json& message) noexcept {
        if (!control || !control->owner || !control->open || !control->connected) {
            return {false, TransportError{"frontend connection is closed", false}};
        }
        Client::Impl& implementation = *control->owner->impl;
        const std::optional<bool> withinBound = jsonFitsBound(message, implementation.options.maximumInboundMessageBytes);
        if (!withinBound) {
            implementation.fail(*control,
                                protocolError(ClientErrorCode::DecodeFailure, "failed to size frontend server message"),
                                "frontend server message sizing failed");
            return {false, TransportError{"failed to size frontend server message", false}};
        }
        if (!*withinBound) {
            return implementation.rejectOversizedInbound(*control);
        }
        auto decoded = frontend::Codec::decodeServer(message);
        if (!decoded) {
            implementation.fail(
                *control,
                protocolError(ClientErrorCode::DecodeFailure, "failed to decode frontend server message", decoded.error().code),
                "frontend server message decode failed");
            return {false, TransportError{"failed to decode frontend server message", false}};
        }
        return implementation.dispatchMessage(*control, decoded.value());
    }

    ReceiveResult Connection::receive(const frontend::ServerMessage& message) noexcept {
        if (!control || !control->owner || !control->open || !control->connected) {
            return {false, TransportError{"frontend connection is closed", false}};
        }
        Client::Impl& implementation = *control->owner->impl;
        const auto encoded = frontend::Codec::encodeServer(message);
        if (!encoded) {
            implementation.fail(*control,
                                protocolError(ClientErrorCode::DecodeFailure, "failed to encode frontend server message"),
                                "frontend server message encoding failed");
            return {false, TransportError{"failed to encode frontend server message", false}};
        }
        const std::optional<bool> withinBound =
            jsonFitsBound(encoded.value(), implementation.options.maximumInboundMessageBytes);
        if (!withinBound) {
            implementation.fail(*control,
                                protocolError(ClientErrorCode::DecodeFailure, "failed to size frontend server message"),
                                "frontend server message sizing failed");
            return {false, TransportError{"failed to size frontend server message", false}};
        }
        if (!*withinBound) {
            return implementation.rejectOversizedInbound(*control);
        }
        auto decoded = frontend::Codec::decodeServer(encoded.value());
        if (!decoded) {
            implementation.fail(
                *control,
                protocolError(ClientErrorCode::DecodeFailure, "failed to validate frontend server message", decoded.error().code),
                "frontend server message validation failed");
            return {false, TransportError{"failed to validate frontend server message", false}};
        }
        return implementation.dispatchMessage(*control, decoded.value());
    }

    void Connection::transportDisconnected(std::optional<TransportError> error) noexcept {
        if (!control || !control->owner || !control->open) {
            return;
        }
        std::optional<Error> clientFailure;
        if (error) {
            clientFailure = transportError(error->message, error->retryable);
        }
        Client::Impl& implementation = *control->owner->impl;
        if (implementation.clientCloseInProgress) {
            clientFailure = clientError(ClientErrorCode::Closed, "frontend client closed");
        }
        implementation.detach(*control, std::move(clientFailure), implementation.clientCloseInProgress);
    }

    void Connection::close(std::string reason) noexcept {
        if (!control || !control->owner || !control->open) {
            return;
        }
        Client::Impl& implementation = *control->owner->impl;
        implementation.requestTransportClose(*control, std::move(reason));
        const Error closeError = implementation.clientCloseInProgress
                                     ? clientError(ClientErrorCode::Closed, "frontend client closed")
                                     : clientError(ClientErrorCode::NotConnected, "frontend connection closed");
        implementation.detach(*control,
                              closeError,
                              implementation.clientCloseInProgress);
    }

    bool Connection::isOpen() const noexcept {
        return control && control->open;
    }
    bool Connection::isTransportConnected() const noexcept {
        return control && control->connected;
    }
    std::uint64_t Connection::generation() const noexcept {
        return control ? control->generation : 0;
    }

    void detail::ClientTestAccess::setNextRequest(Client& client, std::uint64_t next) noexcept {
        client.impl->nextRequest = next;
    }

    void detail::ClientTestAccess::setNextConnectionGeneration(Client& client, std::uint64_t next) noexcept {
        client.impl->nextConnectionGeneration = next;
    }

    void detail::ClientTestAccess::setSynchronizationCounts(Client& client,
                                                            std::size_t received,
                                                            std::size_t applied,
                                                            std::size_t ignored) noexcept {
        client.impl->synchronizationReceivedEvents = received;
        client.impl->synchronizationAppliedEvents = applied;
        client.impl->synchronizationIgnoredEvents = ignored;
    }

    bool detail::ClientTestAccess::tryAccumulateSynchronizationCounts(Client& client,
                                                                     std::size_t received,
                                                                     std::size_t applied,
                                                                     std::size_t ignored) noexcept {
        return client.impl->tryAccumulateSynchronizationCounts(received, applied, ignored);
    }

    std::array<std::size_t, 3> detail::ClientTestAccess::synchronizationCounts(const Client& client) noexcept {
        return {
            client.impl->synchronizationReceivedEvents,
            client.impl->synchronizationAppliedEvents,
            client.impl->synchronizationIgnoredEvents,
        };
    }

    void detail::ClientTestAccess::failNextHelloConstruction(Client& client) noexcept {
        client.impl->failNextHelloConstructionForTesting = true;
    }

    void detail::ClientTestAccess::failAfterNextDispatch(Client& client) noexcept {
        client.impl->failAfterNextDispatchForTesting = true;
    }

    std::size_t detail::ClientTestAccess::erasedTransientBytes(const Client& client) noexcept {
        return client.impl->erasedTransientBytes;
    }

    std::size_t detail::ClientTestAccess::verifiedMovedFromStringScrubs(const Client& client) noexcept {
        return client.impl->verifiedMovedFromStringScrubs;
    }

    bool detail::ClientTestAccess::shortStringStorageScrubbed() noexcept {
        std::string source = "SSO_SECRET_12";
        const std::size_t sentinelBytes = source.size();
        std::string destination(std::move(source));
        bool sourceZeroed = false;
        bool destinationZeroed = false;
        const std::size_t sourceScrubbed = securelyErase(source, &sourceZeroed);
        const std::size_t destinationScrubbed = securelyErase(destination, &destinationZeroed);
        return sourceZeroed && destinationZeroed && sourceScrubbed != 0 && destinationScrubbed >= sentinelBytes;
    }

} // namespace ai::openai::codex::frontend::client
