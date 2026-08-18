/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef TESTS_COMPONENT_CODEX_CODEXFRONTENDCOMPATIBILITYADAPTERS_H
#define TESTS_COMPONENT_CODEX_CODEXFRONTENDCOMPATIBILITYADAPTERS_H

#include "ai/openai/codex/frontend/FrontendService.h"
#include "ai/openai/codex/frontend/client/Changes.h"
#include "ai/openai/codex/frontend/client/GeneratedBindings.h"
#include "ai/openai/codex/frontend/client/Transport.h"
#include "ai/openai/codex/frontend/internal/client/ClientCore.h"
#include "ai/openai/codex/frontend/internal/server/ServerCore.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tests::codex::frontend_compatibility {

    namespace frontend = ::ai::openai::codex::frontend;
    namespace public_client = ::ai::openai::codex::frontend::client;
    namespace core_client = ::ai::openai::codex::frontend::internal::client;
    namespace core_model = ::ai::openai::codex::frontend::internal::model;
    namespace core_server = ::ai::openai::codex::frontend::internal::server;

    inline core_server::ServerCoreOptions serverOptions(const frontend::FrontendServiceOptions& source) {
        core_server::ServerCoreOptions target;
        target.journalMaximumEntries = source.journal.maxEntries;
        target.journalMaximumBytes = source.journal.maxBytes;
        target.journalInitialSequence = core_model::FrontendSequence(source.journal.initialSequence);
        target.maxDirtyEntities = source.coalescer.maxDirtyEntities;
        target.maxPendingDeliveryGroups = source.coalescer.maxDirtyEntities;
        target.maxEventsPerBatch = source.batches.maxEvents;
        target.maxBatchBytes = source.batches.maxSerializedBytes;
        target.maxOutboundMessagesPerConnection = source.maxOutboundMessagesPerConnection;
        target.maxOutboundBytesPerConnection = source.maxOutboundBytesPerConnection;
        target.maxMessagesPerDelivery = source.maxMessagesPerDelivery;
        target.maxConnections = source.maxConnections;
        target.maxUnauthenticatedConnections = source.maxUnauthenticatedConnections;
        target.handshakeTimeoutMs = source.handshakeTimeoutMs;
        target.maximumInboundMessageBytes = source.maximumInboundMessageBytes;
        target.maxInboundMessagesPerSecond = source.maxInboundMessagesPerSecond;
        target.maxInboundBurst = source.maxInboundBurst;
        target.maxOutstandingCommandsPerConnection = source.maxOutstandingCommandsPerConnection;
        target.maximumFailedAuthenticationsPerPeer = source.maximumFailedAuthenticationsPerPeer;
        target.failedAuthenticationWindowMs = source.failedAuthenticationWindowMs;
        target.allowVerifiedLocalTrust = source.allowVerifiedLocalTrust;
        target.allowInsecureLocalTrust = source.allowInsecureLocalTrust;
        target.trustedLocalUserId = source.trustedLocalUserId;
        target.enableFilesystemReadMethods = source.enableFilesystemReadMethods;
        target.enableFilesystemWriteMethods = source.enableFilesystemWriteMethods;
        target.enableCommandExecutionMethods = source.enableCommandExecutionMethods;
        target.filesystemReadPolicy = source.filesystemReadPolicy;
        target.filesystemWritePolicy = source.filesystemWritePolicy;
        target.commandExecutionPolicy = source.commandExecutionPolicy;
        target.authenticator = source.authenticator;
        target.scheduler = source.scheduler;
        target.timerScheduler = source.timerScheduler;
        target.monotonicClockMs = source.monotonicClockMs;
        return target;
    }

    // This reverse projection covers every member of the frozen public
    // FrontendServiceOptions. Core-only P2 policy fields intentionally have no
    // public P2 representation and remain an adapter-owned wiring choice.
    inline frontend::FrontendServiceOptions publicServerOptions(const core_server::ServerCoreOptions& source) {
        frontend::FrontendServiceOptions target;
        target.journal.maxEntries = source.journalMaximumEntries;
        target.journal.maxBytes = source.journalMaximumBytes;
        target.journal.initialSequence = frontend::SequenceNumber(source.journalInitialSequence.value());
        target.coalescer.maxDirtyEntities = source.maxDirtyEntities;
        target.batches.maxEvents = source.maxEventsPerBatch;
        target.batches.maxSerializedBytes = source.maxBatchBytes;
        target.maxOutboundMessagesPerConnection = source.maxOutboundMessagesPerConnection;
        target.maxOutboundBytesPerConnection = source.maxOutboundBytesPerConnection;
        target.maxMessagesPerDelivery = source.maxMessagesPerDelivery;
        target.maxConnections = source.maxConnections;
        target.maxUnauthenticatedConnections = source.maxUnauthenticatedConnections;
        target.handshakeTimeoutMs = source.handshakeTimeoutMs;
        target.maximumInboundMessageBytes = source.maximumInboundMessageBytes;
        target.maxInboundMessagesPerSecond = source.maxInboundMessagesPerSecond;
        target.maxInboundBurst = source.maxInboundBurst;
        target.maxOutstandingCommandsPerConnection = source.maxOutstandingCommandsPerConnection;
        target.maximumFailedAuthenticationsPerPeer = source.maximumFailedAuthenticationsPerPeer;
        target.failedAuthenticationWindowMs = source.failedAuthenticationWindowMs;
        target.allowVerifiedLocalTrust = source.allowVerifiedLocalTrust;
        target.allowInsecureLocalTrust = source.allowInsecureLocalTrust;
        target.trustedLocalUserId = source.trustedLocalUserId;
        target.enableFilesystemReadMethods = source.enableFilesystemReadMethods;
        target.enableFilesystemWriteMethods = source.enableFilesystemWriteMethods;
        target.enableCommandExecutionMethods = source.enableCommandExecutionMethods;
        target.filesystemReadPolicy = source.filesystemReadPolicy;
        target.filesystemWritePolicy = source.filesystemWritePolicy;
        target.commandExecutionPolicy = source.commandExecutionPolicy;
        target.authenticator = source.authenticator;
        target.scheduler = source.scheduler;
        target.timerScheduler = source.timerScheduler;
        target.monotonicClockMs = source.monotonicClockMs;
        return target;
    }

    inline core_server::ConnectionCallbacks serverCallbacks(frontend::FrontendConnectionCallbacks source) {
        return {
            [send = std::move(source.onMessage)](core_server::SerializedServerMessage outbound) mutable {
                if (!send) {
                    return false;
                }
                const std::size_t serializedBytes = outbound.compactJson.size();
                return send(frontend::OutboundMessage{
                    std::move(outbound.message), std::move(outbound.compactJson), serializedBytes});
            },
            [closed = std::move(source.onClosed)](const core_server::ConnectionClose& close) mutable {
                if (closed) {
                    closed(close.reason);
                }
            },
        };
    }

    inline core_client::ClientOptions clientOptions(const public_client::ClientOptions& source) {
        core_client::ClientOptions target;
        target.requestedCapabilities = source.requestedCapabilities;
        target.requiredCapabilities = source.requiredCapabilities;
        if (source.credentialProvider) {
            target.credentialProvider = [provider = source.credentialProvider]() mutable {
                public_client::AuthenticationContext authentication = provider();
                return core_client::AuthenticationContext{std::move(authentication.credential), std::move(authentication.continuityKey)};
            };
        }
        target.limits.maximumInboundMessageBytes = source.maximumInboundMessageBytes;
        target.limits.maximumOutboundMessageBytes = source.maximumOutboundMessageBytes;
        target.limits.maximumDecodedStateBytes = source.maximumDecodedStateBytes;
        target.limits.maximumPendingOperations = source.maximumPendingOperations;
        target.limits.maximumRetainedDiagnostics = source.maximumRetainedDiagnostics;
        target.allowLegacyV1 = source.allowLegacyV1;
        return target;
    }

    inline public_client::ClientOptions publicClientOptions(const core_client::ClientOptions& source) {
        public_client::ClientOptions target;
        target.requestedCapabilities = source.requestedCapabilities;
        target.requiredCapabilities = source.requiredCapabilities;
        if (source.credentialProvider) {
            target.credentialProvider = [provider = source.credentialProvider]() mutable {
                core_client::AuthenticationContext authentication = provider();
                return public_client::AuthenticationContext{std::move(authentication.credential), std::move(authentication.continuityKey)};
            };
        }
        target.maximumInboundMessageBytes = source.limits.maximumInboundMessageBytes;
        target.maximumOutboundMessageBytes = source.limits.maximumOutboundMessageBytes;
        target.maximumDecodedStateBytes = source.limits.maximumDecodedStateBytes;
        target.maximumPendingOperations = source.limits.maximumPendingOperations;
        target.maximumRetainedDiagnostics = source.limits.maximumRetainedDiagnostics;
        target.allowLegacyV1 = source.allowLegacyV1;
        return target;
    }

    inline core_client::SendStatus sendStatus(public_client::SendStatus status) noexcept {
        switch (status) {
            case public_client::SendStatus::Accepted:
                return core_client::SendStatus::Accepted;
            case public_client::SendStatus::Backpressure:
                return core_client::SendStatus::Backpressure;
            case public_client::SendStatus::Closed:
                return core_client::SendStatus::Closed;
            case public_client::SendStatus::Failed:
                return core_client::SendStatus::Failed;
        }
        return core_client::SendStatus::Failed;
    }

    inline core_client::TransportCallbacks clientTransportCallbacks(public_client::TransportCallbacks source) {
        return {
            [send = std::move(source.send)](core_client::OutboundMessage message) mutable {
                if (!send) {
                    return core_client::SendResult{core_client::SendStatus::Failed,
                                                   core_client::TransportError{"frontend transport send callback is absent", false}};
                }
                public_client::OutboundMessage outbound;
                outbound.sensitive = message.sensitive;
                if (const auto* hello = std::get_if<frontend::Hello>(&message.value)) {
                    outbound.kind = public_client::OutboundKind::Hello;
                    auto encoded = frontend::Codec::serializeClient(frontend::ClientMessage{*hello});
                    if (!encoded) {
                        return core_client::SendResult{core_client::SendStatus::Failed,
                                                       core_client::TransportError{"frontend Hello serialization failed", false}};
                    }
                    outbound.compactJson = std::move(encoded).value();
                } else {
                    outbound.kind = public_client::OutboundKind::Command;
                    auto encoded = frontend::Codec::serializeDefinedCommand(std::get<frontend::generated::DefinedCommand>(message.value));
                    if (!encoded) {
                        return core_client::SendResult{core_client::SendStatus::Failed,
                                                       core_client::TransportError{"frontend command serialization failed", false}};
                    }
                    outbound.compactJson = std::move(encoded).value();
                }
                outbound.serializedBytes = outbound.compactJson.size();
                if (outbound.serializedBytes > message.maximumBytes) {
                    return core_client::SendResult{
                        core_client::SendStatus::Failed,
                        core_client::TransportError{"frontend message exceeds the peer input limit", false}};
                }
                public_client::SendResult result = send(std::move(outbound));
                std::optional<core_client::TransportError> error;
                if (result.error) {
                    error = core_client::TransportError{std::move(result.error->message), result.error->retryable};
                }
                return core_client::SendResult{sendStatus(result.status), std::move(error)};
            },
            [close = std::move(source.close)](std::string_view reason) mutable {
                if (close) {
                    close(std::string(reason));
                }
            },
        };
    }

    inline public_client::ErrorOrigin errorOrigin(core_client::ErrorOrigin value) noexcept {
        switch (value) {
            case core_client::ErrorOrigin::Client:
                return public_client::ErrorOrigin::Client;
            case core_client::ErrorOrigin::Transport:
                return public_client::ErrorOrigin::Transport;
            case core_client::ErrorOrigin::Protocol:
                return public_client::ErrorOrigin::Protocol;
            case core_client::ErrorOrigin::Command:
                return public_client::ErrorOrigin::Command;
        }
        return public_client::ErrorOrigin::Client;
    }

    inline public_client::ClientErrorCode clientErrorCode(core_client::ClientErrorCode value) noexcept {
        return static_cast<public_client::ClientErrorCode>(value);
    }

    inline public_client::Error publicError(const core_client::ClientError& source) {
        return {errorOrigin(source.origin),
                source.clientCode ? std::optional<public_client::ClientErrorCode>{clientErrorCode(*source.clientCode)} : std::nullopt,
                source.protocolCode,
                source.message,
                std::nullopt,
                source.remoteDetails,
                source.retryable};
    }

    inline public_client::Submission publicSubmission(const core_client::Submission& source) {
        public_client::Submission target;
        if (source.requestId) {
            target.requestId = public_client::RequestId(*source.requestId);
        }
        if (source.error) {
            target.error = publicError(*source.error);
        }
        return target;
    }

    inline public_client::GeneratedOperationResult publicResult(const core_client::OperationResult& source) {
        public_client::GeneratedOperationResult target;
        target.requestId = public_client::RequestId(source.requestId);
        target.value = source.value;
        if (source.error) {
            target.error = publicError(*source.error);
        }
        return target;
    }

    inline public_client::SessionInfo publicSession(const core_client::SessionInfo& source) {
        public_client::SessionInfo target;
        target.sessionId = source.id.value();
        target.role = source.role;
        target.syncMode = source.synchronizationMode;
        target.serverCurrentSequence = frontend::SequenceNumber(source.serverCurrentSequence.value());
        target.serverVersion = source.serverVersion;
        target.requestedRepresentationCapabilities = source.requestedCapabilities;
        target.selectedRepresentationCapabilities = source.selectedCapabilities;
        for (frontend::FrontendCapability capability : source.observedCapabilities) {
            const auto found = std::find_if(frontend::generated::AllCapabilities.begin(),
                                            frontend::generated::AllCapabilities.end(),
                                            [capability](const frontend::generated::CapabilityMetadata& metadata) {
                                                return metadata.id == static_cast<frontend::generated::Capability>(capability);
                                            });
            if (found == frontend::generated::AllCapabilities.end() ||
                found->category == frontend::generated::CapabilityCategory::StaticMechanism) {
                target.observedMechanismCapabilities.push_back(capability);
            } else if (found->category == frontend::generated::CapabilityCategory::ConditionalTopology) {
                target.observedTopologyCapabilities.push_back(capability);
            } else {
                target.observedProductCapabilities.push_back(capability);
            }
        }
        target.availableMethods = source.availableMethods;
        target.permittedMethods = source.permittedMethods;
        target.permittedScopes = source.permittedScopes;
        return target;
    }

    inline public_client::ConnectionState connectionState(core_client::ConnectionState value) noexcept {
        switch (value) {
            case core_client::ConnectionState::Disconnected:
                return public_client::ConnectionState::Disconnected;
            case core_client::ConnectionState::Connecting:
                return public_client::ConnectionState::Connecting;
            case core_client::ConnectionState::Authenticating:
                return public_client::ConnectionState::Authenticating;
            case core_client::ConnectionState::Synchronizing:
                return public_client::ConnectionState::Synchronizing;
            case core_client::ConnectionState::Ready:
                return public_client::ConnectionState::Ready;
            case core_client::ConnectionState::Closing:
                return public_client::ConnectionState::Closing;
            case core_client::ConnectionState::Closed:
                return public_client::ConnectionState::Closed;
        }
        return public_client::ConnectionState::Closed;
    }

    static_assert(static_cast<unsigned>(core_client::ClientErrorCode::CallbackFailure) ==
                  static_cast<unsigned>(public_client::ClientErrorCode::CallbackFailure));
    static_assert(frontend::generated::AllMethods.size() == public_client::generated::AllBindings.size());

} // namespace tests::codex::frontend_compatibility

#endif // TESTS_COMPONENT_CODEX_CODEXFRONTENDCOMPATIBILITYADAPTERS_H
