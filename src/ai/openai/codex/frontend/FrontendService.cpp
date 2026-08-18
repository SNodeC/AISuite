/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/FrontendService.h"

#include "ai/openai/codex/frontend/internal/server/BackendCoreBridge.h"
#include "ai/openai/codex/frontend/internal/server/ServerCore.h"
#include "core/EventReceiver.h"
#include "core/timer/Timer.h"
#include "utils/Timeval.h"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace ai::openai::codex::frontend {

    namespace server = internal::server;

    namespace {

        FrontendServiceOptions normalizedOptions(FrontendServiceOptions options) {
            if (!options.scheduler) {
                options.scheduler = [](std::function<void()> callback) {
                    core::EventReceiver::atNextTick(std::move(callback));
                };
            }
            if (!options.monotonicClockMs) {
                options.monotonicClockMs = [] {
                    return static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
                };
            }
            if (!options.timerScheduler) {
                options.timerScheduler = [](std::uint64_t delayMs, std::function<void()> callback) {
                    auto timer = std::make_shared<std::optional<core::timer::Timer>>();
                    timer->emplace(
                        core::timer::Timer::singleshotTimer(std::move(callback), utils::Timeval(static_cast<double>(delayMs) / 1000.0)));
                    return [timer]() mutable {
                        if (timer && timer->has_value()) {
                            timer->value().cancel();
                            timer->reset();
                        }
                    };
                };
            }
            return options;
        }

        server::ServerCoreOptions coreOptions(const FrontendServiceOptions& options) {
            server::ServerCoreOptions result;
            result.journalMaximumEntries = options.journal.maxEntries;
            result.journalMaximumBytes = options.journal.maxBytes;
            result.journalInitialSequence = internal::model::FrontendSequence(options.journal.initialSequence);
            result.maxDirtyEntities = options.coalescer.maxDirtyEntities;
            result.maxPendingDeliveryGroups = options.coalescer.maxDirtyEntities;
            result.maxOutboundMessagesPerConnection = options.maxOutboundMessagesPerConnection;
            result.maxOutboundBytesPerConnection = options.maxOutboundBytesPerConnection;
            result.maxMessagesPerDelivery = options.maxMessagesPerDelivery;
            result.maxEventsPerBatch = options.batches.maxEvents;
            result.maxBatchBytes = options.batches.maxSerializedBytes;
            result.maxConnections = options.maxConnections;
            result.maxUnauthenticatedConnections = options.maxUnauthenticatedConnections;
            result.maximumInboundMessageBytes = options.maximumInboundMessageBytes;
            result.maxInboundMessagesPerSecond = options.maxInboundMessagesPerSecond;
            result.maxInboundBurst = options.maxInboundBurst;
            result.maxOutstandingCommandsPerConnection = options.maxOutstandingCommandsPerConnection;
            result.maximumFailedAuthenticationsPerPeer = options.maximumFailedAuthenticationsPerPeer;
            result.failedAuthenticationWindowMs = options.failedAuthenticationWindowMs;
            result.handshakeTimeoutMs = options.handshakeTimeoutMs;
            result.allowVerifiedLocalTrust = options.allowVerifiedLocalTrust;
            result.allowInsecureLocalTrust = options.allowInsecureLocalTrust;
            result.trustedLocalUserId = options.trustedLocalUserId;
            result.enableFilesystemReadMethods = options.enableFilesystemReadMethods;
            result.enableFilesystemWriteMethods = options.enableFilesystemWriteMethods;
            result.enableCommandExecutionMethods = options.enableCommandExecutionMethods;
#if defined(AISUITE_CODEX_CPP_CLIENT_SDK_BUILT) && AISUITE_CODEX_CPP_CLIENT_SDK_BUILT
            result.implementedCapabilities.push_back(FrontendCapability::CppClientSdk);
#endif
            result.filesystemReadPolicy = options.filesystemReadPolicy;
            result.filesystemWritePolicy = options.filesystemWritePolicy;
            result.commandExecutionPolicy = options.commandExecutionPolicy;
            result.authenticator = options.authenticator;
            result.scheduler = options.scheduler;
            result.timerScheduler = options.timerScheduler;
            result.monotonicClockMs = options.monotonicClockMs;
            return result;
        }

        std::shared_ptr<server::ServerCore> makeServerCore(const std::shared_ptr<server::BackendCoreBridge>& bridge,
                                                           server::ServerCoreOptions options) {
            // ServerCore stores its BackendPort as a borrowed reference. Keep
            // the heap-owned bridge alive in the ServerCore control block so a
            // reentrant service close cannot invalidate that reference while
            // an already-entered core callback still holds the core alive.
            return std::shared_ptr<server::ServerCore>(new server::ServerCore(*bridge, std::move(options)),
                                                       [bridge](server::ServerCore* core) {
                                                           static_cast<void>(bridge);
                                                           delete core;
                                                       });
        }

        ConnectionReceiveStatus receiveStatus(server::ReceiveStatus status) noexcept {
            switch (status) {
                case server::ReceiveStatus::Accepted:
                    return ConnectionReceiveStatus::Accepted;
                case server::ReceiveStatus::Rejected:
                    return ConnectionReceiveStatus::Rejected;
                case server::ReceiveStatus::Closing:
                    return ConnectionReceiveStatus::Closing;
                case server::ReceiveStatus::Closed:
                case server::ReceiveStatus::UnknownConnection:
                    return ConnectionReceiveStatus::Closed;
            }
            return ConnectionReceiveStatus::Closed;
        }

        ConnectionReceiveResult publicResult(server::ReceiveResult result) {
            return {receiveStatus(result.status), std::move(result.error)};
        }

    } // namespace

    struct FrontendConnection::Control {
        std::weak_ptr<FrontendService::Impl> service;
        server::ConnectionIdentity identity;
    };

    class FrontendService::Impl : public std::enable_shared_from_this<FrontendService::Impl> {
    public:
        Impl(backend::detail::BackendCoreRuntime& backend, FrontendServiceOptions configuredOptions)
            : options(normalizedOptions(std::move(configuredOptions)))
            , bridge(std::make_shared<server::BackendCoreBridge>(backend, options.maxOutboundBytesPerConnection))
            , core(makeServerCore(bridge, coreOptions(options))) {
            bridge->bindLifetime(core);
            core->start();
            bridge->start();
        }

        ~Impl() {
            shutdown("frontend service closed");
        }

        FrontendConnection openConnection(FrontendPeerContext peer, FrontendConnectionCallbacks callbacks) {
            struct CallbackState {
                FrontendConnectionCallbacks callbacks;
            };
            auto callbackState = std::make_shared<CallbackState>(CallbackState{std::move(callbacks)});
            const std::shared_ptr<server::ServerCore> target = core;
            if (!target) {
                return {};
            }
            const std::optional<server::ConnectionIdentity> identity = target->openConnection(
                std::move(peer),
                server::ConnectionCallbacks{[callbackState](server::SerializedServerMessage outbound) {
                                                if (!callbackState->callbacks.onMessage) {
                                                    return false;
                                                }
                                                const std::size_t serializedBytes = outbound.compactJson.size();
                                                return callbackState->callbacks.onMessage(
                                                    OutboundMessage{std::move(outbound.message),
                                                                    std::move(outbound.compactJson),
                                                                    serializedBytes});
                                            },
                                            [callbackState](const server::ConnectionClose& close) {
                                                if (callbackState->callbacks.onClosed) {
                                                    try {
                                                        callbackState->callbacks.onClosed(close.reason);
                                                    } catch (...) {
                                                    }
                                                }
                                            }});
            if (!identity) {
                return {};
            }
            auto control = std::make_shared<FrontendConnection::Control>();
            control->service = shared_from_this();
            control->identity = *identity;
            return FrontendConnection(std::move(control));
        }

        void shutdown(std::string reason) noexcept {
            if (closed) {
                return;
            }
            closed = true;
            const std::shared_ptr<server::ServerCore> target = core;
            if (target) {
                target->close(std::move(reason));
            }
            bridge->close();
            core.reset();
            bridge.reset();
        }

        FrontendServiceOptions options;
        std::shared_ptr<server::BackendCoreBridge> bridge;
        std::shared_ptr<server::ServerCore> core;
        bool closed = false;
    };

    FrontendConnection::FrontendConnection() noexcept = default;

    FrontendConnection::FrontendConnection(std::shared_ptr<Control> configuredControl) noexcept
        : control(std::move(configuredControl)) {
    }

    FrontendConnection::FrontendConnection(FrontendConnection&& other) noexcept
        : control(std::move(other.control)) {
    }

    FrontendConnection& FrontendConnection::operator=(FrontendConnection&& other) noexcept {
        if (this != &other) {
            close();
            control = std::move(other.control);
        }
        return *this;
    }

    FrontendConnection::~FrontendConnection() {
        close();
    }

    ConnectionReceiveResult FrontendConnection::receive(const ClientMessage& message) noexcept {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? publicResult(target->receive(control->identity, message))
                      : ConnectionReceiveResult{ConnectionReceiveStatus::Closed, std::nullopt};
    }

    ConnectionReceiveResult FrontendConnection::receive(const Json& message) noexcept {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? publicResult(target->receive(control->identity, message))
                      : ConnectionReceiveResult{ConnectionReceiveStatus::Closed, std::nullopt};
    }

    ConnectionReceiveResult FrontendConnection::receive(std::string_view compactJson) noexcept {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? publicResult(target->receive(control->identity, compactJson))
                      : ConnectionReceiveResult{ConnectionReceiveStatus::Closed, std::nullopt};
    }

    ConnectionReceiveResult FrontendConnection::receiveError(CodecError error) noexcept {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? publicResult(target->receiveError(control->identity, std::move(error)))
                      : ConnectionReceiveResult{ConnectionReceiveStatus::Closed, std::move(error)};
    }

    bool FrontendConnection::updatePeerContext(FrontendPeerContext peer) noexcept {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target && target->updatePeerContext(control->identity, std::move(peer));
    }

    void FrontendConnection::close(std::string reason) noexcept {
        if (!control) {
            return;
        }
        if (const std::shared_ptr<FrontendService::Impl> service = control->service.lock()) {
            if (const std::shared_ptr<server::ServerCore> target = service->core) {
                target->closeConnection(control->identity, std::move(reason));
            }
        }
        control.reset();
    }

    bool FrontendConnection::isOpen() const noexcept {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target && target->connectionOpen(control->identity);
    }

    bool FrontendConnection::helloComplete() const noexcept {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target && target->helloComplete(control->identity);
    }

    std::optional<std::string> FrontendConnection::sessionId() const {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        const std::optional<internal::model::SessionIdentity> session = target ? target->session(control->identity) : std::nullopt;
        return session ? std::optional<std::string>{session->value()} : std::nullopt;
    }

    std::optional<FrontendPrincipal> FrontendConnection::principal() const {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->principal(control->identity) : std::nullopt;
    }

    FrontendPeerContext FrontendConnection::peer() const {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        const std::optional<FrontendPeerContext> peer = target ? target->peer(control->identity) : std::nullopt;
        return peer.value_or(FrontendPeerContext{});
    }

    std::size_t FrontendConnection::queuedMessages() const noexcept {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->queuedMessages(control->identity) : 0;
    }

    std::size_t FrontendConnection::queuedBytes() const noexcept {
        const std::shared_ptr<FrontendService::Impl> service = control ? control->service.lock() : nullptr;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->queuedBytes(control->identity) : 0;
    }

    FrontendService::FrontendService(backend::detail::BackendCoreRuntime& backend, FrontendServiceOptions options)
        : impl(std::make_shared<Impl>(backend, std::move(options))) {
    }

    FrontendService::~FrontendService() {
        close();
    }

    FrontendConnection FrontendService::openConnection(FrontendPeerContext peer, FrontendConnectionCallbacks callbacks) {
        const std::shared_ptr<Impl> service = impl;
        return service ? service->openConnection(std::move(peer), std::move(callbacks)) : FrontendConnection{};
    }

    AuthenticationFailureCode FrontendService::recordPreAuthenticationFailure(const FrontendPeerContext& peer,
                                                                              AuthenticationFailureCode failure) noexcept {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->recordPreAuthenticationFailure(peer, failure) : AuthenticationFailureCode::RateLimited;
    }

    void FrontendService::declareTransportFamily(FrontendTransportKind transport) {
        const std::shared_ptr<Impl> service = impl;
        if (const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr) {
            target->declareTransportFamily(transport);
        }
    }

    void FrontendService::withdrawTransportFamily(FrontendTransportKind transport) noexcept {
        const std::shared_ptr<Impl> service = impl;
        if (const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr) {
            target->withdrawTransportFamily(transport);
        }
    }

    void FrontendService::flush() {
        const std::shared_ptr<Impl> service = impl;
        if (const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr) {
            target->flush();
        }
    }

    void FrontendService::close(std::string reason) noexcept {
        std::shared_ptr<Impl> service = std::move(impl);
        if (service) {
            service->shutdown(std::move(reason));
        }
    }

    bool FrontendService::isOpen() const noexcept {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target && target->isOpen();
    }

    bool FrontendService::flushScheduled() const noexcept {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target && target->flushScheduled();
    }

    SequenceNumber FrontendService::currentSequence() const noexcept {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->currentSequence().protocolValue() : SequenceNumber{};
    }

    std::size_t FrontendService::connectionCount() const noexcept {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->connectionCount() : 0;
    }

    std::size_t FrontendService::unauthenticatedConnectionCount() const noexcept {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->unauthenticatedConnectionCount() : 0;
    }

    std::size_t FrontendService::authenticatedConnectionCount() const noexcept {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->authenticatedConnectionCount() : 0;
    }

    std::optional<std::string> FrontendService::currentController() const {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        if (!target) {
            return std::nullopt;
        }
        const std::optional<internal::model::SessionIdentity> controller = target->currentController();
        return controller ? std::optional<std::string>{controller->value()} : std::nullopt;
    }

    std::vector<FrontendMethod> FrontendService::definedMethods() const {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->definedMethods() : std::vector<FrontendMethod>{};
    }

    std::vector<FrontendMethod> FrontendService::implementedMethods() const {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->implementedMethods() : std::vector<FrontendMethod>{};
    }

    std::vector<FrontendMethod> FrontendService::availableMethods() const {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->availableMethods() : std::vector<FrontendMethod>{};
    }

    std::vector<FrontendMethod> FrontendService::permittedMethods(const FrontendPrincipal& principal) const {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->permittedMethods(principal) : std::vector<FrontendMethod>{};
    }

    std::vector<FrontendTransportKind> FrontendService::enabledTransportFamilies() const {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->enabledTransportFamilies() : std::vector<FrontendTransportKind>{};
    }

    std::vector<FrontendCapability> FrontendService::implementedCapabilities() const {
        const std::shared_ptr<Impl> service = impl;
        const std::shared_ptr<server::ServerCore> target = service ? service->core : nullptr;
        return target ? target->implementedCapabilities() : std::vector<FrontendCapability>{};
    }

    EventJournalConfig FrontendService::journalConfig() const noexcept {
        const std::shared_ptr<Impl> service = impl;
        return service ? service->options.journal : EventJournalConfig{};
    }

    UpdateBatchConfig FrontendService::batchConfig() const noexcept {
        const std::shared_ptr<Impl> service = impl;
        return service ? service->options.batches : UpdateBatchConfig{};
    }

} // namespace ai::openai::codex::frontend
