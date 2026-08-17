/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/internal/server/ServerCore.h"

#include "ai/openai/codex/frontend/internal/model/Projection.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <span>
#include <type_traits>

namespace ai::openai::codex::frontend::internal::server {

    bool OccurrenceCoalescingKey::valid() const noexcept {
        constexpr std::size_t MaximumKeyComponentBytes = model::StrongIdentifier<model::SourceStampTag>::MaximumBytes;
        const auto validComponent = [](std::string_view component) noexcept {
            return component.size() <= MaximumKeyComponentBytes && component.find('\0') == std::string_view::npos;
        };
        return validComponent(entityId) && validComponent(channel);
    }

    namespace {

        [[nodiscard]] std::uint64_t defaultMonotonicClockMs() noexcept {
            const auto elapsed = std::chrono::steady_clock::now().time_since_epoch();
            return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count());
        }

        [[nodiscard]] ErrorCode protocolCode(AuthenticationFailureCode code) noexcept {
            switch (code) {
                case AuthenticationFailureCode::AuthenticationRequired:
                    return ErrorCode::AuthenticationRequired;
                case AuthenticationFailureCode::AuthenticationFailed:
                    return ErrorCode::AuthenticationFailed;
                case AuthenticationFailureCode::OriginRejected:
                    return ErrorCode::OriginRejected;
                case AuthenticationFailureCode::TransportSecurityRequired:
                    return ErrorCode::TransportSecurityRequired;
                case AuthenticationFailureCode::RateLimited:
                    return ErrorCode::RateLimited;
            }
            return ErrorCode::AuthenticationFailed;
        }

        [[nodiscard]] std::string authenticationErrorMessage(AuthenticationFailureCode code) {
            switch (code) {
                case AuthenticationFailureCode::AuthenticationRequired:
                    return "frontend authentication is required";
                case AuthenticationFailureCode::AuthenticationFailed:
                    return "frontend authentication failed";
                case AuthenticationFailureCode::OriginRejected:
                    return "frontend origin is not permitted";
                case AuthenticationFailureCode::TransportSecurityRequired:
                    return "frontend transport security is required";
                case AuthenticationFailureCode::RateLimited:
                    return "frontend authentication rate limit exceeded";
            }
            return "frontend authentication failed";
        }

        [[nodiscard]] const generated::MethodMetadata& methodMetadata(generated::MethodId method) noexcept {
            const std::size_t index = static_cast<std::size_t>(method);
            return index < generated::AllMethods.size() ? generated::AllMethods[index] : generated::AllMethods.front();
        }

        [[nodiscard]] const Json& commandParameters(const generated::DefinedCommand& command) noexcept {
            return std::visit(
                [](const auto& parameters) -> const Json& {
                    return parameters.value;
                },
                command.parameters);
        }

        [[nodiscard]] Json invocationPolicyParameters(const generated::DefinedCommand& command) {
            Json parameters = commandParameters(command);
            for (const auto& [key, value] : command.parameterExtensions.items()) {
                parameters[key] = value;
            }
            return parameters;
        }

        [[nodiscard]] bool hasCurrentProtocolEnvelope(const Json& message) noexcept {
            const auto protocol = message.find("protocol");
            const auto version = message.find("version");
            const auto messageKind = message.find("kind");
            if (protocol == message.end() || !protocol->is_string() || protocol->get_ref<const std::string&>() != ProtocolIdentity ||
                version == message.end() || messageKind == message.end() || !messageKind->is_string()) {
                return false;
            }
            if (version->is_number_unsigned()) {
                return version->get<std::uint64_t>() == ProtocolVersion;
            }
            if (version->is_number_integer()) {
                const std::int64_t signedVersion = version->get<std::int64_t>();
                return signedVersion >= 0 && static_cast<std::uint64_t>(signedVersion) == ProtocolVersion;
            }
            return false;
        }

        [[nodiscard]] bool containsScope(std::span<const FrontendScope> scopes, FrontendScope required) noexcept {
            return std::find(scopes.begin(), scopes.end(), required) != scopes.end();
        }

        [[nodiscard]] bool containsScope(const FrontendPrincipal& principal, FrontendScope required) noexcept {
            return containsScope(principal.scopes, required);
        }

        [[nodiscard]] bool validPrincipal(const FrontendPrincipal& principal) noexcept {
            if (principal.id.empty() || principal.profile.empty() || principal.scopes.size() > LocalTrustedScopes.size()) {
                return false;
            }
            std::vector<FrontendScope> seen;
            seen.reserve(principal.scopes.size());
            for (const FrontendScope scope : principal.scopes) {
                if (static_cast<std::size_t>(scope) >= LocalTrustedScopes.size() || containsScope(seen, scope)) {
                    return false;
                }
                seen.push_back(scope);
            }
            return true;
        }

        [[nodiscard]] bool containsCapability(std::span<const FrontendCapability> capabilities, FrontendCapability required) noexcept {
            return std::find(capabilities.begin(), capabilities.end(), required) != capabilities.end();
        }

        void addCapability(std::vector<FrontendCapability>& capabilities, FrontendCapability capability) {
            if (!containsCapability(capabilities, capability)) {
                capabilities.push_back(capability);
            }
        }

        [[nodiscard]] std::vector<FrontendCapability> definedCapabilities() {
            std::vector<FrontendCapability> result;
            result.reserve(generated::AllCapabilities.size());
            for (const generated::CapabilityMetadata& metadata : generated::AllCapabilities) {
                if (const auto capability = frontendCapabilityFromString(metadata.key)) {
                    result.push_back(*capability);
                }
            }
            return result;
        }

        [[nodiscard]] std::string_view addressWithoutEphemeralPort(std::string_view address) noexcept {
            if (address.starts_with('[')) {
                const std::size_t closingBracket = address.find(']');
                if (closingBracket == address.size() - 1) {
                    return address.substr(1, closingBracket - 1);
                }
                if (closingBracket != std::string_view::npos && closingBracket + 1 < address.size() && address[closingBracket + 1] == ':') {
                    const std::string_view port = address.substr(closingBracket + 2);
                    if (!port.empty() && std::all_of(port.begin(), port.end(), [](char character) {
                            return character >= '0' && character <= '9';
                        })) {
                        return address.substr(1, closingBracket - 1);
                    }
                }
                return address;
            }

            const std::size_t separator = address.rfind(':');
            if (separator != std::string_view::npos && address.find(':') == separator) {
                const std::string_view port = address.substr(separator + 1);
                if (!port.empty() && std::all_of(port.begin(), port.end(), [](char character) {
                        return character >= '0' && character <= '9';
                    })) {
                    return address.substr(0, separator);
                }
            }
            return address;
        }

        [[nodiscard]] bool hexadecimalDigit(char character) noexcept {
            return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f') ||
                   (character >= 'A' && character <= 'F');
        }

        [[nodiscard]] std::string_view rfcommAddressWithoutChannel(std::string_view address) noexcept {
            constexpr std::size_t BluetoothAddressSize = 17;
            if (address.size() < BluetoothAddressSize) {
                return address;
            }
            for (std::size_t index = 0; index < BluetoothAddressSize; ++index) {
                const bool separator = index % 3 == 2;
                if ((separator && address[index] != ':') || (!separator && !hexadecimalDigit(address[index]))) {
                    return address;
                }
            }
            if (address.size() == BluetoothAddressSize) {
                return address;
            }
            if (address[BluetoothAddressSize] != ':') {
                return address;
            }
            const std::string_view channel = address.substr(BluetoothAddressSize + 1);
            if (channel.empty() || !std::all_of(channel.begin(), channel.end(), [](char character) {
                    return character >= '0' && character <= '9';
                })) {
                return address;
            }
            return address.substr(0, BluetoothAddressSize);
        }

        [[nodiscard]] std::string peerAdmissionKey(const FrontendPeerContext& peer) {
            constexpr std::size_t MaximumPeerKeyBytes = model::SessionIdentity::MaximumBytes;
            if (peer.remoteAddress) {
                const bool rfcomm = peer.transport == FrontendTransportKind::Rfcomm || peer.transport == FrontendTransportKind::RfcommTls;
                const std::string_view canonical =
                    rfcomm ? rfcommAddressWithoutChannel(*peer.remoteAddress) : addressWithoutEphemeralPort(*peer.remoteAddress);
                return "address:" + std::string(canonical.substr(0, MaximumPeerKeyBytes));
            }
            if (peer.unixUserId) {
                return "unix:uid:" + std::to_string(*peer.unixUserId);
            }
            return std::string(toString(peer.transport)) + ":anonymous";
        }

        [[nodiscard]] std::uint64_t saturatingMultiply(std::size_t left, std::uint64_t right) noexcept {
            if (left == 0 || right == 0) {
                return 0;
            }
            if (left > std::numeric_limits<std::uint64_t>::max() / right) {
                return std::numeric_limits<std::uint64_t>::max();
            }
            return static_cast<std::uint64_t>(left) * right;
        }

        [[nodiscard]] std::uint64_t saturatingAdd(std::uint64_t left, std::uint64_t right) noexcept {
            return right > std::numeric_limits<std::uint64_t>::max() - left ? std::numeric_limits<std::uint64_t>::max() : left + right;
        }

        [[nodiscard]] CommandError
        commandError(ErrorCode code, std::string message, std::optional<model::SafeDetail> details = std::nullopt) {
            CommandError result;
            result.code = code;
            result.message = std::move(message);
            if (details) {
                result.details = details->json();
            }
            return result;
        }

        [[nodiscard]] CodecError codecFailure(ErrorCode code, std::string message, bool closeConnection = true) {
            return CodecError{code, std::move(message), closeConnection, {}, std::nullopt, std::nullopt};
        }

        [[nodiscard]] ReceiveResult containedReceiveFailure() noexcept {
            ReceiveResult result;
            result.status = ReceiveStatus::Rejected;
            try {
                result.error = codecFailure(ErrorCode::InternalError, "frontend server core exception boundary", false);
            } catch (...) {
                result.status = ReceiveStatus::Closed;
            }
            return result;
        }

        [[nodiscard]] bool validOccurrenceDraft(const model::OccurrenceDraft& occurrence) noexcept {
            return model::validateOccurrenceDraft(occurrence);
        }

    } // namespace

    class ServerCore::Impl : public std::enable_shared_from_this<ServerCore::Impl> {
    public:
        explicit Impl(BackendPort& backendPort, ServerCoreOptions configuredOptions)
            : backend(backendPort)
            , options(std::move(configuredOptions)) {
            if (!options.monotonicClockMs) {
                options.monotonicClockMs = defaultMonotonicClockMs;
            }
            journal = std::make_unique<model::TypedOccurrenceJournal>(
                model::JournalConfig{options.journalMaximumEntries, options.journalMaximumBytes, options.journalInitialSequence});
        }

        struct QueuedMessage {
            ServerMessage message;
            std::size_t bytes = 0;
        };

        struct Connection {
            FrontendPeerContext peer;
            ConnectionCallbacks callbacks;
            std::uint64_t generation = 0;
            bool helloAttempted = false;
            bool helloComplete = false;
            bool closing = false;
            std::optional<ConnectionClose> closeAfterDrain;
            std::optional<FrontendPrincipal> principal;
            std::optional<model::SessionIdentity> session;
            std::vector<FrontendCapability> negotiatedCapabilities;
            std::deque<QueuedMessage> outbound;
            // A live Snapshot published from inside BackendPort::snapshot()
            // is accepted immediately but must follow the outer snapshot
            // barrier (and its SyncComplete, when present) on the wire.
            std::deque<QueuedMessage> deferredSnapshotOutbound;
            std::size_t outboundBytes = 0;
            std::map<std::string, generated::MethodId, std::less<>> outstanding;
            std::uint64_t inboundRateTokens = 0;
            std::uint64_t lastInboundRateRefillMs = 0;
            TimerCancellation handshakeTimer;
        };

        struct ConnectionToken {
            ConnectionIdentity identity;
            std::uint64_t generation = 0;
        };

        struct ConnectionContinuation {
            // A callback continuation is valid only while both the physical
            // generation and the exact Hello/closing lifecycle remain stable.
            ConnectionToken token;
            bool helloComplete = false;
            bool closing = false;
        };

        class DispatchScope {
        public:
            explicit DispatchScope(Impl& owner) noexcept
                : owner(owner) {
                ++owner.dispatchDepth;
            }

            DispatchScope(const DispatchScope&) = delete;
            DispatchScope& operator=(const DispatchScope&) = delete;

            ~DispatchScope() {
                owner.leaveDispatch();
            }

        private:
            Impl& owner;
        };

        class BackendSnapshotScope {
        public:
            explicit BackendSnapshotScope(Impl& owner) noexcept
                : owner(owner) {
                ++owner.backendSnapshotDepth;
            }

            BackendSnapshotScope(const BackendSnapshotScope&) = delete;
            BackendSnapshotScope& operator=(const BackendSnapshotScope&) = delete;

            ~BackendSnapshotScope() {
                if (owner.backendSnapshotDepth != 0) {
                    --owner.backendSnapshotDepth;
                }
            }

        private:
            Impl& owner;
        };

        struct FailedAuthenticationWindow {
            std::size_t failures = 0;
            std::uint64_t expiresAtMs = 0;
            std::uint64_t generation = 0;
            TimerCancellation expirationTimer;
        };

        struct StoredPendingOccurrence {
            model::OccurrenceDraft occurrence;
            std::uint64_t insertionOrder = 0;
        };

        enum class BatchBuildStatus { Success, SnapshotRequired, Failure };

        enum class PendingSnapshotSequenceMode { None, ReuseCommittedSequence, AdvanceSequence };

        struct BatchBuildResult {
            BatchBuildStatus status = BatchBuildStatus::Failure;
            std::vector<EventBatch> batches;
        };

        struct FrozenSnapshotRecipient {
            ConnectionToken token;
            model::ProjectionContext projection;
            std::vector<FrontendCapability> negotiatedCapabilities;
        };

        struct SnapshotBarrier {
            model::FrontendSequence sequence;
            model::ControllerState controller;
            std::vector<model::SessionState> sessions;
            std::optional<model::FrontendSequence> oldestReplayableAfter;
            std::optional<model::FrontendSequence> oldestRetainedSequence;
            std::optional<model::FrontendSequence> newestRetainedSequence;
            bool sequenceExhausted = false;
            std::uint64_t providerLifecycleGeneration = 0;
        };

        BackendPort& backend;
        ServerCoreOptions options;
        model::ProjectionAuthority projection;
        std::unique_ptr<model::TypedOccurrenceJournal> journal;
        bool open = false;
        bool terminallyClosed = false;
        bool scheduled = false;
        bool flushRequested = false;
        bool flushActive = false;
        bool deferredPumpActive = false;
        bool schedulerInvocationActive = false;
        std::size_t dispatchDepth = 0;
        std::size_t backendSnapshotDepth = 0;
        std::uint64_t nextFlushGeneration = 1;
        std::uint64_t scheduledFlushGeneration = 0;
        std::optional<std::uint64_t> inlineScheduledFlush;
        bool sequenceExhausted = false;
        std::uint64_t nextConnectionIdentity = 1;
        std::uint64_t nextConnectionGeneration = 1;
        std::uint64_t nextSessionIdentity = 1;
        std::uint64_t nextExternalSessionIdentity = std::numeric_limits<std::uint64_t>::max();
        std::map<ConnectionIdentity, Connection> connections;
        std::map<std::string, FailedAuthenticationWindow, std::less<>> authenticationFailures;
        std::uint64_t nextAuthenticationFailureGeneration = 0;
        std::optional<ConnectionIdentity> controller;
        std::map<model::SessionIdentity, model::SessionState> externalSessions;
        std::optional<model::SessionIdentity> externalController;
        std::optional<CommandToken> controllerTransaction;
        std::optional<ProviderLifecycleAction> providerLifecycleAction;
        std::uint64_t providerLifecycleGeneration = 0;
        std::map<FrontendTransportKind, std::size_t> transportFamilies;
        std::map<OccurrenceCoalescingKey, StoredPendingOccurrence> dirtyOccurrences;
        std::uint64_t nextDirtyInsertionOrder = 0;
        bool dirtySnapshotRequired = false;
        std::vector<model::CanonicalOccurrence> pendingDelivery;
        std::size_t pendingDeliveryGroups = 0;
        PendingSnapshotSequenceMode pendingDeliverySnapshotMode = PendingSnapshotSequenceMode::None;

        [[nodiscard]] std::uint64_t now() const noexcept {
            try {
                return options.monotonicClockMs();
            } catch (...) {
                return 0;
            }
        }

        [[nodiscard]] std::vector<FrontendCapability> implementedCapabilitiesLocked() const {
            std::vector<FrontendCapability> result;
            for (const generated::CapabilityMetadata& metadata : generated::AllCapabilities) {
                const auto capability = frontendCapabilityFromString(metadata.key);
                if (!capability) {
                    continue;
                }
                bool implemented =
                    metadata.category == generated::CapabilityCategory::StaticMechanism && metadata.implementedByCurrentRuntime;
                if (metadata.category == generated::CapabilityCategory::ConditionalTopology) {
                    implemented = *capability == FrontendCapability::MultiTransport && transportFamilies.size() > 1;
                }
                if (metadata.category == generated::CapabilityCategory::Product) {
                    implemented = false;
                }
                if (implemented) {
                    addCapability(result, *capability);
                }
            }
            for (const FrontendCapability capability : options.implementedCapabilities) {
                addCapability(result, capability);
            }
            return result;
        }

        [[nodiscard]] bool methodEnabled(const generated::MethodMetadata& metadata) const noexcept {
            if (!metadata.currentlyImplemented) {
                return false;
            }
            if (metadata.defaultEnabled) {
                return true;
            }

            if (metadata.capability == "conditional_command_execution") {
                return options.enableCommandExecutionMethods && static_cast<bool>(options.commandExecutionPolicy);
            }
            if (metadata.capability == "conditional_filesystem") {
                const bool writes =
                    std::find(metadata.requiredScopes.begin(), metadata.requiredScopes.end(), FrontendScope::FilesystemWrite) !=
                    metadata.requiredScopes.end();
                return writes ? options.enableFilesystemWriteMethods && static_cast<bool>(options.filesystemWritePolicy)
                              : options.enableFilesystemReadMethods && static_cast<bool>(options.filesystemReadPolicy);
            }
            return false;
        }

        [[nodiscard]] bool scopesPermit(const FrontendPrincipal& principal, const generated::MethodMetadata& metadata) const noexcept {
            return std::all_of(metadata.requiredScopes.begin(), metadata.requiredScopes.end(), [&](FrontendScope required) {
                return containsScope(principal, required);
            });
        }

        [[nodiscard]] std::optional<bool> invocationPolicyPermits(ConnectionContinuation continuation,
                                                                  const FrontendPrincipal& principal,
                                                                  const generated::MethodMetadata& metadata,
                                                                  const Json& parameters) const noexcept {
            try {
                for (const FrontendScope scope : metadata.requiredScopes) {
                    if (scope == FrontendScope::FilesystemRead && options.filesystemReadPolicy &&
                        !options.filesystemReadPolicy(principal, metadata.method, parameters)) {
                        return false;
                    }
                    if (!findConnection(continuation)) {
                        return std::nullopt;
                    }
                    if (scope == FrontendScope::FilesystemWrite && options.filesystemWritePolicy &&
                        !options.filesystemWritePolicy(principal, metadata.method, parameters)) {
                        return false;
                    }
                    if (!findConnection(continuation)) {
                        return std::nullopt;
                    }
                    if (scope == FrontendScope::CommandExecution && options.commandExecutionPolicy &&
                        !options.commandExecutionPolicy(principal, metadata.method, parameters)) {
                        return false;
                    }
                    if (!findConnection(continuation)) {
                        return std::nullopt;
                    }
                }
                return true;
            } catch (...) {
                return false;
            }
        }

        void setProviderLifecycleAction(std::optional<ProviderLifecycleAction> action) noexcept {
            providerLifecycleAction = action;
            ++providerLifecycleGeneration;
            if (providerLifecycleGeneration == 0) {
                providerLifecycleGeneration = 1;
            }
        }

        void refreshProviderLifecycleAction(const model::ProviderState& provider) noexcept {
            if (!providerLifecycleAction) {
                return;
            }
            const bool complete =
                *providerLifecycleAction == ProviderLifecycleAction::Stop
                    ? provider.lifecycle == model::ProviderLifecycle::Stopped ||
                          (provider.lifecycle == model::ProviderLifecycle::Failed && !provider.desiredRunning)
                    : provider.lifecycle == model::ProviderLifecycle::Ready || provider.lifecycle == model::ProviderLifecycle::Failed;
            if (complete) {
                setProviderLifecycleAction(std::nullopt);
            }
        }

        [[nodiscard]] bool providerLifecycleActionValid(ProviderLifecycleAction action,
                                                        const model::ProviderState& provider) const noexcept {
            switch (action) {
                case ProviderLifecycleAction::Start:
                    return provider.lifecycle == model::ProviderLifecycle::Stopped && !provider.desiredRunning;
                case ProviderLifecycleAction::Stop:
                    return provider.lifecycle != model::ProviderLifecycle::Stopped &&
                           provider.lifecycle != model::ProviderLifecycle::Stopping;
                case ProviderLifecycleAction::Restart:
                    return provider.lifecycle != model::ProviderLifecycle::Stopping;
            }
            return false;
        }

        [[nodiscard]] std::vector<FrontendMethod> definedMethodsLocked() const {
            std::vector<FrontendMethod> result;
            result.reserve(generated::AllMethods.size());
            for (const generated::MethodMetadata& metadata : generated::AllMethods) {
                result.emplace_back(metadata.method);
            }
            return result;
        }

        [[nodiscard]] std::vector<FrontendMethod> implementedMethodsLocked() const {
            std::vector<FrontendMethod> result;
            for (const generated::MethodMetadata& metadata : generated::AllMethods) {
                if (metadata.currentlyImplemented) {
                    result.emplace_back(metadata.method);
                }
            }
            return result;
        }

        [[nodiscard]] std::vector<FrontendMethod> availableMethodsLocked() const {
            std::vector<FrontendMethod> result;
            for (const generated::MethodMetadata& metadata : generated::AllMethods) {
                if (methodEnabled(metadata)) {
                    result.emplace_back(metadata.method);
                }
            }
            return result;
        }

        [[nodiscard]] std::vector<FrontendMethod> permittedMethodsLocked(const FrontendPrincipal& principal) const {
            std::vector<FrontendMethod> result;
            if (!validPrincipal(principal)) {
                return result;
            }
            for (const generated::MethodMetadata& metadata : generated::AllMethods) {
                if (methodEnabled(metadata) && scopesPermit(principal, metadata)) {
                    result.emplace_back(metadata.method);
                }
            }
            return result;
        }

        [[nodiscard]] AuthenticationResult authenticateLocal(const FrontendPeerContext& peer, const AuthenticationCredential&) const {
            const bool verifiedLocal = options.allowVerifiedLocalTrust && peer.transport == FrontendTransportKind::Unix &&
                                       options.trustedLocalUserId && peer.localPeer && peer.unixUserId &&
                                       *peer.unixUserId == *options.trustedLocalUserId;
            const bool explicitlyInsecureLocal = options.allowInsecureLocalTrust && peer.transport == FrontendTransportKind::Unix;
            if (verifiedLocal || explicitlyInsecureLocal) {
                FrontendPrincipal principal;
                principal.id = verifiedLocal ? "unix-user-" + std::to_string(*peer.unixUserId) : "insecure-local-override";
                principal.scopes.assign(LocalTrustedScopes.begin(), LocalTrustedScopes.end());
                principal.profile = std::string(LocalTrustedScopeProfile.name);
                principal.localTrusted = true;
                return AuthenticationSuccess{std::move(principal)};
            }
            return AuthenticationFailure{AuthenticationFailureCode::AuthenticationRequired};
        }

        void expireAuthenticationFailuresLocked(std::uint64_t current) noexcept {
            for (auto iterator = authenticationFailures.begin(); iterator != authenticationFailures.end();) {
                if (current >= iterator->second.expiresAtMs) {
                    iterator = authenticationFailures.erase(iterator);
                } else {
                    ++iterator;
                }
            }
        }

        [[nodiscard]] bool authenticationRateLimitedLocked(const FrontendPeerContext& peer) noexcept {
            const std::uint64_t current = now();
            expireAuthenticationFailuresLocked(current);
            if (options.maximumFailedAuthenticationsPerPeer == 0 || options.failedAuthenticationWindowMs == 0) {
                return true;
            }
            const std::string key = peerAdmissionKey(peer);
            const auto found = authenticationFailures.find(key);
            if (found != authenticationFailures.end()) {
                return found->second.failures >= options.maximumFailedAuthenticationsPerPeer;
            }
            return authenticationFailures.size() >= options.maxConnections;
        }

        [[nodiscard]] AuthenticationFailureCode recordAuthenticationFailureLocked(const FrontendPeerContext& peer,
                                                                                  AuthenticationFailureCode failure) {
            if (!open || terminallyClosed) {
                return AuthenticationFailureCode::RateLimited;
            }
            const bool rateLimited = authenticationRateLimitedLocked(peer);
            if (!open || terminallyClosed) {
                return AuthenticationFailureCode::RateLimited;
            }
            if (rateLimited) {
                return AuthenticationFailureCode::RateLimited;
            }

            const std::uint64_t current = now();
            if (!open || terminallyClosed) {
                return AuthenticationFailureCode::RateLimited;
            }
            const std::string key = peerAdmissionKey(peer);
            std::uint64_t generation = 0;
            {
                auto [found, inserted] = authenticationFailures.try_emplace(key);
                FailedAuthenticationWindow& window = found->second;
                if (window.failures < std::numeric_limits<std::size_t>::max()) {
                    ++window.failures;
                }
                if (!inserted) {
                    return failure;
                }

                window.expiresAtMs = saturatingAdd(current, options.failedAuthenticationWindowMs);
                if (nextAuthenticationFailureGeneration < std::numeric_limits<std::uint64_t>::max()) {
                    ++nextAuthenticationFailureGeneration;
                }
                window.generation = std::max<std::uint64_t>(1, nextAuthenticationFailureGeneration);
                generation = window.generation;
            }
            if (options.timerScheduler) {
                const std::weak_ptr<Impl> weak = weak_from_this();
                try {
                    TimerCancellation cancellation = options.timerScheduler(options.failedAuthenticationWindowMs, [weak, key, generation] {
                        if (const std::shared_ptr<Impl> self = weak.lock()) {
                            DispatchScope dispatch(*self);
                            const auto currentWindow = self->authenticationFailures.find(key);
                            if (currentWindow != self->authenticationFailures.end() && currentWindow->second.generation == generation) {
                                currentWindow->second.expirationTimer = {};
                                self->authenticationFailures.erase(currentWindow);
                            }
                        }
                    });
                    const auto currentWindow = authenticationFailures.find(key);
                    if (currentWindow != authenticationFailures.end() && currentWindow->second.generation == generation) {
                        currentWindow->second.expirationTimer = std::move(cancellation);
                    } else if (cancellation) {
                        cancellation();
                    }
                } catch (...) {
                    const auto currentWindow = authenticationFailures.find(key);
                    if (currentWindow != authenticationFailures.end()) {
                        currentWindow->second.failures = options.maximumFailedAuthenticationsPerPeer;
                    }
                }
            }
            return failure;
        }

        [[nodiscard]] bool inboundAllowed(Connection& connection, std::uint64_t current) {
            if (options.maxInboundMessagesPerSecond == 0 || options.maxInboundBurst == 0) {
                return false;
            }
            const std::uint64_t elapsed = current >= connection.lastInboundRateRefillMs ? current - connection.lastInboundRateRefillMs : 0;
            if (elapsed != 0) {
                const std::uint64_t rate = static_cast<std::uint64_t>(options.maxInboundMessagesPerSecond);
                const std::uint64_t added =
                    elapsed > std::numeric_limits<std::uint64_t>::max() / rate ? std::numeric_limits<std::uint64_t>::max() : elapsed * rate;
                const std::uint64_t capacity = saturatingMultiply(options.maxInboundBurst, 1000);
                const std::uint64_t bounded = std::min(capacity, connection.inboundRateTokens);
                connection.inboundRateTokens = added > capacity - bounded ? capacity : bounded + added;
                connection.lastInboundRateRefillMs = current;
            }
            if (connection.inboundRateTokens < 1000) {
                return false;
            }
            connection.inboundRateTokens -= 1000;
            return true;
        }

        [[nodiscard]] model::ProjectionContext projectionContext(ConnectionIdentity identity, const Connection& connection) const;

        [[nodiscard]] Connection* findConnection(ConnectionIdentity identity) noexcept;
        [[nodiscard]] const Connection* findConnection(ConnectionIdentity identity) const noexcept;
        [[nodiscard]] Connection* findConnection(ConnectionToken token) noexcept;
        [[nodiscard]] const Connection* findConnection(ConnectionToken token) const noexcept;
        [[nodiscard]] Connection* findConnection(ConnectionContinuation continuation) noexcept;
        [[nodiscard]] const Connection* findConnection(ConnectionContinuation continuation) const noexcept;
        [[nodiscard]] std::optional<ConnectionToken> connectionToken(ConnectionIdentity identity) const noexcept;
        void closeNow(ConnectionIdentity identity, ConnectionClose close) noexcept;
        void closeAfterQueuedMessages(ConnectionIdentity identity, ConnectionClose close);
        [[nodiscard]] bool enqueueQueued(ConnectionIdentity identity, ServerMessage message, bool deferredSnapshot);
        [[nodiscard]] bool enqueue(ConnectionIdentity identity, ServerMessage message);
        [[nodiscard]] bool enqueueDeferredSnapshot(ConnectionIdentity identity, ServerMessage message);
        [[nodiscard]] bool releaseDeferredSnapshots() noexcept;
        void leaveDispatch() noexcept;
        void requestFlush() noexcept;
        void pumpDeferredFlushes() noexcept;
        void invokeScheduledFlush(std::uint64_t generation) noexcept;
        void runFlushTurn() noexcept;
        void requirePendingDeliverySnapshot(PendingSnapshotSequenceMode mode, PublishResult* current = nullptr) noexcept;
        void flushConnectionLocked(ConnectionToken token);
        void flushLocked();
        [[nodiscard]] ReceiveResult protocolFailure(ConnectionIdentity identity, CodecError error);
        [[nodiscard]] ReceiveResult rejectCommand(ConnectionIdentity identity, std::string requestId, ErrorCode code, std::string message);
        [[nodiscard]] bool beginInbound(ConnectionIdentity identity, std::size_t bytes, ReceiveResult& failure);
        [[nodiscard]] ReceiveResult receiveJsonLocked(ConnectionIdentity identity, const Json& message);
        [[nodiscard]] ReceiveResult receiveClientLocked(ConnectionIdentity identity, const ClientMessage& message);
        [[nodiscard]] ReceiveResult receiveHelloLocked(ConnectionIdentity identity, const Hello& hello);
        [[nodiscard]] ReceiveResult receiveDefinedCommandLocked(ConnectionIdentity identity, const generated::DefinedCommand& command);
        [[nodiscard]] ReceiveStatus
        executeFrontendNative(ConnectionToken token, const generated::DefinedCommand& command, const generated::MethodMetadata& metadata);
        [[nodiscard]] bool respondSuccess(ConnectionIdentity identity, std::string requestId, generated::MethodId method, Json value);
        [[nodiscard]] bool respondFailure(ConnectionIdentity identity,
                                          std::string requestId,
                                          ErrorCode code,
                                          std::string message,
                                          std::optional<model::SafeDetail> details = std::nullopt);
        [[nodiscard]] SnapshotBarrier captureSnapshotBarrier() const;
        [[nodiscard]] model::CanonicalSnapshot applySnapshotBarrier(model::CanonicalSnapshot snapshot, const SnapshotBarrier& barrier);
        [[nodiscard]] std::optional<FrozenSnapshotRecipient> freezeSnapshotRecipient(ConnectionToken token) const;
        [[nodiscard]] bool enqueueFrozenSnapshot(const FrozenSnapshotRecipient& recipient,
                                                 const model::CanonicalSnapshot& snapshot,
                                                 bool deferUntilSnapshotBarrier = false);
        [[nodiscard]] std::optional<model::FrontendSequence> enqueueSnapshot(ConnectionToken token,
                                                                             const SnapshotBarrier* suppliedBarrier = nullptr);
        [[nodiscard]] BatchBuildResult buildOccurrenceBatches(ConnectionIdentity identity,
                                                              const Connection& connection,
                                                              std::span<const model::CanonicalOccurrence> occurrences) const;
        [[nodiscard]] bool enqueueBuiltBatches(ConnectionIdentity identity, std::span<const EventBatch> batches);
        [[nodiscard]] bool emitOccurrencesOrSnapshot(ConnectionToken token, std::span<const model::CanonicalOccurrence> occurrences);
        void broadcastPendingDelivery();
        void drainDirtyOccurrences();
        void materializePendingDeliveryLocked();
        [[nodiscard]] PublishResult appendAndStageDelivery(model::OccurrenceDraft occurrence, bool scheduleDelivery) noexcept;
        void handleSequenceExhaustion() noexcept;
        [[nodiscard]] bool synchronizeSnapshot(ConnectionToken token, const SnapshotBarrier* suppliedBarrier = nullptr);
        [[nodiscard]] bool synchronizeReplay(ConnectionToken token, const model::JournalReplayResult& replay);
        [[nodiscard]] bool supportsExpandedOccurrence(const Connection& connection, ExpandedEventType type) const noexcept;
        [[nodiscard]] SyncMode initialSyncMode(ConnectionIdentity identity,
                                               const Connection& connection,
                                               const std::optional<SequenceNumber>& requestedAfter) const;
        [[nodiscard]] bool complete(BackendCompletion completion);
        [[nodiscard]] OccurrenceStageResult
        stageOccurrenceLocked(OccurrenceCoalescingKey key, model::OccurrenceDraft occurrence, OccurrenceFlushUrgency urgency) noexcept;
        [[nodiscard]] std::vector<model::SessionState> sessionStatesLocked() const;
        [[nodiscard]] model::ControllerState controllerStateLocked() const;
        void stageSessionChangedLocked(model::SessionState changedSession, bool connected) noexcept;
        void stageControllerChangedLocked() noexcept;
    };

    ServerCore::Impl::Connection* ServerCore::Impl::findConnection(ConnectionIdentity identity) noexcept {
        const auto found = connections.find(identity);
        return found == connections.end() ? nullptr : &found->second;
    }

    const ServerCore::Impl::Connection* ServerCore::Impl::findConnection(ConnectionIdentity identity) const noexcept {
        const auto found = connections.find(identity);
        return found == connections.end() ? nullptr : &found->second;
    }

    ServerCore::Impl::Connection* ServerCore::Impl::findConnection(ConnectionToken token) noexcept {
        Connection* connection = findConnection(token.identity);
        return connection && connection->generation == token.generation ? connection : nullptr;
    }

    const ServerCore::Impl::Connection* ServerCore::Impl::findConnection(ConnectionToken token) const noexcept {
        const Connection* connection = findConnection(token.identity);
        return connection && connection->generation == token.generation ? connection : nullptr;
    }

    ServerCore::Impl::Connection* ServerCore::Impl::findConnection(ConnectionContinuation continuation) noexcept {
        Connection* connection = findConnection(continuation.token);
        return connection && connection->helloComplete == continuation.helloComplete && connection->closing == continuation.closing
                   ? connection
                   : nullptr;
    }

    const ServerCore::Impl::Connection* ServerCore::Impl::findConnection(ConnectionContinuation continuation) const noexcept {
        const Connection* connection = findConnection(continuation.token);
        return connection && connection->helloComplete == continuation.helloComplete && connection->closing == continuation.closing
                   ? connection
                   : nullptr;
    }

    std::optional<ServerCore::Impl::ConnectionToken> ServerCore::Impl::connectionToken(ConnectionIdentity identity) const noexcept {
        const Connection* connection = findConnection(identity);
        if (!connection) {
            return std::nullopt;
        }
        return ConnectionToken{identity, connection->generation};
    }

    void ServerCore::Impl::closeNow(ConnectionIdentity identity, ConnectionClose close) noexcept {
        TimerCancellation cancellation;
        ConnectionCallbacks callbacks;
        std::optional<model::SessionState> closedSessionState;
        std::optional<FrontendSessionToken> closedSession;
        bool releasedController = false;
        {
            const auto found = connections.find(identity);
            if (found == connections.end()) {
                return;
            }
            cancellation = std::move(found->second.handshakeTimer);
            callbacks = std::move(found->second.callbacks);
            if (found->second.session) {
                closedSessionState.emplace(*found->second.session);
                closedSessionState->role = SessionRole::Observer;
            }
            if (found->second.session) {
                closedSession.emplace(identity, found->second.generation, *found->second.session);
            }
            releasedController = controller && *controller == identity;
            if (releasedController) {
                controller.reset();
            }
            if (controllerTransaction && controllerTransaction->connection == identity) {
                controllerTransaction.reset();
            }
            connections.erase(found);
        }

        if (releasedController) {
            stageControllerChangedLocked();
        }
        if (closedSessionState) {
            stageSessionChangedLocked(std::move(*closedSessionState), false);
        }

        // The connection is absent from the authority before any callback can
        // synchronously re-enter the owner event loop.
        if (cancellation) {
            try {
                cancellation();
            } catch (...) {
            }
        }
        if (closedSession) {
            try {
                backend.sessionClosed(*closedSession);
            } catch (...) {
            }
        }
        if (callbacks.onClosed) {
            try {
                callbacks.onClosed(close);
            } catch (...) {
            }
        }
    }

    void ServerCore::Impl::closeAfterQueuedMessages(ConnectionIdentity identity, ConnectionClose close) {
        Connection* connection = findConnection(identity);
        if (!connection) {
            return;
        }
        connection->closing = true;
        connection->closeAfterDrain = std::move(close);
        if (connection->outbound.empty()) {
            const ConnectionClose drained = *connection->closeAfterDrain;
            connection = nullptr;
            closeNow(identity, drained);
        }
    }

    bool ServerCore::Impl::enqueueQueued(ConnectionIdentity identity, ServerMessage message, bool deferredSnapshot) {
        Connection* connection = findConnection(identity);
        if (!connection) {
            return false;
        }

        const auto encoded = Codec::serializeServer(message);
        if (!encoded) {
            closeNow(identity, ConnectionClose{"frontend protocol encoding failed", ErrorCode::InternalError, false});
            return false;
        }
        const std::size_t bytes = encoded.value().size();
        const bool messageCapacityExceeded =
            connection->outbound.size() >= options.maxOutboundMessagesPerConnection ||
            connection->deferredSnapshotOutbound.size() >= options.maxOutboundMessagesPerConnection - connection->outbound.size();
        if (messageCapacityExceeded || bytes > options.maxOutboundBytesPerConnection ||
            connection->outboundBytes > options.maxOutboundBytesPerConnection - bytes) {
            closeNow(identity, ConnectionClose{"frontend outbound backpressure limit exceeded", ErrorCode::CapacityExceeded, false});
            return false;
        }

        try {
            std::deque<QueuedMessage>& destination = deferredSnapshot ? connection->deferredSnapshotOutbound : connection->outbound;
            destination.push_back(QueuedMessage{std::move(message), bytes});
            connection->outboundBytes += bytes;
            return true;
        } catch (...) {
            closeNow(identity, ConnectionClose{"frontend outbound queue allocation failed", ErrorCode::InternalError, false});
            return false;
        }
    }

    bool ServerCore::Impl::enqueue(ConnectionIdentity identity, ServerMessage message) {
        return enqueueQueued(identity, std::move(message), false);
    }

    bool ServerCore::Impl::enqueueDeferredSnapshot(ConnectionIdentity identity, ServerMessage message) {
        return enqueueQueued(identity, std::move(message), true);
    }

    bool ServerCore::Impl::releaseDeferredSnapshots() noexcept {
        bool released = false;
        std::vector<ConnectionToken> recipients;
        try {
            recipients.reserve(connections.size());
            for (const auto& [identity, connection] : connections) {
                if (!connection.deferredSnapshotOutbound.empty()) {
                    recipients.push_back(ConnectionToken{identity, connection.generation});
                }
            }
        } catch (...) {
            return false;
        }

        for (const ConnectionToken token : recipients) {
            Connection* connection = findConnection(token);
            if (!connection || connection->deferredSnapshotOutbound.empty()) {
                continue;
            }
            try {
                connection->outbound.insert(connection->outbound.end(),
                                            std::make_move_iterator(connection->deferredSnapshotOutbound.begin()),
                                            std::make_move_iterator(connection->deferredSnapshotOutbound.end()));
                connection->deferredSnapshotOutbound.clear();
                released = true;
            } catch (...) {
                connection = nullptr;
                closeNow(token.identity, ConnectionClose{"frontend deferred Snapshot queueing failed", ErrorCode::InternalError, false});
            }
        }
        return released;
    }

    void ServerCore::Impl::leaveDispatch() noexcept {
        if (dispatchDepth != 0) {
            --dispatchDepth;
        }
        if (dispatchDepth == 0) {
            // Keep deferred live Snapshots behind every message constructed by
            // the outer snapshot transaction, including SyncComplete.
            dispatchDepth = 1;
            const bool releasedSnapshots = releaseDeferredSnapshots();
            dispatchDepth = 0;
            if (releasedSnapshots && open && !terminallyClosed) {
                flushRequested = true;
            }
            pumpDeferredFlushes();
        }
    }

    void ServerCore::Impl::requestFlush() noexcept {
        if (terminallyClosed || !open) {
            return;
        }
        flushRequested = true;
        if (dispatchDepth == 0 && !flushActive && !deferredPumpActive) {
            pumpDeferredFlushes();
        }
    }

    void ServerCore::Impl::pumpDeferredFlushes() noexcept {
        if (dispatchDepth != 0 || flushActive || deferredPumpActive) {
            return;
        }

        deferredPumpActive = true;
        while (dispatchDepth == 0 && !flushActive) {
            if (inlineScheduledFlush) {
                const std::uint64_t generation = *inlineScheduledFlush;
                inlineScheduledFlush.reset();
                invokeScheduledFlush(generation);
                continue;
            }
            if (!flushRequested || scheduled) {
                break;
            }

            flushRequested = false;
            scheduled = true;
            const std::uint64_t generation = nextFlushGeneration++;
            if (nextFlushGeneration == 0) {
                nextFlushGeneration = 1;
            }
            scheduledFlushGeneration = generation;
            const std::weak_ptr<Impl> weak = weak_from_this();
            const auto callback = [weak, generation] {
                if (const std::shared_ptr<Impl> self = weak.lock()) {
                    if (self->dispatchDepth != 0 || self->flushActive || self->schedulerInvocationActive) {
                        if (self->scheduled && self->scheduledFlushGeneration == generation) {
                            self->inlineScheduledFlush = generation;
                        }
                        return;
                    }
                    self->invokeScheduledFlush(generation);
                }
            };

            if (!options.scheduler) {
                inlineScheduledFlush = generation;
                continue;
            }
            schedulerInvocationActive = true;
            try {
                options.scheduler(callback);
            } catch (...) {
                if (scheduled && scheduledFlushGeneration == generation) {
                    inlineScheduledFlush = generation;
                }
            }
            schedulerInvocationActive = false;
            if (!inlineScheduledFlush) {
                break;
            }
        }
        deferredPumpActive = false;
    }

    void ServerCore::Impl::invokeScheduledFlush(std::uint64_t generation) noexcept {
        if (!scheduled || scheduledFlushGeneration != generation) {
            return;
        }
        scheduled = false;
        scheduledFlushGeneration = 0;
        flushRequested = false;
        DispatchScope dispatch(*this);
        runFlushTurn();
    }

    void ServerCore::Impl::runFlushTurn() noexcept {
        if (flushActive) {
            flushRequested = true;
            return;
        }
        flushActive = true;
        try {
            flushLocked();
        } catch (...) {
            dirtyOccurrences.clear();
            nextDirtyInsertionOrder = 0;
            dirtySnapshotRequired = false;
            requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence);
        }
        flushActive = false;
    }

    void ServerCore::Impl::requirePendingDeliverySnapshot(PendingSnapshotSequenceMode mode, PublishResult* current) noexcept {
        if (mode == PendingSnapshotSequenceMode::None) {
            return;
        }
        pendingDelivery.clear();
        pendingDeliveryGroups = 0;
        switch (pendingDeliverySnapshotMode) {
            case PendingSnapshotSequenceMode::None:
                pendingDeliverySnapshotMode = mode;
                break;
            case PendingSnapshotSequenceMode::ReuseCommittedSequence:
                if (mode == PendingSnapshotSequenceMode::AdvanceSequence) {
                    pendingDeliverySnapshotMode = PendingSnapshotSequenceMode::AdvanceSequence;
                }
                break;
            case PendingSnapshotSequenceMode::AdvanceSequence:
                break;
        }
        if (current != nullptr && current->accepted) {
            current->deliveryMode = PublishDeliveryMode::SnapshotFallback;
        }
    }

    void ServerCore::Impl::flushConnectionLocked(ConnectionToken token) {
        if (!findConnection(token)) {
            return;
        }
        if (options.maxMessagesPerDelivery == 0) {
            closeNow(token.identity, ConnectionClose{"frontend delivery limit is zero", std::nullopt, false});
            return;
        }
        std::size_t delivered = 0;
        while (delivered < options.maxMessagesPerDelivery) {
            Connection* connection = findConnection(token);
            if (!connection || connection->outbound.empty()) {
                break;
            }

            // Account and remove the message before entering transport code.
            QueuedMessage queued = std::move(connection->outbound.front());
            connection->outbound.pop_front();
            connection->outboundBytes -= queued.bytes;
            ConnectionCallbacks::Send send = connection->callbacks.onMessage;
            const ConnectionContinuation continuation{token, connection->helloComplete, connection->closing};
            connection = nullptr;
            bool accepted = false;
            if (send) {
                try {
                    accepted = send(queued.message);
                } catch (...) {
                    if (findConnection(continuation)) {
                        closeNow(token.identity, ConnectionClose{"frontend outbound callback threw", ErrorCode::CapacityExceeded, false});
                    }
                    return;
                }
            }
            if (!findConnection(continuation)) {
                return;
            }
            if (!accepted) {
                closeNow(token.identity, ConnectionClose{"frontend transport rejected outbound data", ErrorCode::CapacityExceeded, false});
                return;
            }
            ++delivered;
        }

        std::optional<ConnectionClose> drainedClose;
        Connection* connection = findConnection(token);
        if (connection && connection->outbound.empty() && connection->closeAfterDrain) {
            drainedClose = *connection->closeAfterDrain;
        }
        connection = nullptr;
        if (drainedClose) {
            closeNow(token.identity, *drainedClose);
        }
    }

    void ServerCore::Impl::flushLocked() {
        try {
            drainDirtyOccurrences();
            broadcastPendingDelivery();
        } catch (...) {
            dirtyOccurrences.clear();
            nextDirtyInsertionOrder = 0;
            dirtySnapshotRequired = false;
            requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence);
        }

        std::vector<ConnectionToken> recipients;
        recipients.reserve(connections.size());
        for (const auto& [identity, connection] : connections) {
            if (!connection.outbound.empty()) {
                recipients.push_back(ConnectionToken{identity, connection.generation});
            }
        }
        for (const ConnectionToken token : recipients) {
            flushConnectionLocked(token);
        }

        const bool pending = std::any_of(connections.begin(), connections.end(), [](const auto& entry) {
            return !entry.second.outbound.empty();
        });
        if (pending) {
            requestFlush();
        }
    }

    ReceiveResult ServerCore::Impl::protocolFailure(ConnectionIdentity identity, CodecError error) {
        const Connection* connection = findConnection(identity);
        if (!connection) {
            return {ReceiveStatus::UnknownConnection, std::move(error)};
        }
        const bool authenticated = connection->principal.has_value();
        const bool helloComplete = connection->helloComplete;
        connection = nullptr;

        // Before authentication, expose only envelope/admission failures. A
        // malformed command must not reveal the privileged method vocabulary.
        if (!authenticated) {
            switch (error.code) {
                case ErrorCode::MalformedJson:
                case ErrorCode::WrongProtocol:
                case ErrorCode::UnsupportedVersion:
                case ErrorCode::FrameTooLarge:
                case ErrorCode::RateLimited:
                case ErrorCode::AuthenticationRequired:
                case ErrorCode::AuthenticationFailed:
                case ErrorCode::OriginRejected:
                case ErrorCode::TransportSecurityRequired:
                    break;
                default:
                    error = codecFailure(ErrorCode::InvalidField, "frontend handshake message is invalid");
                    break;
            }
        }
        if (!helloComplete || error.code == ErrorCode::MalformedJson || error.code == ErrorCode::WrongProtocol ||
            error.code == ErrorCode::UnsupportedVersion) {
            error.closeConnection = true;
        }
        if (error.code == ErrorCode::UnsupportedVersion) {
            error.message = "unsupported frontend protocol version";
        }

        ProtocolErrorMessage message;
        message.code = error.code;
        message.message = error.message;
        message.supportedVersions = error.supportedVersions.empty()
                                        ? std::vector<std::uint32_t>(SupportedProtocolVersions.begin(), SupportedProtocolVersions.end())
                                        : error.supportedVersions;
        message.closeConnection = error.closeConnection;
        message.requestId = error.requestId;
        message.details = error.details;
        const bool queued = enqueue(identity, ServerMessage{std::move(message)});
        if (!queued) {
            return {ReceiveStatus::Closed, std::move(error)};
        }
        if (error.closeConnection) {
            closeAfterQueuedMessages(identity, ConnectionClose{"frontend protocol requested connection close", error.code, false});
        }
        requestFlush();
        return {error.closeConnection ? ReceiveStatus::Closing : ReceiveStatus::Rejected, std::move(error)};
    }

    bool ServerCore::Impl::respondFailure(
        ConnectionIdentity identity, std::string requestId, ErrorCode code, std::string message, std::optional<model::SafeDetail> details) {
        const bool queued = enqueue(
            identity, ServerMessage{Response::failure(std::move(requestId), commandError(code, std::move(message), std::move(details)))});
        if (queued) {
            requestFlush();
        }
        return queued;
    }

    ReceiveResult ServerCore::Impl::rejectCommand(ConnectionIdentity identity, std::string requestId, ErrorCode code, std::string message) {
        const std::string diagnostic = message;
        if (!respondFailure(identity, std::move(requestId), code, std::move(message))) {
            return {ReceiveStatus::Closed, codecFailure(code, diagnostic, false)};
        }
        return {ReceiveStatus::Rejected, codecFailure(code, diagnostic, false)};
    }

    bool ServerCore::Impl::beginInbound(ConnectionIdentity identity, std::size_t bytes, ReceiveResult& failure) {
        const std::optional<ConnectionToken> token = connectionToken(identity);
        std::optional<ConnectionContinuation> continuation;
        {
            const Connection* connection = token ? findConnection(*token) : nullptr;
            if (!connection) {
                failure = {ReceiveStatus::UnknownConnection, std::nullopt};
                return false;
            }
            if (connection->closing) {
                failure = {ReceiveStatus::Closing, std::nullopt};
                return false;
            }
            continuation = ConnectionContinuation{*token, connection->helloComplete, false};
        }
        if (options.maximumInboundMessageBytes == 0 || bytes > options.maximumInboundMessageBytes) {
            failure = protocolFailure(identity, codecFailure(ErrorCode::FrameTooLarge, "frontend inbound message exceeds frame capacity"));
            return false;
        }
        const std::uint64_t current = now();
        Connection* connection = findConnection(*continuation);
        if (!connection) {
            failure = {ReceiveStatus::Closed, std::nullopt};
            return false;
        }
        if (!inboundAllowed(*connection, current)) {
            failure = protocolFailure(identity, codecFailure(ErrorCode::RateLimited, "frontend inbound message rate limit exceeded"));
            return false;
        }
        return true;
    }

    model::ProjectionContext ServerCore::Impl::projectionContext(ConnectionIdentity identity, const Connection& connection) const {
        model::ProjectionContext context;
        context.principal = *connection.principal;
        context.selectedCapabilities = connection.negotiatedCapabilities;
        context.peer = connection.peer;
        context.controllerOwned = controller && *controller == identity;
        return context;
    }

    ReceiveResult ServerCore::Impl::receiveJsonLocked(ConnectionIdentity identity, const Json& message) {
        if (!message.is_object()) {
            return protocolFailure(identity, codecFailure(ErrorCode::MalformedJson, "frontend message must be an object"));
        }
        const auto kindMember = message.find("kind");
        const bool currentEnvelope = hasCurrentProtocolEnvelope(message);
        if (currentEnvelope && kindMember->get_ref<const std::string&>() != kind::Hello) {
            const Connection* connection = findConnection(identity);
            if (!connection || !connection->helloComplete || !connection->principal || !connection->session) {
                return protocolFailure(
                    identity,
                    codecFailure(ErrorCode::AuthenticationRequired, "frontend authentication must complete before commands are accepted"));
            }
        }
        if (kindMember != message.end() && kindMember->is_string() && kindMember->get_ref<const std::string&>() == kind::Command) {
            if (currentEnvelope) {
                const auto methodMember = message.find("method");
                const auto requestIdMember = message.find("requestId");
                if (methodMember != message.end() && methodMember->is_string() && requestIdMember != message.end() &&
                    requestIdMember->is_string() && !requestIdMember->get_ref<const std::string&>().empty()) {
                    const auto method = generated::definedMethodFromString(methodMember->get_ref<const std::string&>());
                    if (method && !methodEnabled(methodMetadata(*method))) {
                        CodecError error = codecFailure(ErrorCode::UnknownMethod, "frontend command method is unavailable", false);
                        error.requestId = requestIdMember->get<std::string>();
                        return protocolFailure(identity, std::move(error));
                    }
                }
            }
            const auto decoded = Codec::decodeDefinedCommand(message);
            if (!decoded) {
                return protocolFailure(identity, decoded.error());
            }
            return receiveDefinedCommandLocked(identity, decoded.value());
        }

        const auto decoded = Codec::decodeClient(message);
        if (!decoded) {
            return protocolFailure(identity, decoded.error());
        }
        return receiveClientLocked(identity, decoded.value());
    }

    ReceiveResult ServerCore::Impl::receiveClientLocked(ConnectionIdentity identity, const ClientMessage& message) {
        return std::visit(
            [&](const auto& value) -> ReceiveResult {
                using Message = std::decay_t<decltype(value)>;
                if constexpr (std::is_same_v<Message, Hello>) {
                    return receiveHelloLocked(identity, value);
                } else {
                    const auto encoded = Codec::encodeClient(ClientMessage{value});
                    if (!encoded) {
                        return protocolFailure(identity, encoded.error());
                    }
                    const auto defined = Codec::decodeDefinedCommand(encoded.value());
                    if (!defined) {
                        return protocolFailure(identity, defined.error());
                    }
                    return receiveDefinedCommandLocked(identity, defined.value());
                }
            },
            message);
    }

    SyncMode ServerCore::Impl::initialSyncMode(ConnectionIdentity identity,
                                               const Connection& connection,
                                               const std::optional<SequenceNumber>& requestedAfter) const {
        if (!requestedAfter) {
            return SyncMode::Snapshot;
        }
        const model::JournalReplayResult replay = journal->replayAfter(model::FrontendSequence{*requestedAfter});
        if (replay.status != model::JournalReplayStatus::Available) {
            return SyncMode::Snapshot;
        }
        return buildOccurrenceBatches(identity, connection, replay.records).status == BatchBuildStatus::Success ? SyncMode::Replay
                                                                                                                : SyncMode::Snapshot;
    }

    ReceiveResult ServerCore::Impl::receiveHelloLocked(ConnectionIdentity identity, const Hello& hello) {
        const std::optional<ConnectionToken> token = connectionToken(identity);
        Connection* connection = token ? findConnection(*token) : nullptr;
        if (!connection) {
            return {ReceiveStatus::UnknownConnection, std::nullopt};
        }
        if (connection->helloComplete) {
            return protocolFailure(identity, codecFailure(ErrorCode::InvalidCommand, "hello may only be sent once", false));
        }
        if (connection->helloAttempted) {
            return protocolFailure(identity,
                                   codecFailure(ErrorCode::AuthenticationFailed, "frontend authentication may only be attempted once"));
        }
        connection->helloAttempted = true;
        const FrontendPeerContext peer = connection->peer;
        const ConnectionContinuation awaitingHello{*token, false, false};
        connection = nullptr;

        std::vector<FrontendCapability> requestedCapabilities;
        if (hello.capabilities) {
            for (const FrontendCapability capability : *hello.capabilities) {
                if (containsCapability(requestedCapabilities, capability)) {
                    return protocolFailure(identity,
                                           codecFailure(ErrorCode::InvalidField, "frontend Hello capabilities contain duplicates"));
                }
                requestedCapabilities.push_back(capability);
            }
        }
        for (const FrontendCapability required : options.requiredClientCapabilities) {
            if (!containsCapability(requestedCapabilities, required)) {
                return protocolFailure(identity,
                                       codecFailure(ErrorCode::UnsupportedVersion, "frontend Hello lacks a required protocol capability"));
            }
        }

        const bool rateLimited = authenticationRateLimitedLocked(peer);
        if (!findConnection(awaitingHello)) {
            return {ReceiveStatus::Closed, std::nullopt};
        }
        if (rateLimited) {
            return protocolFailure(
                identity, codecFailure(ErrorCode::RateLimited, authenticationErrorMessage(AuthenticationFailureCode::RateLimited)));
        }

        const AuthenticationCredential credential = hello.authentication.value_or(AuthenticationCredential{NoCredential{}});
        AuthenticationResult authentication = AuthenticationFailure{AuthenticationFailureCode::AuthenticationRequired};
        authentication = authenticateLocal(peer, credential);
        if (!std::holds_alternative<AuthenticationSuccess>(authentication) && options.authenticator) {
            try {
                authentication = options.authenticator(peer, credential);
            } catch (...) {
                authentication = AuthenticationFailure{AuthenticationFailureCode::AuthenticationFailed};
            }
        }
        if (!findConnection(awaitingHello)) {
            return {ReceiveStatus::Closed, std::nullopt};
        }

        if (const auto* failure = std::get_if<AuthenticationFailure>(&authentication)) {
            const AuthenticationFailureCode failureCode = recordAuthenticationFailureLocked(peer, failure->code);
            if (!findConnection(awaitingHello)) {
                return {ReceiveStatus::Closed, std::nullopt};
            }
            const ErrorCode errorCode = protocolCode(failureCode);
            return protocolFailure(identity, codecFailure(errorCode, authenticationErrorMessage(failureCode)));
        }

        FrontendPrincipal principal = std::get<AuthenticationSuccess>(std::move(authentication)).principal;
        if (!validPrincipal(principal) || principal.id.size() > model::SessionIdentity::MaximumBytes) {
            static_cast<void>(recordAuthenticationFailureLocked(peer, AuthenticationFailureCode::AuthenticationFailed));
            if (!findConnection(awaitingHello)) {
                return {ReceiveStatus::Closed, std::nullopt};
            }
            return protocolFailure(
                identity,
                codecFailure(ErrorCode::AuthenticationFailed, authenticationErrorMessage(AuthenticationFailureCode::AuthenticationFailed)));
        }
        if (nextSessionIdentity == 0 || nextSessionIdentity > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return protocolFailure(identity, codecFailure(ErrorCode::CapacityExceeded, "frontend session identity space exhausted"));
        }
        model::SessionIdentity session(std::to_string(nextSessionIdentity++));
        const std::vector<FrontendCapability> implemented = implementedCapabilitiesLocked();
        std::vector<FrontendCapability> negotiated;
        for (const FrontendCapability requested : requestedCapabilities) {
            if (containsCapability(implemented, requested)) {
                negotiated.push_back(requested);
            }
        }

        connection = findConnection(awaitingHello);
        if (!connection) {
            return {ReceiveStatus::Closed, std::nullopt};
        }
        TimerCancellation cancellation = std::move(connection->handshakeTimer);
        connection->handshakeTimer = {};
        connection = nullptr;

        if (cancellation) {
            try {
                cancellation();
            } catch (...) {
            }
            if (!findConnection(awaitingHello)) {
                return {ReceiveStatus::Closed, std::nullopt};
            }
        }

        connection = findConnection(awaitingHello);
        if (!connection) {
            return {ReceiveStatus::Closed, std::nullopt};
        }
        connection->principal = principal;
        connection->negotiatedCapabilities = std::move(negotiated);
        const ConnectionContinuation openingSession{*token, false, false};
        const FrontendSessionToken backendSessionToken{token->identity, token->generation, session};
        connection = nullptr;

        bool backendSessionOpened = false;
        try {
            backendSessionOpened = backend.sessionOpened(backendSessionToken, principal);
        } catch (...) {
            backendSessionOpened = false;
        }
        if (!backendSessionOpened) {
            if (!findConnection(openingSession)) {
                return {ReceiveStatus::Closed, std::nullopt};
            }
            // Authentication has succeeded, so report this post-authentication
            // admission failure on the wire and close only after the bounded
            // ProtocolError has drained. No frontend session has been committed.
            return protocolFailure(identity, codecFailure(ErrorCode::BackendUnavailable, "backend rejected frontend session", false));
        }
        connection = findConnection(openingSession);
        if (!connection) {
            try {
                backend.sessionClosed(backendSessionToken);
            } catch (...) {
            }
            return {ReceiveStatus::Closed, std::nullopt};
        }
        connection->session = session;
        connection->helloComplete = true;
        const ConnectionContinuation active{*token, true, false};
        connection = findConnection(active);
        if (!connection) {
            return {ReceiveStatus::Closed, std::nullopt};
        }

        const SyncMode syncMode = initialSyncMode(identity, *connection, hello.resumeAfter);
        const SnapshotBarrier synchronizationBarrier = captureSnapshotBarrier();
        Welcome welcome;
        welcome.sessionId = session.value();
        welcome.role = SessionRole::Observer;
        welcome.currentSequence = synchronizationBarrier.sequence.protocolValue();
        welcome.syncMode = syncMode;
        if (hello.capabilities) {
            CapabilityAdvertisement advertisement;
            advertisement.defined = definedCapabilities();
            advertisement.implemented = implemented;
            advertisement.permitted = implemented;
            welcome.capabilities = std::move(advertisement);
            welcome.availableMethods = availableMethodsLocked();
            welcome.permittedMethods = permittedMethodsLocked(principal);
            Json permittedScopes = Json::array();
            for (const FrontendScope scope : principal.scopes) {
                permittedScopes.push_back(std::string(toString(scope)));
            }
            welcome.extensions["permittedScopes"] = std::move(permittedScopes);
        }
        if (!options.serverVersion.empty()) {
            welcome.serverVersion = options.serverVersion;
        }

        if (!enqueue(identity, ServerMessage{std::move(welcome)})) {
            return {ReceiveStatus::Closed, std::nullopt};
        }
        if (!findConnection(active)) {
            return {ReceiveStatus::Closed, std::nullopt};
        }

        bool synchronized = false;
        if (syncMode == SyncMode::Replay && hello.resumeAfter) {
            const model::JournalReplayResult replay = journal->replayAfter(model::FrontendSequence{*hello.resumeAfter});
            synchronized = synchronizeReplay(*token, replay);
        } else {
            synchronized = synchronizeSnapshot(*token, &synchronizationBarrier);
        }
        if (!synchronized) {
            if (findConnection(active)) {
                closeNow(identity, ConnectionClose{"frontend initial synchronization failed", ErrorCode::InternalError, false});
            }
            return {ReceiveStatus::Closing, codecFailure(ErrorCode::InternalError, "frontend synchronization failed", false)};
        }
        connection = findConnection(active);
        if (connection && connection->session) {
            model::SessionState changedSession(*connection->session);
            changedSession.role = SessionRole::Observer;
            stageSessionChangedLocked(std::move(changedSession), true);
        }
        requestFlush();
        return {ReceiveStatus::Accepted, std::nullopt};
    }

    bool ServerCore::Impl::respondSuccess(ConnectionIdentity identity, std::string requestId, generated::MethodId method, Json value) {
        generated::CompleteCommandResult typedResult = generated::makeResult(method, std::move(value));
        const auto encoded = Codec::encodeDefinedResult(typedResult);
        if (!encoded) {
            return respondFailure(identity,
                                  std::move(requestId),
                                  ErrorCode::TypedDecodingFailure,
                                  "frontend-native result violates the generated result schema");
        }
        const bool queued = enqueue(identity, ServerMessage{Response::success(std::move(requestId), encoded.value())});
        if (queued) {
            requestFlush();
        }
        return queued;
    }

    ReceiveStatus ServerCore::Impl::executeFrontendNative(ConnectionToken token,
                                                          const generated::DefinedCommand& command,
                                                          const generated::MethodMetadata& metadata) {
        const ConnectionIdentity identity = token.identity;
        const ConnectionContinuation active{token, true, false};
        const std::string requestId = command.requestId;
        const auto rejectResponse = [&](ErrorCode code, std::string message) {
            return respondFailure(identity, requestId, code, std::move(message)) ? ReceiveStatus::Rejected : ReceiveStatus::Closed;
        };
        const auto acceptResponse = [&](Json value) {
            return respondSuccess(identity, requestId, metadata.id, std::move(value)) ? ReceiveStatus::Accepted : ReceiveStatus::Closed;
        };
        switch (metadata.id) {
            case generated::MethodId::ControllerAcquire: {
                if (controllerTransaction) {
                    return rejectResponse(ErrorCode::Conflict, "another frontend controller transaction is in progress");
                }
                if (externalController || (controller && *controller != identity)) {
                    return respondFailure(identity, requestId, ErrorCode::Conflict, "frontend command conflicts with current state")
                               ? ReceiveStatus::Accepted
                               : ReceiveStatus::Closed;
                }
                Connection* connection = findConnection(token);
                if (!connection || !connection->session || !connection->principal) {
                    return ReceiveStatus::Closed;
                }
                const model::SessionIdentity session = *connection->session;
                const FrontendPrincipal principal = *connection->principal;
                const CommandToken commandToken{identity, token.generation, requestId, metadata.id};
                connection->outstanding.emplace(requestId, metadata.id);
                controllerTransaction = commandToken;
                connection = nullptr;
                BackendSubmitStatus status = BackendSubmitStatus::Rejected;
                try {
                    status = backend.submit(BackendInvocation{commandToken, session, principal, command});
                } catch (...) {
                    status = BackendSubmitStatus::Unavailable;
                }
                connection = findConnection(active);
                if (!connection) {
                    return ReceiveStatus::Closed;
                }
                if (status == BackendSubmitStatus::Accepted || !connection->outstanding.contains(requestId)) {
                    return ReceiveStatus::Accepted;
                }
                connection->outstanding.erase(requestId);
                if (controllerTransaction == commandToken) {
                    controllerTransaction.reset();
                }
                connection = nullptr;
                const ErrorCode code = status == BackendSubmitStatus::Unavailable
                                           ? ErrorCode::BackendUnavailable
                                           : (status == BackendSubmitStatus::CapacityExceeded ? ErrorCode::CapacityExceeded
                                                                                              : ErrorCode::LocalSubmissionFailure);
                return rejectResponse(code, "frontend controller acquisition submission failed");
            }
            case generated::MethodId::ControllerRelease: {
                if (controllerTransaction) {
                    return rejectResponse(ErrorCode::Conflict, "another frontend controller transaction is in progress");
                }
                if (externalController) {
                    return respondFailure(identity, requestId, ErrorCode::Conflict, "frontend command conflicts with current state")
                               ? ReceiveStatus::Accepted
                               : ReceiveStatus::Closed;
                }
                Connection* connection = findConnection(token);
                if (!connection || !connection->session || !connection->principal || !controller || *controller != identity) {
                    return ReceiveStatus::Closed;
                }
                const model::SessionIdentity session = *connection->session;
                const FrontendPrincipal principal = *connection->principal;
                const CommandToken commandToken{identity, token.generation, requestId, metadata.id};
                connection->outstanding.emplace(requestId, metadata.id);
                controllerTransaction = commandToken;
                connection = nullptr;
                BackendSubmitStatus status = BackendSubmitStatus::Rejected;
                try {
                    status = backend.submit(BackendInvocation{commandToken, session, principal, command});
                } catch (...) {
                    status = BackendSubmitStatus::Unavailable;
                }
                connection = findConnection(active);
                if (!connection) {
                    return ReceiveStatus::Closed;
                }
                if (status == BackendSubmitStatus::Accepted || !connection->outstanding.contains(requestId)) {
                    return ReceiveStatus::Accepted;
                }
                connection->outstanding.erase(requestId);
                if (controllerTransaction == commandToken) {
                    controllerTransaction.reset();
                }
                connection = nullptr;
                const ErrorCode code = status == BackendSubmitStatus::Unavailable
                                           ? ErrorCode::BackendUnavailable
                                           : (status == BackendSubmitStatus::CapacityExceeded ? ErrorCode::CapacityExceeded
                                                                                              : ErrorCode::LocalSubmissionFailure);
                return rejectResponse(code, "frontend controller release submission failed");
            }
            case generated::MethodId::SnapshotGet: {
                const SnapshotBarrier barrier = captureSnapshotBarrier();
                if (!respondSuccess(identity, requestId, metadata.id, Json{{"sequence", barrier.sequence.value()}})) {
                    return ReceiveStatus::Closed;
                }
                return findConnection(active) && synchronizeSnapshot(token, &barrier) ? ReceiveStatus::Accepted : ReceiveStatus::Closed;
            }
            case generated::MethodId::EventsReplay: {
                const Json& parameters = commandParameters(command);
                const auto after = parameters.find("after");
                if (after == parameters.end() || !after->is_number_unsigned()) {
                    return rejectResponse(ErrorCode::InvalidField, "events.replay requires an unsigned after sequence");
                }
                const model::FrontendSequence requested{after->get<std::uint64_t>()};
                const model::JournalReplayResult replay = journal->replayAfter(requested);
                if (replay.status == model::JournalReplayStatus::FutureSequence) {
                    return rejectResponse(ErrorCode::InvalidCommand, "events.replay cannot start after the current sequence");
                }
                Connection* connection = findConnection(active);
                if (!connection) {
                    return ReceiveStatus::Closed;
                }
                const SyncMode mode =
                    replay.status == model::JournalReplayStatus::Available &&
                            buildOccurrenceBatches(identity, *connection, replay.records).status == BatchBuildStatus::Success
                        ? SyncMode::Replay
                        : SyncMode::Snapshot;
                const SnapshotBarrier snapshotBarrier = captureSnapshotBarrier();
                connection = nullptr;
                if (!respondSuccess(identity,
                                    requestId,
                                    metadata.id,
                                    Json{{"sequence", replay.currentSequence.value()}, {"syncMode", toString(mode)}})) {
                    return ReceiveStatus::Closed;
                }
                return findConnection(active) &&
                               (mode == SyncMode::Replay ? synchronizeReplay(token, replay) : synchronizeSnapshot(token, &snapshotBarrier))
                           ? ReceiveStatus::Accepted
                           : ReceiveStatus::Closed;
            }
            case generated::MethodId::ProviderStart:
            case generated::MethodId::ProviderStop:
            case generated::MethodId::ProviderRestart: {
                ProviderLifecycleAction action = ProviderLifecycleAction::Start;
                if (metadata.id == generated::MethodId::ProviderStop) {
                    action = ProviderLifecycleAction::Stop;
                } else if (metadata.id == generated::MethodId::ProviderRestart) {
                    action = ProviderLifecycleAction::Restart;
                }
                model::ProviderState provider;
                const std::uint64_t snapshotLifecycleGeneration = providerLifecycleGeneration;
                try {
                    BackendSnapshotScope snapshotScope(*this);
                    provider = backend.snapshot().provider;
                } catch (...) {
                    if (!findConnection(active)) {
                        return ReceiveStatus::Closed;
                    }
                    return rejectResponse(ErrorCode::InternalError, "failed to dispatch frontend command");
                }
                if (!findConnection(active)) {
                    return ReceiveStatus::Closed;
                }
                if (metadata.controllerRequired && (!controller || *controller != identity)) {
                    return rejectResponse(ErrorCode::PermissionDenied, "the current controller is required");
                }
                if (providerLifecycleGeneration != snapshotLifecycleGeneration) {
                    return rejectResponse(ErrorCode::Conflict, "provider lifecycle changed during state inspection");
                }
                refreshProviderLifecycleAction(provider);
                if (providerLifecycleAction) {
                    return rejectResponse(ErrorCode::Conflict, "another provider lifecycle action is still in progress");
                }
                if (!providerLifecycleActionValid(action, provider)) {
                    return rejectResponse(ErrorCode::Conflict, "provider lifecycle action is not valid in the current state");
                }
                setProviderLifecycleAction(action);
                const std::uint64_t submittedLifecycleGeneration = providerLifecycleGeneration;
                bool backendAccepted = false;
                try {
                    backendAccepted = backend.performProviderLifecycleAction(action);
                } catch (...) {
                    backendAccepted = false;
                }
                if (!findConnection(active)) {
                    return ReceiveStatus::Closed;
                }
                if (!backendAccepted && providerLifecycleGeneration == submittedLifecycleGeneration && providerLifecycleAction &&
                    *providerLifecycleAction == action) {
                    setProviderLifecycleAction(std::nullopt);
                }
                if (!backendAccepted) {
                    return rejectResponse(ErrorCode::InternalError, "provider lifecycle action failed locally");
                }
                if (providerLifecycleGeneration != submittedLifecycleGeneration && providerLifecycleAction) {
                    return rejectResponse(ErrorCode::Conflict, "provider lifecycle changed during action dispatch");
                }
                const bool actionStillPending = providerLifecycleGeneration == submittedLifecycleGeneration && providerLifecycleAction &&
                                                *providerLifecycleAction == action;
                if (actionStillPending && (action == ProviderLifecycleAction::Start || action == ProviderLifecycleAction::Restart)) {
                    model::ProviderState transitioned = provider;
                    transitioned.desiredRunning = true;
                    transitioned.recovery = {};
                    if (transitioned.lifecycle == model::ProviderLifecycle::Stopped) {
                        if (transitioned.generation != std::numeric_limits<std::uint64_t>::max()) {
                            ++transitioned.generation;
                        }
                        transitioned.lifecycle = model::ProviderLifecycle::Starting;
                        transitioned.initialization.reset();
                    }
                    model::OccurrenceDraft occurrence{model::SourceStamp{"backend-event:0"},
                                                      model::OccurrencePayload{model::ProviderUpdatedOccurrence{std::move(transitioned)}}};
                    OccurrenceCoalescingKey key;
                    key.kind = OccurrenceEntityKind::BackendLifecycle;
                    key.entityId = "provider";
                    static_cast<void>(stageOccurrenceLocked(std::move(key), std::move(occurrence), OccurrenceFlushUrgency::Immediate));
                    materializePendingDeliveryLocked();
                    if (!findConnection(active)) {
                        return ReceiveStatus::Closed;
                    }
                }
                return acceptResponse(Json::object());
            }
            default:
                return rejectResponse(ErrorCode::InternalError, "generated frontend-native method lacks a server-core binding");
        }
    }

    ReceiveResult ServerCore::Impl::receiveDefinedCommandLocked(ConnectionIdentity identity, const generated::DefinedCommand& command) {
        const std::optional<ConnectionToken> connectionGeneration = connectionToken(identity);
        Connection* connection = connectionGeneration ? findConnection(*connectionGeneration) : nullptr;
        if (!connection) {
            return {ReceiveStatus::UnknownConnection, std::nullopt};
        }
        if (!connection->helloComplete || !connection->principal || !connection->session) {
            return protocolFailure(
                identity,
                codecFailure(ErrorCode::AuthenticationRequired, "frontend authentication must complete before commands are accepted"));
        }

        const generated::MethodId method = generated::commandMethod(command.parameters);
        const generated::MethodMetadata& metadata = methodMetadata(method);
        if (metadata.id != method || metadata.method.empty()) {
            return rejectCommand(
                identity, command.requestId, ErrorCode::UnknownMethod, "frontend command method is outside the generated authority");
        }
        if (command.requestId.empty() || command.requestId.size() > model::SessionIdentity::MaximumBytes) {
            return rejectCommand(identity, command.requestId, ErrorCode::InvalidField, "frontend command request ID is empty or oversized");
        }
        if (!methodEnabled(metadata)) {
            CodecError error = codecFailure(ErrorCode::UnknownMethod, "frontend command method is unavailable", false);
            error.requestId = command.requestId;
            return protocolFailure(identity, std::move(error));
        }
        const FrontendPrincipal principal = *connection->principal;
        const model::SessionIdentity session = *connection->session;
        const ConnectionContinuation active{*connectionGeneration, true, false};
        bool controllerRequired = metadata.controllerRequired;
        bool scopeAllowed = scopesPermit(principal, metadata);
        if (method == generated::MethodId::AccountRead) {
            const Json& parameters = commandParameters(command);
            const auto refreshToken = parameters.find("refreshToken");
            const bool privilegedRead = refreshToken != parameters.end() && refreshToken->is_boolean() && refreshToken->get<bool>();
            if (privilegedRead) {
                scopeAllowed =
                    containsScope(principal, FrontendScope::Control) && containsScope(principal, FrontendScope::AccountManagement);
                controllerRequired = true;
            }
        }
        connection = nullptr;
        const Json policyParameters = invocationPolicyParameters(command);
        const std::optional<bool> policyPermits = invocationPolicyPermits(active, principal, metadata, policyParameters);
        connection = findConnection(active);
        if (!connection || !policyPermits) {
            return {ReceiveStatus::Closed, std::nullopt};
        }
        if (!*policyPermits) {
            return rejectCommand(identity, command.requestId, ErrorCode::PermissionDenied, "frontend deployment policy denied the command");
        }
        if (!scopeAllowed) {
            return rejectCommand(identity, command.requestId, ErrorCode::PermissionDenied, "frontend principal lacks a required scope");
        }
        if (controllerRequired && (!controller || *controller != identity)) {
            return rejectCommand(identity, command.requestId, ErrorCode::PermissionDenied, "the current controller is required");
        }
        if (metadata.providerReadyRequired) {
            connection = nullptr;
            const bool providerReady = backend.providerReady();
            connection = findConnection(active);
            if (!connection) {
                return {ReceiveStatus::Closed, std::nullopt};
            }
            if (!providerReady) {
                return rejectCommand(identity, command.requestId, ErrorCode::BackendUnavailable, "the Codex App Server is not ready");
            }
        }
        if (controllerRequired && (!controller || *controller != identity)) {
            connection = nullptr;
            return rejectCommand(identity, command.requestId, ErrorCode::PermissionDenied, "the current controller is required");
        }
        if (connection->outstanding.contains(command.requestId)) {
            return rejectCommand(
                identity, command.requestId, ErrorCode::DuplicateRequestId, "requestId is already pending in this frontend session");
        }

        if (options.maxOutstandingCommandsPerConnection == 0 ||
            connection->outstanding.size() >= options.maxOutstandingCommandsPerConnection) {
            return rejectCommand(
                identity, command.requestId, ErrorCode::CapacityExceeded, "frontend outstanding command capacity exceeded");
        }

        if (metadata.frontendNative) {
            connection = nullptr;
            return {executeFrontendNative(*connectionGeneration, command, metadata), std::nullopt};
        }

        const CommandToken token{identity, connectionGeneration->generation, command.requestId, method};
        connection->outstanding.emplace(command.requestId, method);
        connection = nullptr;
        BackendSubmitStatus status = BackendSubmitStatus::Rejected;
        try {
            status = backend.submit(BackendInvocation{token, session, principal, command});
        } catch (...) {
            status = BackendSubmitStatus::Unavailable;
        }

        connection = findConnection(active);
        if (!connection) {
            return {ReceiveStatus::Closed, std::nullopt};
        }
        if (status == BackendSubmitStatus::Accepted) {
            return {ReceiveStatus::Accepted, std::nullopt};
        }

        const auto pending = connection->outstanding.find(command.requestId);
        if (pending == connection->outstanding.end()) {
            // A backend may synchronously complete from submit(). That
            // completion is authoritative even if submit() then reports a
            // non-accepted status; never emit a duplicate terminal response.
            return {ReceiveStatus::Accepted, std::nullopt};
        }
        connection->outstanding.erase(pending);
        connection = nullptr;
        ErrorCode code = ErrorCode::LocalSubmissionFailure;
        if (status == BackendSubmitStatus::Unavailable) {
            code = ErrorCode::BackendUnavailable;
        } else if (status == BackendSubmitStatus::CapacityExceeded) {
            code = ErrorCode::CapacityExceeded;
        }
        return rejectCommand(identity, command.requestId, code, "frontend command submission failed");
    }

    bool ServerCore::Impl::supportsExpandedOccurrence(const Connection& connection, ExpandedEventType type) const noexcept {
        switch (type) {
            case ExpandedEventType::ItemUpserted:
            case ExpandedEventType::ItemContentUpdated:
                return containsCapability(connection.negotiatedCapabilities, FrontendCapability::CompleteThreadItems);
            case ExpandedEventType::PendingRequestsUpdated:
                return containsCapability(connection.negotiatedCapabilities, FrontendCapability::DedicatedPendingRequests);
            default:
                return containsCapability(connection.negotiatedCapabilities, FrontendCapability::DedicatedNotificationEvents);
        }
    }

    ServerCore::Impl::SnapshotBarrier ServerCore::Impl::captureSnapshotBarrier() const {
        SnapshotBarrier barrier;
        barrier.sequence = journal->currentSequence();
        barrier.controller = controllerStateLocked();
        barrier.sessions = sessionStatesLocked();
        barrier.oldestReplayableAfter = journal->oldestReplayableAfter();
        barrier.oldestRetainedSequence = journal->oldestRetainedSequence();
        barrier.newestRetainedSequence = journal->newestRetainedSequence();
        barrier.sequenceExhausted = sequenceExhausted;
        barrier.providerLifecycleGeneration = providerLifecycleGeneration;
        return barrier;
    }

    model::CanonicalSnapshot ServerCore::Impl::applySnapshotBarrier(model::CanonicalSnapshot snapshot, const SnapshotBarrier& barrier) {
        // Session/controller ownership and the replay cursor are captured
        // before a BackendPort callback can re-enter and advance the stream.
        // The resulting Snapshot is therefore one exact canonical barrier;
        // any reentrant suffix is delivered later at a greater sequence.
        snapshot.sessions = barrier.sessions;
        snapshot.controller = barrier.controller;
        if (!snapshot.backendCursor.backendRevision || *snapshot.backendCursor.backendRevision == 0) {
            snapshot.backendCursor.backendRevision = 1;
        }
        if (providerLifecycleGeneration == barrier.providerLifecycleGeneration) {
            refreshProviderLifecycleAction(snapshot.provider);
        }
        snapshot.sequence = barrier.sequence;
        snapshot.backendCursor.currentSequence = snapshot.sequence;
        snapshot.backendCursor.oldestReplayableAfter = barrier.oldestReplayableAfter;
        snapshot.backendCursor.oldestRetainedSequence = barrier.oldestRetainedSequence;
        snapshot.backendCursor.newestRetainedSequence = barrier.newestRetainedSequence;
        snapshot.backendCursor.frontendSequenceExhausted = barrier.sequenceExhausted;
        return snapshot;
    }

    std::optional<ServerCore::Impl::FrozenSnapshotRecipient> ServerCore::Impl::freezeSnapshotRecipient(ConnectionToken token) const {
        const Connection* connection = findConnection(ConnectionContinuation{token, true, false});
        if (!connection) {
            return std::nullopt;
        }
        return FrozenSnapshotRecipient{token, projectionContext(token.identity, *connection), connection->negotiatedCapabilities};
    }

    bool ServerCore::Impl::enqueueFrozenSnapshot(const FrozenSnapshotRecipient& recipient,
                                                 const model::CanonicalSnapshot& canonical,
                                                 bool deferUntilSnapshotBarrier) {
        try {
            if (!findConnection(ConnectionContinuation{recipient.token, true, false})) {
                return false;
            }

            const model::ProjectionOutcome<model::CanonicalSnapshot> projected =
                projection.projectSnapshot(canonical, recipient.projection);
            if (!projected) {
                return false;
            }
            const model::CanonicalSnapshot& snapshot = projected.value();

            const model::ModelResult<Snapshot> encoded = model::encodeProjectedSnapshot(snapshot, recipient.negotiatedCapabilities);
            return encoded && (deferUntilSnapshotBarrier ? enqueueDeferredSnapshot(recipient.token.identity, ServerMessage{encoded.value()})
                                                         : enqueue(recipient.token.identity, ServerMessage{encoded.value()}));
        } catch (...) {
            return false;
        }
    }

    std::optional<model::FrontendSequence> ServerCore::Impl::enqueueSnapshot(ConnectionToken token,
                                                                             const SnapshotBarrier* suppliedBarrier) {
        const ConnectionContinuation active{token, true, false};
        const std::optional<FrozenSnapshotRecipient> recipient = freezeSnapshotRecipient(token);
        if (!recipient) {
            return std::nullopt;
        }
        const SnapshotBarrier captured = suppliedBarrier ? SnapshotBarrier{} : captureSnapshotBarrier();
        const SnapshotBarrier& barrier = suppliedBarrier ? *suppliedBarrier : captured;
        model::CanonicalSnapshot canonical;
        try {
            BackendSnapshotScope snapshotScope(*this);
            canonical = backend.snapshot();
        } catch (...) {
            return std::nullopt;
        }
        if (!findConnection(active)) {
            return std::nullopt;
        }
        canonical = applySnapshotBarrier(std::move(canonical), barrier);
        return enqueueFrozenSnapshot(*recipient, canonical) ? std::optional<model::FrontendSequence>{barrier.sequence} : std::nullopt;
    }

    ServerCore::Impl::BatchBuildResult ServerCore::Impl::buildOccurrenceBatches(
        ConnectionIdentity identity, const Connection& connection, std::span<const model::CanonicalOccurrence> occurrences) const {
        BatchBuildResult result;
        if (options.maxEventsPerBatch == 0 || options.maxBatchBytes == 0) {
            return result;
        }
        if (occurrences.empty()) {
            result.status = BatchBuildStatus::Success;
            return result;
        }

        try {
            std::vector<FrontendEvent> pending;
            std::optional<bool> pendingExpanded;
            auto makeBatch = [](std::vector<FrontendEvent> events) {
                return EventBatch{events.front().sequence, events.back().sequence, std::move(events), Json::object()};
            };
            auto encodedSize = [](const EventBatch& batch) -> std::optional<std::size_t> {
                const auto serialized = Codec::serializeServer(ServerMessage{batch});
                return serialized ? std::optional<std::size_t>{serialized.value().size()} : std::nullopt;
            };
            auto flushPending = [&] {
                if (!pending.empty()) {
                    result.batches.push_back(makeBatch(std::move(pending)));
                    pending.clear();
                    pendingExpanded.reset();
                }
            };

            std::optional<model::FrontendSequence> previousSequence;
            for (std::size_t begin = 0; begin < occurrences.size();) {
                const model::FrontendSequence sequence = occurrences[begin].identity().sequence;
                if (previousSequence && sequence < *previousSequence) {
                    result.batches.clear();
                    return result;
                }
                std::size_t end = begin + 1;
                while (end < occurrences.size() && occurrences[end].identity().sequence == sequence) {
                    ++end;
                }
                previousSequence = sequence;

                std::optional<model::CanonicalOccurrence> canonicalGroup;
                if (end - begin == 1) {
                    canonicalGroup = occurrences[begin];
                } else {
                    model::OccurrenceResult<model::CanonicalOccurrence> merged =
                        model::mergeOccurrenceGroup(occurrences.subspan(begin, end - begin));
                    if (!merged) {
                        result.batches.clear();
                        return result;
                    }
                    canonicalGroup = std::move(merged).value();
                }

                const model::ProjectionOutcome<std::optional<model::CanonicalOccurrence>> outcome =
                    projection.projectOccurrence(*canonicalGroup, projectionContext(identity, connection));
                if (!outcome) {
                    result.batches.clear();
                    return result;
                }
                const std::optional<model::CanonicalOccurrence>& projected = outcome.value();
                if (!projected) {
                    begin = end;
                    continue;
                }

                const model::LegacyCompatibilityKind legacyKind = projected->legacyCompatibility().kind;
                if ((legacyKind == model::LegacyCompatibilityKind::LegacyItem &&
                     containsCapability(connection.negotiatedCapabilities, FrontendCapability::CompleteThreadItems)) ||
                    (legacyKind == model::LegacyCompatibilityKind::LegacyPendingRequest &&
                     containsCapability(connection.negotiatedCapabilities, FrontendCapability::DedicatedPendingRequests))) {
                    result.status = BatchBuildStatus::SnapshotRequired;
                    result.batches.clear();
                    return result;
                }

                const bool useExpanded = !projected->expandedPayloads().empty() &&
                                         std::all_of(projected->expandedPayloads().begin(),
                                                     projected->expandedPayloads().end(),
                                                     [&](const model::OccurrencePayload& payload) {
                                                         return supportsExpandedOccurrence(connection, model::occurrenceType(payload));
                                                     });
                if (legacyKind == model::LegacyCompatibilityKind::DirectExpanded && !useExpanded) {
                    // This family has no legacy representation.  Keep the
                    // canonical journal occurrence, but do not expose an
                    // expanded-only event to a connection that did not
                    // negotiate its representation capability.
                    begin = end;
                    continue;
                }
                std::vector<FrontendEvent> groupEvents;
                if (useExpanded) {
                    const model::OccurrenceResult<std::vector<ExpandedFrontendEvent>> encoded = model::encodeExpandedOccurrence(*projected);
                    if (!encoded) {
                        result.batches.clear();
                        return result;
                    }
                    groupEvents.reserve(encoded.value().size());
                    for (const ExpandedFrontendEvent& event : encoded.value()) {
                        groupEvents.push_back(
                            FrontendEvent{event.sequence, std::string(toString(event.type)), event.data, event.extensions});
                    }
                } else {
                    const model::OccurrenceResult<FrontendEvent> encoded = model::encodeLegacyOccurrence(*projected);
                    if (!encoded) {
                        result.batches.clear();
                        return result;
                    }
                    groupEvents.push_back(encoded.value());
                }
                if (groupEvents.empty()) {
                    result.batches.clear();
                    return result;
                }

                // Legacy and expanded arrays are different schema branches.
                // A capability mix can switch representation by family, so a
                // wire batch never straddles that switch.
                if (pendingExpanded && *pendingExpanded != useExpanded) {
                    flushPending();
                }

                std::vector<FrontendEvent> candidate = pending;
                candidate.insert(candidate.end(), groupEvents.begin(), groupEvents.end());
                EventBatch candidateBatch = makeBatch(std::move(candidate));
                const std::optional<std::size_t> candidateBytes = encodedSize(candidateBatch);
                if (!candidateBytes) {
                    result.batches.clear();
                    return result;
                }
                if (candidateBatch.events.size() <= options.maxEventsPerBatch && *candidateBytes <= options.maxBatchBytes) {
                    pending = std::move(candidateBatch.events);
                    pendingExpanded = useExpanded;
                    begin = end;
                    continue;
                }

                flushPending();
                EventBatch groupBatch = makeBatch(std::move(groupEvents));
                const std::optional<std::size_t> groupBytes = encodedSize(groupBatch);
                if (!groupBytes) {
                    result.batches.clear();
                    return result;
                }
                if (groupBatch.events.size() > options.maxEventsPerBatch || *groupBytes > options.maxBatchBytes) {
                    result.status = BatchBuildStatus::SnapshotRequired;
                    result.batches.clear();
                    return result;
                }
                pending = std::move(groupBatch.events);
                pendingExpanded = useExpanded;
                begin = end;
            }

            flushPending();
            result.status = BatchBuildStatus::Success;
            return result;
        } catch (...) {
            result.batches.clear();
            return result;
        }
    }

    bool ServerCore::Impl::enqueueBuiltBatches(ConnectionIdentity identity, std::span<const EventBatch> batches) {
        for (const EventBatch& batch : batches) {
            if (!enqueue(identity, ServerMessage{batch})) {
                return false;
            }
        }
        return true;
    }

    bool ServerCore::Impl::emitOccurrencesOrSnapshot(ConnectionToken token, std::span<const model::CanonicalOccurrence> occurrences) {
        Connection* connection = findConnection(ConnectionContinuation{token, true, false});
        if (!connection) {
            return false;
        }
        const BatchBuildResult built = buildOccurrenceBatches(token.identity, *connection, occurrences);
        connection = nullptr;
        if (built.status == BatchBuildStatus::SnapshotRequired) {
            return enqueueSnapshot(token).has_value();
        }
        return built.status == BatchBuildStatus::Success && enqueueBuiltBatches(token.identity, built.batches);
    }

    void ServerCore::Impl::handleSequenceExhaustion() noexcept {
        sequenceExhausted = true;
        dirtyOccurrences.clear();
        dirtySnapshotRequired = false;
        nextDirtyInsertionOrder = 0;
        pendingDelivery.clear();
        pendingDeliveryGroups = 0;
        pendingDeliverySnapshotMode = PendingSnapshotSequenceMode::None;
        static_cast<void>(journal->invalidateReplay());

        try {
            const SnapshotBarrier barrier = captureSnapshotBarrier();
            std::vector<FrozenSnapshotRecipient> recipients;
            recipients.reserve(connections.size());
            for (const auto& [identity, connection] : connections) {
                if (connection.helloComplete && !connection.closing) {
                    const ConnectionToken token{identity, connection.generation};
                    if (std::optional<FrozenSnapshotRecipient> recipient = freezeSnapshotRecipient(token)) {
                        recipients.push_back(std::move(*recipient));
                    }
                }
            }
            std::optional<model::CanonicalSnapshot> snapshot;
            try {
                BackendSnapshotScope snapshotScope(*this);
                model::CanonicalSnapshot canonical = backend.snapshot();
                if (open && !terminallyClosed) {
                    snapshot = applySnapshotBarrier(std::move(canonical), barrier);
                }
            } catch (...) {
            }
            if (!open || terminallyClosed) {
                return;
            }
            for (const FrozenSnapshotRecipient& recipient : recipients) {
                if (!findConnection(ConnectionContinuation{recipient.token, true, false})) {
                    continue;
                }
                ProtocolErrorMessage error;
                error.code = ErrorCode::SequenceOverflow;
                error.message = "frontend event sequence is exhausted";
                error.supportedVersions.assign(SupportedProtocolVersions.begin(), SupportedProtocolVersions.end());
                error.closeConnection = false;
                static_cast<void>(enqueue(recipient.token.identity, ServerMessage{std::move(error)}));
                if (snapshot && findConnection(ConnectionContinuation{recipient.token, true, false})) {
                    static_cast<void>(enqueueFrozenSnapshot(recipient, *snapshot));
                }
            }
            if (open && !terminallyClosed) {
                requestFlush();
            }
        } catch (...) {
            // Sequence exhaustion remains observable through snapshot cursor
            // metadata on a later synchronization even if this best-effort
            // broadcast cannot allocate.
        }
    }

    PublishResult ServerCore::Impl::appendAndStageDelivery(model::OccurrenceDraft occurrence, bool scheduleDelivery) noexcept {
        PublishResult result;
        result.occurrenceCount = std::max<std::size_t>(1, occurrence.expandedPayloads.size());
        if (!open || !validOccurrenceDraft(occurrence)) {
            result.error = !validOccurrenceDraft(occurrence) ? ErrorCode::InvalidField : ErrorCode::BackendUnavailable;
            return result;
        }
        if (sequenceExhausted) {
            result.error = ErrorCode::SequenceOverflow;
            return result;
        }

        try {
            model::JournalAppendResult appended = journal->appendGroup(std::move(occurrence));
            if (appended.status == model::JournalAppendStatus::SequenceExhausted) {
                result.error = ErrorCode::SequenceOverflow;
                handleSequenceExhaustion();
                return result;
            }
            if (appended.status != model::JournalAppendStatus::Appended && appended.status != model::JournalAppendStatus::NotRetained) {
                result.error =
                    appended.status == model::JournalAppendStatus::EncodingFailure ? ErrorCode::InternalError : ErrorCode::InvalidField;
                return result;
            }
            if (!appended.sequence) {
                result.error = ErrorCode::InternalError;
                return result;
            }

            result.accepted = true;
            result.sequence = *appended.sequence;
            if (dirtySnapshotRequired) {
                requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence, &result);
            } else if (pendingDeliverySnapshotMode != PendingSnapshotSequenceMode::None) {
                requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::ReuseCommittedSequence, &result);
            } else if (options.maxPendingDeliveryGroups == 0 || pendingDeliveryGroups >= options.maxPendingDeliveryGroups) {
                requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::ReuseCommittedSequence, &result);
            } else {
                pendingDelivery.insert(pendingDelivery.end(),
                                       std::make_move_iterator(appended.records.begin()),
                                       std::make_move_iterator(appended.records.end()));
                ++pendingDeliveryGroups;
                result.deliveryMode = PublishDeliveryMode::Occurrences;
            }
            if (scheduleDelivery) {
                requestFlush();
            }
            return result;
        } catch (...) {
            // The journal may already have advanced. Preserve that monotonic
            // state and replace any uncertain live-delivery suffix with a
            // projected snapshot at the next delivery turn.
            if (result.accepted) {
                requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::ReuseCommittedSequence, &result);
                result.error.reset();
            } else {
                requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence);
                result.error = ErrorCode::InternalError;
            }
            if (scheduleDelivery) {
                requestFlush();
            }
            return result;
        }
    }

    void ServerCore::Impl::drainDirtyOccurrences() {
        if (dirtyOccurrences.empty() && !dirtySnapshotRequired) {
            return;
        }

        if (dirtySnapshotRequired || sequenceExhausted) {
            dirtyOccurrences.clear();
            dirtySnapshotRequired = false;
            nextDirtyInsertionOrder = 0;
            requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence);
            return;
        }

        std::vector<StoredPendingOccurrence*> ordered;
        ordered.reserve(dirtyOccurrences.size());
        for (auto& [key, stored] : dirtyOccurrences) {
            static_cast<void>(key);
            ordered.push_back(&stored);
        }
        std::sort(ordered.begin(), ordered.end(), [](const StoredPendingOccurrence* left, const StoredPendingOccurrence* right) {
            return left->insertionOrder < right->insertionOrder;
        });

        std::vector<model::OccurrenceDraft> occurrences;
        occurrences.reserve(ordered.size());
        for (StoredPendingOccurrence* stored : ordered) {
            occurrences.push_back(std::move(stored->occurrence));
        }
        dirtyOccurrences.clear();
        nextDirtyInsertionOrder = 0;
        for (model::OccurrenceDraft& occurrence : occurrences) {
            const PublishResult appended = appendAndStageDelivery(std::move(occurrence), false);
            if (!appended.accepted) {
                if (appended.error == ErrorCode::SequenceOverflow) {
                    return;
                }
                requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence);
                break;
            }
        }
    }

    void ServerCore::Impl::broadcastPendingDelivery() {
        if (pendingDelivery.empty() && pendingDeliverySnapshotMode == PendingSnapshotSequenceMode::None) {
            return;
        }

        std::vector<ConnectionToken> recipientTokens;
        recipientTokens.reserve(connections.size());
        for (const auto& [identity, connection] : connections) {
            if (connection.helloComplete && !connection.closing) {
                recipientTokens.push_back(ConnectionToken{identity, connection.generation});
            }
        }

        if (pendingDeliverySnapshotMode != PendingSnapshotSequenceMode::None) {
            pendingDelivery.clear();
            pendingDeliveryGroups = 0;
            const PendingSnapshotSequenceMode sequenceMode = pendingDeliverySnapshotMode;
            bool replayInvalidated = false;
            switch (sequenceMode) {
                case PendingSnapshotSequenceMode::ReuseCommittedSequence:
                    replayInvalidated = journal->invalidateReplayAtCurrentSequence();
                    break;
                case PendingSnapshotSequenceMode::AdvanceSequence:
                    replayInvalidated = journal->invalidateReplay();
                    break;
                case PendingSnapshotSequenceMode::None:
                    return;
            }
            // The pending suffix is now represented by this exact journal
            // barrier. Reentrant work belongs to a later suffix and may select
            // its own (possibly stronger) mode.
            pendingDeliverySnapshotMode = PendingSnapshotSequenceMode::None;
            if (!replayInvalidated) {
                sequenceExhausted = true;
            }
            const SnapshotBarrier barrier = captureSnapshotBarrier();
            std::vector<FrozenSnapshotRecipient> recipients;
            recipients.reserve(recipientTokens.size());
            for (const ConnectionToken token : recipientTokens) {
                if (std::optional<FrozenSnapshotRecipient> recipient = freezeSnapshotRecipient(token)) {
                    recipients.push_back(std::move(*recipient));
                }
            }
            model::CanonicalSnapshot snapshot;
            {
                BackendSnapshotScope snapshotScope(*this);
                snapshot = backend.snapshot();
            }
            if (!open || terminallyClosed) {
                return;
            }
            snapshot = applySnapshotBarrier(std::move(snapshot), barrier);
            for (const FrozenSnapshotRecipient& recipient : recipients) {
                if (findConnection(ConnectionContinuation{recipient.token, true, false}) && !enqueueFrozenSnapshot(recipient, snapshot)) {
                    closeNow(recipient.token.identity,
                             ConnectionClose{"frontend snapshot fallback projection or queueing failed", ErrorCode::InternalError, false});
                }
            }
            return;
        }

        std::vector<model::CanonicalOccurrence> occurrences = std::move(pendingDelivery);
        pendingDelivery.clear();
        pendingDeliveryGroups = 0;

        struct PlannedDelivery {
            ConnectionToken token;
            BatchBuildResult built;
            std::optional<FrozenSnapshotRecipient> snapshotRecipient;
        };
        std::vector<PlannedDelivery> plans;
        plans.reserve(recipientTokens.size());
        bool snapshotRequired = false;
        for (const ConnectionToken token : recipientTokens) {
            Connection* connection = findConnection(ConnectionContinuation{token, true, false});
            if (!connection) {
                continue;
            }
            BatchBuildResult built = buildOccurrenceBatches(token.identity, *connection, occurrences);
            connection = nullptr;
            std::optional<FrozenSnapshotRecipient> recipient;
            if (built.status == BatchBuildStatus::SnapshotRequired) {
                recipient = freezeSnapshotRecipient(token);
                snapshotRequired = snapshotRequired || recipient.has_value();
            }
            plans.push_back(PlannedDelivery{token, std::move(built), std::move(recipient)});
        }

        std::optional<model::CanonicalSnapshot> fallbackSnapshot;
        if (snapshotRequired) {
            const SnapshotBarrier barrier = captureSnapshotBarrier();
            BackendSnapshotScope snapshotScope(*this);
            model::CanonicalSnapshot canonical = backend.snapshot();
            if (!open || terminallyClosed) {
                return;
            }
            fallbackSnapshot = applySnapshotBarrier(std::move(canonical), barrier);
        }

        for (const PlannedDelivery& plan : plans) {
            if (!findConnection(ConnectionContinuation{plan.token, true, false})) {
                continue;
            }
            bool delivered = false;
            if (plan.built.status == BatchBuildStatus::Success) {
                delivered = enqueueBuiltBatches(plan.token.identity, plan.built.batches);
            } else if (plan.built.status == BatchBuildStatus::SnapshotRequired && plan.snapshotRecipient && fallbackSnapshot) {
                delivered = enqueueFrozenSnapshot(*plan.snapshotRecipient, *fallbackSnapshot);
            }
            if (!delivered && findConnection(ConnectionContinuation{plan.token, true, false})) {
                closeNow(plan.token.identity,
                         ConnectionClose{"frontend occurrence projection or queueing failed", ErrorCode::InternalError, false});
            }
        }
    }

    void ServerCore::Impl::materializePendingDeliveryLocked() {
        drainDirtyOccurrences();
        broadcastPendingDelivery();
    }

    bool ServerCore::Impl::synchronizeSnapshot(ConnectionToken token, const SnapshotBarrier* suppliedBarrier) {
        const std::optional<model::FrontendSequence> sequence = enqueueSnapshot(token, suppliedBarrier);
        if (!sequence) {
            return false;
        }
        if (!findConnection(ConnectionContinuation{token, true, false})) {
            return false;
        }
        const bool completed = enqueue(token.identity, ServerMessage{SyncComplete{sequence->protocolValue(), Json::object()}});
        if (completed) {
            requestFlush();
        }
        return completed;
    }

    bool ServerCore::Impl::synchronizeReplay(ConnectionToken token, const model::JournalReplayResult& replay) {
        if (replay.status != model::JournalReplayStatus::Available) {
            return false;
        }
        if (!emitOccurrencesOrSnapshot(token, replay.records)) {
            return false;
        }
        if (!findConnection(ConnectionContinuation{token, true, false})) {
            return false;
        }
        const bool completed = enqueue(token.identity, ServerMessage{SyncComplete{replay.currentSequence.protocolValue(), Json::object()}});
        if (completed) {
            requestFlush();
        }
        return completed;
    }

    bool ServerCore::Impl::complete(BackendCompletion completion) {
        const ConnectionToken token{completion.token.connection, completion.token.connectionGeneration};
        Connection* connection = findConnection(ConnectionContinuation{token, true, false});
        if (!connection) {
            return false;
        }
        const auto pending = connection->outstanding.find(completion.token.requestId);
        if (pending == connection->outstanding.end() || pending->second != completion.token.method) {
            return false;
        }
        const bool controllerCompletion = completion.token.method == generated::MethodId::ControllerAcquire ||
                                          completion.token.method == generated::MethodId::ControllerRelease;
        if (controllerCompletion && (!controllerTransaction || *controllerTransaction != completion.token)) {
            return false;
        }
        const auto clearControllerTransaction = [&] {
            if (controllerCompletion && controllerTransaction && *controllerTransaction == completion.token) {
                controllerTransaction.reset();
            }
        };

        if (const auto* failure = std::get_if<BackendCommandFailure>(&completion.value)) {
            connection->outstanding.erase(pending);
            clearControllerTransaction();
            connection = nullptr;
            return respondFailure(
                completion.token.connection, std::move(completion.token.requestId), failure->code, failure->message, failure->details);
        }

        BackendCommandSuccess success = std::get<BackendCommandSuccess>(std::move(completion.value));
        if (generated::commandMethod(success.result) != completion.token.method) {
            connection->outstanding.erase(pending);
            clearControllerTransaction();
            connection = nullptr;
            return respondFailure(completion.token.connection,
                                  std::move(completion.token.requestId),
                                  ErrorCode::TypedDecodingFailure,
                                  "backend completion result method does not match its command");
        }
        const auto encoded = Codec::encodeDefinedResult(success.result);
        if (!encoded) {
            connection->outstanding.erase(pending);
            clearControllerTransaction();
            connection = nullptr;
            return respondFailure(completion.token.connection,
                                  std::move(completion.token.requestId),
                                  ErrorCode::TypedDecodingFailure,
                                  "backend completion violates the generated result schema");
        }
        clearControllerTransaction();

        // Controller ownership is one transaction spanning BackendCore and
        // ServerCore.  No canonical owner changes until the matching backend
        // command has completed successfully for this exact generation.
        if (completion.token.method == generated::MethodId::ControllerAcquire) {
            if (externalController || (controller && *controller != completion.token.connection)) {
                connection->outstanding.erase(pending);
                connection = nullptr;
                return respondFailure(completion.token.connection,
                                      std::move(completion.token.requestId),
                                      ErrorCode::Conflict,
                                      "frontend controller changed during acquisition");
            }
            externalController.reset();
            const bool changed = !controller;
            controller = completion.token.connection;
            if (changed) {
                stageControllerChangedLocked();
                materializePendingDeliveryLocked();
            }
        } else if (completion.token.method == generated::MethodId::ControllerRelease) {
            if (!controller && externalController) {
                connection->outstanding.erase(completion.token.requestId);
                connection = nullptr;
                const bool queued = enqueue(completion.token.connection,
                                            ServerMessage{Response::success(std::move(completion.token.requestId), encoded.value())});
                if (queued) {
                    requestFlush();
                }
                return queued;
            }
            if (!controller && !externalController) {
                connection->outstanding.erase(completion.token.requestId);
                connection = nullptr;
                const bool queued = enqueue(completion.token.connection,
                                            ServerMessage{Response::success(std::move(completion.token.requestId), encoded.value())});
                if (queued) {
                    requestFlush();
                }
                return queued;
            }
            if (!controller || *controller != completion.token.connection) {
                connection->outstanding.erase(pending);
                connection = nullptr;
                return respondFailure(completion.token.connection,
                                      std::move(completion.token.requestId),
                                      ErrorCode::Conflict,
                                      "frontend controller changed during release");
            }
            controller.reset();
            stageControllerChangedLocked();
            materializePendingDeliveryLocked();
        }
        connection = findConnection(ConnectionContinuation{token, true, false});
        if (!connection) {
            return false;
        }
        connection->outstanding.erase(completion.token.requestId);
        connection = nullptr;
        const bool queued =
            enqueue(completion.token.connection, ServerMessage{Response::success(std::move(completion.token.requestId), encoded.value())});
        if (queued) {
            requestFlush();
        }
        return queued;
    }

    std::vector<model::SessionState> ServerCore::Impl::sessionStatesLocked() const {
        std::vector<model::SessionState> sessions;
        sessions.reserve(connections.size() + externalSessions.size());
        for (const auto& [identity, connection] : connections) {
            if (!connection.helloComplete || !connection.session || connection.closing) {
                continue;
            }
            model::SessionState session(*connection.session);
            session.role = controller && *controller == identity ? SessionRole::Controller : SessionRole::Observer;
            sessions.push_back(std::move(session));
        }
        for (const auto& [identity, external] : externalSessions) {
            static_cast<void>(identity);
            sessions.push_back(external);
        }
        return sessions;
    }

    model::ControllerState ServerCore::Impl::controllerStateLocked() const {
        model::ControllerState state;
        if (!controller) {
            if (externalController) {
                state.session = externalController;
            }
            return state;
        }
        const Connection* connection = findConnection(*controller);
        if (!connection || !connection->session) {
            return state;
        }
        state.session = *connection->session;
        return state;
    }

    void ServerCore::Impl::stageSessionChangedLocked(model::SessionState changedSession, bool connected) noexcept {
        try {
            const model::SessionIdentity changedId = changedSession.id;
            model::SessionsUpdatedOccurrence update{sessionStatesLocked()};
            update.changedSession = std::move(changedSession);
            update.connected = connected;
            model::OccurrenceDraft occurrence{model::SourceStamp{"server-core"}, model::OccurrencePayload{std::move(update)}};
            occurrence.sessionId = changedId;
            occurrence.legacyCompatibility.changedSessionId = changedId;
            occurrence.legacyCompatibility.connected = connected;
            OccurrenceCoalescingKey key;
            key.kind = OccurrenceEntityKind::Session;
            key.entityId = changedId.value();
            static_cast<void>(stageOccurrenceLocked(std::move(key), std::move(occurrence), OccurrenceFlushUrgency::Immediate));
        } catch (...) {
            dirtySnapshotRequired = true;
            requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence);
            try {
                requestFlush();
            } catch (...) {
            }
        }
    }

    void ServerCore::Impl::stageControllerChangedLocked() noexcept {
        try {
            model::ControllerState state = controllerStateLocked();
            model::OccurrenceDraft occurrence{model::SourceStamp{"server-core"},
                                              model::OccurrencePayload{model::ControllerUpdatedOccurrence{state}}};
            occurrence.sessionId = state.session;
            occurrence.controllerId = state.controller;
            OccurrenceCoalescingKey key;
            key.kind = OccurrenceEntityKind::Controller;
            key.entityId = "controller";
            static_cast<void>(stageOccurrenceLocked(std::move(key), std::move(occurrence), OccurrenceFlushUrgency::Immediate));
        } catch (...) {
            dirtySnapshotRequired = true;
            requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence);
            try {
                requestFlush();
            } catch (...) {
            }
        }
    }

    OccurrenceStageResult ServerCore::Impl::stageOccurrenceLocked(OccurrenceCoalescingKey key,
                                                                  model::OccurrenceDraft occurrence,
                                                                  OccurrenceFlushUrgency urgency) noexcept {
        const bool immediate = urgency == OccurrenceFlushUrgency::Immediate;
        try {
            if (!open || !key.valid() || !validOccurrenceDraft(occurrence)) {
                return {OccurrenceStageStatus::InvalidOccurrence, false, immediate};
            }

            const bool scheduleRequired = dirtyOccurrences.empty() && !dirtySnapshotRequired;
            const auto existing = dirtyOccurrences.find(key);
            if (existing != dirtyOccurrences.end()) {
                existing->second.occurrence = std::move(occurrence);
                // A first item upsert can still be dirty when its content and
                // terminal replacement arrive. Keep that upsert ahead of the
                // dependent content update; the replacement already contains
                // the final accumulated item state.
                if (immediate && key.kind != OccurrenceEntityKind::Item) {
                    if (nextDirtyInsertionOrder == std::numeric_limits<std::uint64_t>::max()) {
                        dirtySnapshotRequired = true;
                        requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence);
                        requestFlush();
                        return {OccurrenceStageStatus::SnapshotRequired, scheduleRequired, true};
                    }
                    existing->second.insertionOrder = nextDirtyInsertionOrder++;
                }
                requestFlush();
                return {OccurrenceStageStatus::Accepted, scheduleRequired, immediate};
            }

            if (options.maxDirtyEntities == 0 || dirtyOccurrences.size() >= options.maxDirtyEntities ||
                nextDirtyInsertionOrder == std::numeric_limits<std::uint64_t>::max()) {
                dirtySnapshotRequired = true;
                requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence);
                requestFlush();
                return {OccurrenceStageStatus::SnapshotRequired, scheduleRequired, true};
            }

            dirtyOccurrences.emplace(std::move(key), StoredPendingOccurrence{std::move(occurrence), nextDirtyInsertionOrder++});
            requestFlush();
            return {OccurrenceStageStatus::Accepted, scheduleRequired, immediate};
        } catch (...) {
            dirtySnapshotRequired = true;
            requirePendingDeliverySnapshot(PendingSnapshotSequenceMode::AdvanceSequence);
            try {
                requestFlush();
            } catch (...) {
            }
            return {OccurrenceStageStatus::AllocationFailure, false, true};
        }
    }

    ServerCore::ServerCore(BackendPort& backend, ServerCoreOptions options)
        : impl(std::make_shared<Impl>(backend, std::move(options))) {
        backend.bind(*this);
    }

    ServerCore::~ServerCore() {
        close();
        impl->backend.unbind(*this);
    }

    void ServerCore::start() {
        Impl::DispatchScope dispatch(*impl);
        if (!impl->terminallyClosed) {
            impl->open = true;
        }
    }

    void ServerCore::close(std::string reason) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            impl->terminallyClosed = true;
            impl->open = false;
            impl->scheduled = false;
            impl->scheduledFlushGeneration = 0;
            impl->flushRequested = false;
            impl->inlineScheduledFlush.reset();
            impl->dirtyOccurrences.clear();
            impl->nextDirtyInsertionOrder = 0;
            impl->dirtySnapshotRequired = false;
            impl->pendingDelivery.clear();
            impl->pendingDeliveryGroups = 0;
            impl->pendingDeliverySnapshotMode = Impl::PendingSnapshotSequenceMode::None;
            impl->externalSessions.clear();
            impl->externalController.reset();

            while (!impl->connections.empty()) {
                const ConnectionIdentity identity = impl->connections.begin()->first;
                try {
                    impl->closeNow(identity, ConnectionClose{reason, std::nullopt, true});
                } catch (...) {
                    // Preserve session/controller cleanup even when copying a
                    // caller-provided close reason cannot allocate.
                    impl->closeNow(identity, ConnectionClose{});
                }
            }

            while (!impl->authenticationFailures.empty()) {
                TimerCancellation cancellation;
                {
                    const auto window = impl->authenticationFailures.begin();
                    cancellation = std::move(window->second.expirationTimer);
                    impl->authenticationFailures.erase(window);
                }
                if (cancellation) {
                    try {
                        cancellation();
                    } catch (...) {
                    }
                }
            }

            // Reentrant close/cancellation callbacks cannot leave work armed
            // on a terminally closed core.
            impl->scheduled = false;
            impl->scheduledFlushGeneration = 0;
            impl->flushRequested = false;
            impl->inlineScheduledFlush.reset();
            impl->dirtyOccurrences.clear();
            impl->nextDirtyInsertionOrder = 0;
            impl->dirtySnapshotRequired = false;
            impl->pendingDelivery.clear();
            impl->pendingDeliveryGroups = 0;
            impl->pendingDeliverySnapshotMode = Impl::PendingSnapshotSequenceMode::None;
        } catch (...) {
            // close() is terminal even at an allocation/callback exception
            // boundary; destructors must never propagate.
            try {
                impl->open = false;
                impl->terminallyClosed = true;
            } catch (...) {
            }
        }
    }

    std::optional<ConnectionIdentity> ServerCore::openConnection(FrontendPeerContext peer, ConnectionCallbacks callbacks) {
        Impl::DispatchScope dispatch(*impl);
        const std::uint64_t current = impl->now();
        if (!impl->open || impl->connections.size() >= impl->options.maxConnections) {
            return std::nullopt;
        }
        const std::size_t unauthenticated =
            static_cast<std::size_t>(std::count_if(impl->connections.begin(), impl->connections.end(), [](const auto& entry) {
                return !entry.second.helloComplete;
            }));
        if (unauthenticated >= impl->options.maxUnauthenticatedConnections ||
            impl->nextConnectionIdentity == std::numeric_limits<std::uint64_t>::max() ||
            impl->nextConnectionGeneration == std::numeric_limits<std::uint64_t>::max()) {
            return std::nullopt;
        }

        const ConnectionIdentity identity{impl->nextConnectionIdentity++};
        const std::uint64_t generation = impl->nextConnectionGeneration++;
        Impl::Connection connection;
        connection.peer = std::move(peer);
        connection.callbacks = std::move(callbacks);
        connection.generation = generation;
        connection.lastInboundRateRefillMs = current;
        connection.inboundRateTokens = saturatingMultiply(impl->options.maxInboundBurst, 1000);
        impl->connections.emplace(identity, std::move(connection));

        if (impl->options.timerScheduler && impl->options.handshakeTimeoutMs > 0) {
            const std::weak_ptr<Impl> weak = impl;
            const Impl::ConnectionToken token{identity, generation};
            const Impl::ConnectionContinuation awaitingHello{token, false, false};
            try {
                TimerCancellation cancellation =
                    impl->options.timerScheduler(impl->options.handshakeTimeoutMs, [weak, identity, generation] {
                        if (const std::shared_ptr<Impl> self = weak.lock()) {
                            Impl::DispatchScope timerDispatch(*self);
                            const Impl::ConnectionToken token{identity, generation};
                            if (self->findConnection(Impl::ConnectionContinuation{token, false, false})) {
                                self->closeNow(identity,
                                               ConnectionClose{"frontend Hello timeout", ErrorCode::AuthenticationRequired, false});
                            }
                        }
                    });
                Impl::Connection* inserted = impl->findConnection(awaitingHello);
                if (inserted) {
                    inserted->handshakeTimer = std::move(cancellation);
                } else if (cancellation) {
                    cancellation();
                }
            } catch (...) {
                if (impl->findConnection(awaitingHello)) {
                    impl->closeNow(identity,
                                   ConnectionClose{"frontend handshake timer scheduling failed", ErrorCode::InternalError, false});
                }
                const Impl::Connection* remaining = impl->findConnection(token);
                return remaining && !remaining->closing ? std::optional<ConnectionIdentity>{identity} : std::nullopt;
            }
        }
        const Impl::Connection* opened = impl->findConnection(Impl::ConnectionToken{identity, generation});
        if (!opened || opened->closing) {
            return std::nullopt;
        }
        return identity;
    }

    bool ServerCore::updatePeerContext(ConnectionIdentity identity, FrontendPeerContext peer) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            Impl::Connection* connection = impl->findConnection(identity);
            if (!connection || connection->helloAttempted || connection->helloComplete || connection->closing) {
                return false;
            }
            connection->peer = std::move(peer);
            return true;
        } catch (...) {
            return false;
        }
    }

    void ServerCore::closeConnection(ConnectionIdentity identity, std::string reason) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            impl->closeNow(identity, ConnectionClose{std::move(reason), std::nullopt, true});
        } catch (...) {
            try {
                Impl::DispatchScope dispatch(*impl);
                impl->closeNow(identity, ConnectionClose{});
            } catch (...) {
            }
        }
    }

    ReceiveResult ServerCore::receive(ConnectionIdentity identity, const ClientMessage& message) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            const auto encoded = Codec::serializeClient(message);
            if (!encoded) {
                return impl->protocolFailure(identity, encoded.error());
            }
            ReceiveResult failure;
            if (!impl->beginInbound(identity, encoded.value().size(), failure)) {
                return failure;
            }
            return impl->receiveClientLocked(identity, message);
        } catch (...) {
            return containedReceiveFailure();
        }
    }

    ReceiveResult ServerCore::receive(ConnectionIdentity identity, const Json& message) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            std::size_t bytes = 0;
            bytes = message.dump().size();
            ReceiveResult failure;
            if (!impl->beginInbound(identity, bytes, failure)) {
                return failure;
            }
            return impl->receiveJsonLocked(identity, message);
        } catch (...) {
            return containedReceiveFailure();
        }
    }

    ReceiveResult ServerCore::receive(ConnectionIdentity identity, std::string_view compactJson) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            ReceiveResult failure;
            if (!impl->beginInbound(identity, compactJson.size(), failure)) {
                return failure;
            }
            Json message = Json::parse(compactJson, nullptr, false);
            if (message.is_discarded()) {
                return impl->protocolFailure(identity, codecFailure(ErrorCode::MalformedJson, "message is not valid JSON", false));
            }
            return impl->receiveJsonLocked(identity, message);
        } catch (...) {
            return containedReceiveFailure();
        }
    }

    ReceiveResult ServerCore::receiveDefinedCommand(ConnectionIdentity identity, const generated::DefinedCommand& command) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            const auto encoded = Codec::serializeDefinedCommand(command);
            if (!encoded) {
                return impl->protocolFailure(identity, encoded.error());
            }
            ReceiveResult failure;
            if (!impl->beginInbound(identity, encoded.value().size(), failure)) {
                return failure;
            }
            return impl->receiveDefinedCommandLocked(identity, command);
        } catch (...) {
            return containedReceiveFailure();
        }
    }

    ReceiveResult ServerCore::receiveError(ConnectionIdentity identity, CodecError error) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            return impl->protocolFailure(identity, std::move(error));
        } catch (...) {
            return containedReceiveFailure();
        }
    }

    AuthenticationFailureCode ServerCore::recordPreAuthenticationFailure(const FrontendPeerContext& peer,
                                                                         AuthenticationFailureCode failure) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            if (!impl->open || impl->terminallyClosed) {
                return AuthenticationFailureCode::RateLimited;
            }
            return impl->recordAuthenticationFailureLocked(peer, failure);
        } catch (...) {
            return AuthenticationFailureCode::RateLimited;
        }
    }

    bool ServerCore::complete(BackendCompletion completion) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            return impl->complete(std::move(completion));
        } catch (...) {
            return false;
        }
    }

    OccurrenceStageResult
    ServerCore::stageGroup(OccurrenceCoalescingKey key, model::OccurrenceDraft occurrence, OccurrenceFlushUrgency urgency) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            return impl->stageOccurrenceLocked(std::move(key), std::move(occurrence), urgency);
        } catch (...) {
            try {
                Impl::DispatchScope dispatch(*impl);
                impl->dirtySnapshotRequired = true;
                impl->requirePendingDeliverySnapshot(Impl::PendingSnapshotSequenceMode::AdvanceSequence);
                impl->requestFlush();
            } catch (...) {
            }
            return {OccurrenceStageStatus::AllocationFailure, false, urgency == OccurrenceFlushUrgency::Immediate};
        }
    }

    OccurrenceStageResult ServerCore::stageGroups(std::vector<OccurrenceStageRequest> groups) noexcept {
        OccurrenceStageResult aggregate{OccurrenceStageStatus::Accepted, false, false};
        try {
            Impl::DispatchScope dispatch(*impl);
            for (OccurrenceStageRequest& group : groups) {
                const OccurrenceStageResult staged =
                    impl->stageOccurrenceLocked(std::move(group.key), std::move(group.occurrence), group.urgency);
                aggregate.scheduleRequired = aggregate.scheduleRequired || staged.scheduleRequired;
                aggregate.immediateFlush = aggregate.immediateFlush || staged.immediateFlush;
                if (staged.status == OccurrenceStageStatus::Accepted) {
                    continue;
                }
                aggregate.status = staged.status;
                if (staged.status == OccurrenceStageStatus::InvalidOccurrence) {
                    impl->dirtySnapshotRequired = true;
                    impl->requirePendingDeliverySnapshot(Impl::PendingSnapshotSequenceMode::AdvanceSequence);
                    impl->requestFlush();
                    aggregate.status = OccurrenceStageStatus::SnapshotRequired;
                    aggregate.immediateFlush = true;
                }
                break;
            }
            return aggregate;
        } catch (...) {
            try {
                Impl::DispatchScope dispatch(*impl);
                impl->dirtySnapshotRequired = true;
                impl->requirePendingDeliverySnapshot(Impl::PendingSnapshotSequenceMode::AdvanceSequence);
                impl->requestFlush();
            } catch (...) {
            }
            return {OccurrenceStageStatus::AllocationFailure, aggregate.scheduleRequired, true};
        }
    }

    OccurrenceStageResult ServerCore::requireSnapshot(OccurrenceFlushUrgency urgency) noexcept {
        const bool immediate = urgency == OccurrenceFlushUrgency::Immediate;
        try {
            Impl::DispatchScope dispatch(*impl);
            const bool scheduleRequired = impl->dirtyOccurrences.empty() && !impl->dirtySnapshotRequired;
            impl->dirtySnapshotRequired = true;
            impl->requirePendingDeliverySnapshot(Impl::PendingSnapshotSequenceMode::AdvanceSequence);
            impl->requestFlush();
            return {OccurrenceStageStatus::SnapshotRequired, scheduleRequired, immediate};
        } catch (...) {
            return {OccurrenceStageStatus::AllocationFailure, false, true};
        }
    }

    PublishResult ServerCore::publishGroup(model::OccurrenceDraft occurrence) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            return impl->appendAndStageDelivery(std::move(occurrence), true);
        } catch (...) {
            PublishResult result;
            result.error = ErrorCode::InternalError;
            return result;
        }
    }

    SnapshotPublishResult ServerCore::publishSnapshot(model::CanonicalSnapshot snapshot) noexcept {
        SnapshotPublishResult result;
        try {
            Impl::DispatchScope dispatch(*impl);
            if (!impl->open) {
                result.error = ErrorCode::BackendUnavailable;
                return result;
            }
            const bool deferUntilSnapshotBarrier = impl->backendSnapshotDepth != 0;

            impl->dirtyOccurrences.clear();
            impl->nextDirtyInsertionOrder = 0;
            impl->dirtySnapshotRequired = false;
            impl->pendingDelivery.clear();
            impl->pendingDeliveryGroups = 0;
            impl->pendingDeliverySnapshotMode = Impl::PendingSnapshotSequenceMode::None;
            if (!impl->journal->invalidateReplay()) {
                impl->sequenceExhausted = true;
            }
            const Impl::SnapshotBarrier barrier = impl->captureSnapshotBarrier();
            snapshot = impl->applySnapshotBarrier(std::move(snapshot), barrier);
            // A live Snapshot is an authoritative same-sequence barrier. At
            // the maximum sequence it remains accepted and carries exhausted
            // cursor metadata; it does not report contradictory failure.
            result.accepted = true;
            result.sequence = snapshot.sequence;

            std::vector<Impl::FrozenSnapshotRecipient> recipients;
            recipients.reserve(impl->connections.size());
            for (const auto& [identity, connection] : impl->connections) {
                if (connection.helloComplete && !connection.closing) {
                    const Impl::ConnectionToken token{identity, connection.generation};
                    if (std::optional<Impl::FrozenSnapshotRecipient> recipient = impl->freezeSnapshotRecipient(token)) {
                        recipients.push_back(std::move(*recipient));
                    }
                }
            }
            for (const Impl::FrozenSnapshotRecipient& recipient : recipients) {
                if (!impl->findConnection(Impl::ConnectionContinuation{recipient.token, true, false})) {
                    continue;
                }
                if (!impl->enqueueFrozenSnapshot(recipient, snapshot, deferUntilSnapshotBarrier)) {
                    impl->closeNow(
                        recipient.token.identity,
                        ConnectionClose{"frontend live snapshot projection or queueing failed", ErrorCode::InternalError, false});
                    continue;
                }
                ++result.recipientCount;
            }
            impl->requestFlush();
            return result;
        } catch (...) {
            result.accepted = false;
            result.error = ErrorCode::InternalError;
            return result;
        }
    }

    std::optional<model::SessionIdentity> ServerCore::reserveExternalSessionIdentity() noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            if (!impl->open || impl->nextExternalSessionIdentity <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                return std::nullopt;
            }
            model::SessionIdentity identity(std::to_string(impl->nextExternalSessionIdentity));
            --impl->nextExternalSessionIdentity;
            return identity;
        } catch (...) {
            return std::nullopt;
        }
    }

    bool ServerCore::replaceExternalTopology(std::vector<model::SessionState> sessions,
                                             std::optional<model::SessionIdentity> controller,
                                             bool bridgeControllerPresent) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            if (!impl->open) {
                return false;
            }
            std::map<model::SessionIdentity, model::SessionState> replacement;
            for (model::SessionState& session : sessions) {
                if (!replacement.emplace(session.id, std::move(session)).second) {
                    return false;
                }
            }
            if (controller && !replacement.contains(*controller)) {
                return false;
            }
            for (auto& [identity, session] : replacement) {
                session.role = controller && *controller == identity ? SessionRole::Controller : SessionRole::Observer;
            }
            impl->externalSessions = std::move(replacement);
            if (controller) {
                impl->controller.reset();
            } else if (!bridgeControllerPresent && !impl->controllerTransaction) {
                // An authoritative BackendCore Snapshot says no bridge-owned
                // command session owns the controller. Preserve an in-flight
                // acquire/release transaction until its typed completion, but
                // otherwise clear stale local ownership before composing the
                // canonical Snapshot barrier.
                impl->controller.reset();
            }
            impl->externalController = std::move(controller);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool ServerCore::externalSessionChanged(model::SessionState session, bool connected) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            if (!impl->open) {
                return false;
            }
            const model::SessionIdentity identity = session.id;
            if (connected) {
                session.role =
                    impl->externalController && *impl->externalController == identity ? SessionRole::Controller : SessionRole::Observer;
                if (const auto existing = impl->externalSessions.find(identity);
                    existing != impl->externalSessions.end() && existing->second == session) {
                    return true;
                }
                impl->externalSessions.insert_or_assign(identity, session);
            } else {
                if (!impl->externalSessions.contains(identity)) {
                    return true;
                }
                if (impl->externalController && *impl->externalController == identity) {
                    impl->externalController.reset();
                    impl->stageControllerChangedLocked();
                }
                impl->externalSessions.erase(identity);
                session.role = SessionRole::Observer;
            }
            impl->stageSessionChangedLocked(std::move(session), connected);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool ServerCore::externalControllerChanged(std::optional<model::SessionIdentity> controller) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            if (!impl->open || (controller && !impl->externalSessions.contains(*controller))) {
                return false;
            }
            const std::optional<model::SessionIdentity> before = impl->controllerStateLocked().session;
            if (controller) {
                impl->controller.reset();
            }
            impl->externalController = std::move(controller);
            for (auto& [identity, session] : impl->externalSessions) {
                session.role =
                    impl->externalController && *impl->externalController == identity ? SessionRole::Controller : SessionRole::Observer;
            }
            if (before != impl->controllerStateLocked().session) {
                impl->stageControllerChangedLocked();
            }
            return true;
        } catch (...) {
            return false;
        }
    }

    void ServerCore::invalidateReplay() noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            impl->dirtyOccurrences.clear();
            impl->nextDirtyInsertionOrder = 0;
            impl->dirtySnapshotRequired = false;
            impl->pendingDelivery.clear();
            impl->pendingDeliveryGroups = 0;
            impl->pendingDeliverySnapshotMode = Impl::PendingSnapshotSequenceMode::None;
            if (!impl->journal->invalidateReplay()) {
                impl->sequenceExhausted = true;
            }
        } catch (...) {
        }
    }

    void ServerCore::flush() {
        Impl::DispatchScope dispatch(*impl);
        if (impl->flushActive || impl->deferredPumpActive || impl->dispatchDepth > 1) {
            impl->requestFlush();
            return;
        }
        impl->scheduled = false;
        impl->scheduledFlushGeneration = 0;
        impl->inlineScheduledFlush.reset();
        impl->flushRequested = false;
        impl->runFlushTurn();
    }

    void ServerCore::flushConnection(ConnectionIdentity identity) {
        Impl::DispatchScope dispatch(*impl);
        if (impl->flushActive || impl->deferredPumpActive || impl->dispatchDepth > 1) {
            impl->requestFlush();
            return;
        }
        impl->flushActive = true;
        try {
            if (const std::optional<Impl::ConnectionToken> token = impl->connectionToken(identity)) {
                impl->flushConnectionLocked(*token);
            }
        } catch (...) {
            impl->flushActive = false;
            throw;
        }
        impl->flushActive = false;
    }

    void ServerCore::declareTransportFamily(FrontendTransportKind transport) {
        Impl::DispatchScope dispatch(*impl);
        std::size_t& declarations = impl->transportFamilies[transport];
        if (declarations < std::numeric_limits<std::size_t>::max()) {
            ++declarations;
        }
    }

    void ServerCore::withdrawTransportFamily(FrontendTransportKind transport) noexcept {
        try {
            Impl::DispatchScope dispatch(*impl);
            const auto found = impl->transportFamilies.find(transport);
            if (found == impl->transportFamilies.end()) {
                return;
            }
            if (found->second > 1) {
                --found->second;
            } else {
                impl->transportFamilies.erase(found);
            }
        } catch (...) {
        }
    }

    bool ServerCore::isOpen() const noexcept {
        try {
            return impl->open;
        } catch (...) {
            return false;
        }
    }

    bool ServerCore::flushScheduled() const noexcept {
        try {
            return impl->scheduled || impl->flushRequested || impl->inlineScheduledFlush.has_value();
        } catch (...) {
            return false;
        }
    }

    model::FrontendSequence ServerCore::currentSequence() const noexcept {
        try {
            return impl->journal->currentSequence();
        } catch (...) {
            return {};
        }
    }

    std::size_t ServerCore::connectionCount() const noexcept {
        try {
            return impl->connections.size();
        } catch (...) {
            return 0;
        }
    }

    std::size_t ServerCore::unauthenticatedConnectionCount() const noexcept {
        try {
            return static_cast<std::size_t>(std::count_if(impl->connections.begin(), impl->connections.end(), [](const auto& entry) {
                return !entry.second.helloComplete;
            }));
        } catch (...) {
            return 0;
        }
    }

    std::size_t ServerCore::authenticatedConnectionCount() const noexcept {
        try {
            return static_cast<std::size_t>(std::count_if(impl->connections.begin(), impl->connections.end(), [](const auto& entry) {
                return entry.second.helloComplete;
            }));
        } catch (...) {
            return 0;
        }
    }

    std::optional<model::SessionIdentity> ServerCore::currentController() const {
        if (impl->controller) {
            const Impl::Connection* connection = impl->findConnection(*impl->controller);
            return connection ? connection->session : std::nullopt;
        }
        return impl->externalController;
    }

    std::optional<model::SessionIdentity> ServerCore::session(ConnectionIdentity identity) const {
        const Impl::Connection* connection = impl->findConnection(identity);
        return connection ? connection->session : std::nullopt;
    }

    bool ServerCore::connectionOpen(ConnectionIdentity identity) const noexcept {
        try {
            return impl->findConnection(identity) != nullptr;
        } catch (...) {
            return false;
        }
    }

    std::optional<FrontendPrincipal> ServerCore::principal(ConnectionIdentity identity) const {
        const Impl::Connection* connection = impl->findConnection(identity);
        return connection ? connection->principal : std::nullopt;
    }

    std::optional<FrontendPeerContext> ServerCore::peer(ConnectionIdentity identity) const {
        const Impl::Connection* connection = impl->findConnection(identity);
        return connection ? std::optional<FrontendPeerContext>{connection->peer} : std::nullopt;
    }

    bool ServerCore::helloComplete(ConnectionIdentity identity) const noexcept {
        try {
            const Impl::Connection* connection = impl->findConnection(identity);
            return connection && connection->helloComplete;
        } catch (...) {
            return false;
        }
    }

    std::size_t ServerCore::queuedMessages(ConnectionIdentity identity) const noexcept {
        try {
            const Impl::Connection* connection = impl->findConnection(identity);
            return connection ? connection->outbound.size() + connection->deferredSnapshotOutbound.size() : 0;
        } catch (...) {
            return 0;
        }
    }

    std::size_t ServerCore::queuedBytes(ConnectionIdentity identity) const noexcept {
        try {
            const Impl::Connection* connection = impl->findConnection(identity);
            return connection ? connection->outboundBytes : 0;
        } catch (...) {
            return 0;
        }
    }

    std::size_t ServerCore::outstandingCommands(ConnectionIdentity identity) const noexcept {
        try {
            const Impl::Connection* connection = impl->findConnection(identity);
            return connection ? connection->outstanding.size() : 0;
        } catch (...) {
            return 0;
        }
    }

    std::vector<FrontendMethod> ServerCore::definedMethods() const {
        return impl->definedMethodsLocked();
    }

    std::vector<FrontendMethod> ServerCore::implementedMethods() const {
        return impl->implementedMethodsLocked();
    }

    std::vector<FrontendMethod> ServerCore::availableMethods() const {
        return impl->availableMethodsLocked();
    }

    std::vector<FrontendMethod> ServerCore::permittedMethods(const FrontendPrincipal& principal) const {
        return impl->permittedMethodsLocked(principal);
    }

    std::vector<FrontendCapability> ServerCore::implementedCapabilities() const {
        return impl->implementedCapabilitiesLocked();
    }

    std::vector<FrontendTransportKind> ServerCore::enabledTransportFamilies() const {
        std::vector<FrontendTransportKind> result;
        result.reserve(impl->transportFamilies.size());
        for (const auto& [transport, declarations] : impl->transportFamilies) {
            static_cast<void>(declarations);
            result.push_back(transport);
        }
        return result;
    }

} // namespace ai::openai::codex::frontend::internal::server
