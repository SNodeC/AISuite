/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/ClientConnection.h"

#include "apps/codex-backend-client/CodexBackendClientSocketContext.h"

#include <limits>
#include <utility>

namespace apps::codex_backend_client {
    namespace sdk = ai::openai::codex::frontend::client;

    std::optional<PhysicalConnectionAttemptGate::Generation> PhysicalConnectionAttemptGate::begin() noexcept {
        if (activeGeneration || nextGeneration == 0) {
            return std::nullopt;
        }
        const Generation generation = nextGeneration;
        if (nextGeneration == std::numeric_limits<Generation>::max()) {
            nextGeneration = 0;
        } else {
            ++nextGeneration;
        }
        activeGeneration = generation;
        return generation;
    }

    bool PhysicalConnectionAttemptGate::isCurrent(const Generation generation) const noexcept {
        return activeGeneration == generation;
    }

    bool PhysicalConnectionAttemptGate::complete(const Generation generation) noexcept {
        if (!isCurrent(generation)) {
            return false;
        }
        activeGeneration.reset();
        return true;
    }

    bool PhysicalConnectionAttemptGate::active() const noexcept {
        return activeGeneration.has_value();
    }

    std::optional<PhysicalConnectionAttemptGate::Generation> PhysicalConnectionAttemptGate::current() const noexcept {
        return activeGeneration;
    }

    ClientConnection::ClientConnection(sdk::Client& sdk, ClientConnectionCallbacks callbacks)
        : sdk(sdk)
        , callbacks(std::move(callbacks)) {
    }

    ClientConnection::~ClientConnection() {
        shutdown();
    }

    void ClientConnection::shutdown() noexcept {
        if (!applicationShutdown) {
            applicationShutdown = true;
            try {
                if (callbacks.onLocalShutdown) {
                    callbacks.onLocalShutdown();
                }
            } catch (...) {
            }
        }
        if (context != nullptr) {
            context->disconnect();
        }
    }

    bool ClientConnection::connected() const noexcept {
        return online && context != nullptr && protocolConnection.isTransportConnected();
    }

    bool ClientConnection::prepareAttempt(const PhysicalConnectionAttemptGate::Generation generation) noexcept {
        if (generation == 0 || context != nullptr || preparedGeneration != 0) {
            return false;
        }
        preparedGeneration = generation;
        return true;
    }

    void ClientConnection::cancelPreparedAttempt(const PhysicalConnectionAttemptGate::Generation generation) noexcept {
        if (preparedGeneration == generation && context == nullptr) {
            preparedGeneration = 0;
        }
    }

    bool ClientConnection::hasAttachment(const PhysicalConnectionAttemptGate::Generation generation) const noexcept {
        return context != nullptr && attachmentGeneration == generation;
    }

    PhysicalConnectionAttemptGate::Generation ClientConnection::preparedAttemptForFactory() const noexcept {
        return preparedGeneration;
    }

    bool ClientConnection::acceptsAttemptGeneration(const PhysicalConnectionAttemptGate::Generation attemptGeneration) const noexcept {
        const bool generationAware = callbacks.onAttemptConnected || callbacks.onAttemptDisconnected || callbacks.onAttemptFailure;
        return context == nullptr && (attemptGeneration == 0 || attemptGeneration == preparedGeneration) &&
               (!generationAware || attemptGeneration != 0);
    }

    void ClientConnection::attach(CodexBackendClientSocketContext& attached,
                                  const PhysicalConnectionAttemptGate::Generation attemptGeneration) noexcept {
        if (!acceptsAttemptGeneration(attemptGeneration)) {
            attached.disconnect();
            return;
        }
        context = &attached;
        attachmentGeneration = attemptGeneration;
        if (attemptGeneration != 0) {
            preparedGeneration = 0;
        }
        online = false;
        failureReported = false;
    }

    void ClientConnection::didConnect(CodexBackendClientSocketContext& attached) noexcept {
        if (context != &attached) {
            return;
        }
        protocolConnection = sdk.openConnection({[this](sdk::OutboundMessage message) {
                                                     return send(std::move(message));
                                                 },
                                                 [this](std::string reason) {
                                                     closeTransport(std::move(reason));
                                                 }});
        if (!protocolConnection.isOpen()) {
            reportFailure("exactly one outgoing frontend transport may be active");
            closeTransport("frontend SDK rejected the transport attachment");
            return;
        }
        try {
            if (callbacks.onBeforeTransportConnected) {
                callbacks.onBeforeTransportConnected(callbacks.verifiedLocalUnix);
            }
        } catch (...) {
            reportFailure("frontend transport authentication preparation failed");
            closeTransport("frontend transport authentication preparation failed");
            return;
        }
        online = true;
        protocolConnection.transportConnected();
        if (!protocolConnection.isTransportConnected()) {
            return;
        }
        try {
            if (callbacks.onConnected) {
                callbacks.onConnected();
            }
        } catch (...) {
        }
        try {
            if (attachmentGeneration != 0 && callbacks.onAttemptConnected) {
                callbacks.onAttemptConnected(attachmentGeneration);
            }
        } catch (...) {
        }
    }

    void ClientConnection::didReceive(CodexBackendClientSocketContext& attached, std::string frame) noexcept {
        if (context != &attached || !online) {
            return;
        }
        const sdk::ReceiveResult result = protocolConnection.receive(std::string_view(frame));
        if (!result.accepted && result.error) {
            reportFailure(result.error->message);
        }
    }

    void ClientConnection::didFail(CodexBackendClientSocketContext& attached, std::string message) noexcept {
        if (context != &attached) {
            return;
        }
        reportFailure(std::move(message));
    }

    void ClientConnection::detach(CodexBackendClientSocketContext& attached) noexcept {
        if (context != &attached) {
            return;
        }
        const bool notify = online;
        const PhysicalConnectionAttemptGate::Generation detachedGeneration = attachmentGeneration;
        online = false;
        context = nullptr;
        attachmentGeneration = 0;
        if (applicationShutdown) {
            protocolConnection.transportDisconnected();
        } else {
            protocolConnection.transportDisconnected(sdk::TransportError{"physical frontend transport disconnected", true});
        }
        if (notify && callbacks.onDisconnected) {
            try {
                callbacks.onDisconnected();
            } catch (...) {
            }
        }
        if (detachedGeneration != 0 && callbacks.onAttemptDisconnected) {
            try {
                callbacks.onAttemptDisconnected(detachedGeneration);
            } catch (...) {
            }
        }
    }

    sdk::SendResult ClientConnection::send(sdk::OutboundMessage message) noexcept {
        if (!online || context == nullptr) {
            return {sdk::SendStatus::Closed, sdk::TransportError{"frontend transport is closed", true}};
        }
        try {
            if (callbacks.onOutbound) {
                callbacks.onOutbound(message);
            }
        } catch (...) {
        }
        return context->send(std::move(message));
    }

    void ClientConnection::closeTransport(std::string reason) noexcept {
        reportFailure(std::move(reason));
        if (context != nullptr) {
            context->disconnect();
        }
    }

    void ClientConnection::reportFailure(std::string message) noexcept {
        if (failureReported) {
            return;
        }
        failureReported = true;
        try {
            if (callbacks.onFailure) {
                callbacks.onFailure(message);
            }
        } catch (...) {
        }
        try {
            if (attachmentGeneration != 0 && callbacks.onAttemptFailure) {
                callbacks.onAttemptFailure(attachmentGeneration, std::move(message));
            }
        } catch (...) {
        }
    }

} // namespace apps::codex_backend_client
