/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/FrontendWebSocketClient.h"

#include "web/websocket/SubProtocolContext.h"
#include "web/websocket/SubProtocolFactory.h"
#include "web/websocket/client/SocketContextUpgradeFactory.h"
#include "web/websocket/client/SubProtocol.h"
#include "web/websocket/client/SubProtocolFactorySelector.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <utility>

namespace apps::codex_backend_client {
    namespace sdk = ai::openai::codex::frontend::client;

    namespace {

        constexpr std::size_t MaximumInboundMessageBytes = 16U * 1024U * 1024U;
        constexpr std::uint16_t CloseUnsupportedData = 1003;
        constexpr std::uint16_t ClosePolicyViolation = 1008;
        constexpr std::uint16_t CloseUnexpectedCondition = 1011;
        FrontendWebSocketClientRuntime* installedRuntime = nullptr;

    } // namespace

    class FrontendWebSocketClientSubProtocol final : public web::websocket::client::SubProtocol {
    public:
        FrontendWebSocketClientSubProtocol(web::websocket::SubProtocolContext* context, FrontendWebSocketClientRuntime& runtime)
            : web::websocket::client::SubProtocol(context, "codex", 0, 3)
            , runtime(runtime)
            , attemptGeneration(runtime.claimAttempt(context != nullptr ? context->getSocketConnection() : nullptr)) {
        }

        ~FrontendWebSocketClientSubProtocol() override {
            detach("frontend WebSocket subprotocol destroyed");
        }

        void close(bool intentional = false) noexcept {
            if (intentional && !intentionalShutdown) {
                intentionalShutdown = true;
                try {
                    if (runtime.callbacks.onLocalShutdown) {
                        runtime.callbacks.onLocalShutdown();
                    }
                } catch (...) {
                }
            }
            closeBounded(ClosePolicyViolation, "frontend client connection closed");
        }

    private:
        void onConnected() override {
            if (attemptGeneration == 0 || !runtime.isCurrentAttempt(attemptGeneration)) {
                closeBounded(ClosePolicyViolation, "stale frontend WebSocket transport attempt");
                return;
            }
            if (runtime.active != nullptr && runtime.active != this) {
                runtime.reportAttemptFailure(attemptGeneration, "exactly one outgoing frontend transport may be active");
                closeBounded(ClosePolicyViolation, "frontend SDK already has an active transport");
                return;
            }
            protocolConnection = runtime.client.openConnection({[this](sdk::OutboundMessage message) {
                                                                    return send(std::move(message));
                                                                },
                                                                [this](std::string reason) {
                                                                    reportFailureOnce(std::move(reason));
                                                                    close();
                                                                }});
            if (!protocolConnection.isOpen()) {
                runtime.reportAttemptFailure(attemptGeneration, "frontend SDK rejected the WebSocket transport attachment");
                closeBounded(ClosePolicyViolation, "frontend SDK rejected transport");
                return;
            }
            runtime.active = this;
            try {
                if (runtime.callbacks.onBeforeTransportConnected) {
                    runtime.callbacks.onBeforeTransportConnected(false);
                }
            } catch (...) {
                runtime.reportAttemptFailure(attemptGeneration, "frontend transport authentication preparation failed");
                closeBounded(ClosePolicyViolation, "frontend transport authentication preparation failed");
                return;
            }
            protocolConnection.transportConnected();
            if (!protocolConnection.isTransportConnected()) {
                return;
            }
            notifyConnected = true;
            try {
                if (runtime.callbacks.onConnected) {
                    runtime.callbacks.onConnected();
                }
            } catch (...) {
            }
            try {
                if (runtime.callbacks.onAttemptConnected) {
                    runtime.callbacks.onAttemptConnected(attemptGeneration);
                }
            } catch (...) {
            }
        }

        void onMessageStart(int opCode) override {
            inbound.clear();
            receivedOpCode = opCode;
            if (opCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                failBounded(CloseUnsupportedData, "binary frontend messages are not supported");
            }
        }

        void onMessageData(const char* chunk, std::size_t chunkLength) override {
            if (closeStarted || receivedOpCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                return;
            }
            if (chunkLength > MaximumInboundMessageBytes || inbound.size() > MaximumInboundMessageBytes - chunkLength) {
                failBounded(ClosePolicyViolation, "frontend message exceeds the configured limit");
                return;
            }
            try {
                inbound.append(chunk, chunkLength);
            } catch (...) {
                failBounded(CloseUnexpectedCondition, "frontend message buffering failed");
            }
        }

        void onMessageEnd() override {
            if (closeStarted || receivedOpCode != web::websocket::SubProtocolContext::OpCode::TEXT) {
                inbound.clear();
                return;
            }
            const sdk::ReceiveResult result = protocolConnection.receive(std::string_view(inbound));
            inbound.clear();
            if (!result.accepted) {
                reportFailureOnce(result.error ? result.error->message : "frontend WebSocket message was rejected");
                closeBounded(ClosePolicyViolation, "invalid frontend protocol message");
            }
        }

        void onMessageError([[maybe_unused]] std::uint16_t error) override {
            failBounded(ClosePolicyViolation, "invalid frontend WebSocket message");
        }

        void onDisconnected() override {
            detach("frontend WebSocket transport disconnected");
        }

        bool onSignal([[maybe_unused]] int signal) override {
            close(true);
            return false;
        }

        sdk::SendResult send(sdk::OutboundMessage message) noexcept {
            if (closeStarted || runtime.active != this) {
                return {sdk::SendStatus::Closed, sdk::TransportError{"frontend WebSocket transport is closed", true}};
            }
            try {
                sendMessage(std::move(message.compactJson));
                return {sdk::SendStatus::Accepted, std::nullopt};
            } catch (...) {
                return {sdk::SendStatus::Failed, sdk::TransportError{"frontend WebSocket send failed", true}};
            }
        }

        void closeBounded(std::uint16_t status, const char* reason) noexcept {
            if (closeStarted) {
                return;
            }
            closeStarted = true;
            inbound.clear();
            try {
                sendClose(status, reason, std::char_traits<char>::length(reason));
            } catch (...) {
            }
        }

        void failBounded(std::uint16_t status, const char* reason) noexcept {
            reportFailureOnce(reason);
            closeBounded(status, reason);
        }

        void reportFailureOnce(std::string message) noexcept {
            if (failureReported) {
                return;
            }
            failureReported = true;
            runtime.reportAttemptFailure(attemptGeneration, std::move(message));
        }

        void detach(std::string reason) noexcept {
            if (detached) {
                return;
            }
            detached = true;
            inbound.clear();
            if (intentionalShutdown) {
                protocolConnection.transportDisconnected();
            } else {
                protocolConnection.transportDisconnected(sdk::TransportError{std::move(reason), true});
            }
            if (runtime.active == this) {
                runtime.active = nullptr;
            }
            if (notifyConnected && runtime.callbacks.onDisconnected) {
                notifyConnected = false;
                try {
                    runtime.callbacks.onDisconnected();
                } catch (...) {
                }
            }
            if (attemptGeneration != 0 && runtime.isCurrentAttempt(attemptGeneration)) {
                runtime.abandonAttempt(attemptGeneration);
                try {
                    if (runtime.callbacks.onAttemptDisconnected) {
                        runtime.callbacks.onAttemptDisconnected(attemptGeneration);
                    }
                } catch (...) {
                }
            }
        }

        FrontendWebSocketClientRuntime& runtime;
        const std::uint64_t attemptGeneration;
        sdk::Connection protocolConnection;
        std::string inbound;
        int receivedOpCode = 0;
        bool notifyConnected = false;
        bool closeStarted = false;
        bool detached = false;
        bool intentionalShutdown = false;
        bool failureReported = false;
    };

    namespace {

        class FrontendWebSocketClientFactory final : public web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol> {
        public:
            FrontendWebSocketClientFactory()
                : web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>("codex") {
            }

        private:
            web::websocket::client::SubProtocol* create(web::websocket::SubProtocolContext* context) override {
                if (installedRuntime == nullptr) {
                    return nullptr;
                }
                return new FrontendWebSocketClientSubProtocol(context, *installedRuntime);
            }
        };

        web::websocket::SubProtocolFactory<web::websocket::client::SubProtocol>* createFrontendWebSocketClientFactory() {
            return new FrontendWebSocketClientFactory();
        }

    } // namespace

    FrontendWebSocketClientRuntime::FrontendWebSocketClientRuntime(sdk::Client& client, FrontendWebSocketClientCallbacks callbacks)
        : client(client)
        , callbacks(std::move(callbacks)) {
    }

    FrontendWebSocketClientRuntime::~FrontendWebSocketClientRuntime() {
        uninstall();
    }

    bool FrontendWebSocketClientRuntime::install() noexcept {
        if (installedRuntime != nullptr && installedRuntime != this) {
            return false;
        }
        installedRuntime = this;
        installed = true;
        return true;
    }

    void FrontendWebSocketClientRuntime::uninstall() noexcept {
        shutdown();
        if (installedRuntime == this) {
            installedRuntime = nullptr;
        }
        installed = false;
    }

    void FrontendWebSocketClientRuntime::shutdown() noexcept {
        if (active != nullptr) {
            active->close(true);
        }
    }

    bool FrontendWebSocketClientRuntime::connected() const noexcept {
        return active != nullptr;
    }

    void FrontendWebSocketClientRuntime::reportFailure(std::string message) noexcept {
        try {
            if (callbacks.onFailure) {
                callbacks.onFailure(std::move(message));
            }
        } catch (...) {
        }
    }

    bool FrontendWebSocketClientRuntime::prepareAttempt(const std::uint64_t generation) noexcept {
        if (generation == 0 || preparedGeneration != 0 || active != nullptr) {
            return false;
        }
        preparedGeneration = generation;
        preparedTransport = nullptr;
        return true;
    }

    bool FrontendWebSocketClientRuntime::bindAttemptTransport(const std::uint64_t generation,
                                                              const core::socket::stream::SocketConnection* transport) noexcept {
        if (generation == 0 || transport == nullptr || preparedGeneration != generation || active != nullptr) {
            return false;
        }
        if (preparedTransport != nullptr && preparedTransport != transport) {
            return false;
        }
        preparedTransport = transport;
        return true;
    }

    void FrontendWebSocketClientRuntime::abandonAttempt(const std::uint64_t generation) noexcept {
        if (preparedGeneration == generation) {
            preparedGeneration = 0;
            preparedTransport = nullptr;
        }
    }

    bool FrontendWebSocketClientRuntime::isCurrentAttempt(const std::uint64_t generation) const noexcept {
        return generation != 0 && preparedGeneration == generation;
    }

    void FrontendWebSocketClientRuntime::reportAttemptFailure(const std::uint64_t generation, std::string message) noexcept {
        if (!isCurrentAttempt(generation)) {
            return;
        }
        try {
            if (callbacks.onFailure) {
                callbacks.onFailure(message);
            }
        } catch (...) {
        }
        try {
            if (callbacks.onAttemptFailure) {
                callbacks.onAttemptFailure(generation, std::move(message));
            }
        } catch (...) {
        }
    }

    std::uint64_t FrontendWebSocketClientRuntime::claimAttempt(const core::socket::stream::SocketConnection* transport) noexcept {
        if (transport == nullptr) {
            return 0;
        }
        if (preparedGeneration != 0) {
            return preparedTransport == transport ? preparedGeneration : 0;
        }
        if (callbacks.onAttemptConnected || callbacks.onAttemptDisconnected || callbacks.onAttemptFailure) {
            // Generation-aware application composition must prepare every
            // physical upgrade explicitly. A subprotocol arriving after its
            // HTTP attempt was abandoned is stale; never manufacture an
            // implicit generation that could attach it to the SDK.
            return 0;
        }
        if (nextImplicitGeneration == 0) {
            return 0;
        }
        preparedGeneration = nextImplicitGeneration;
        if (nextImplicitGeneration == std::numeric_limits<std::uint64_t>::max()) {
            nextImplicitGeneration = 0;
        } else {
            ++nextImplicitGeneration;
        }
        preparedTransport = transport;
        return preparedGeneration;
    }

    void linkFrontendWebSocketClient() noexcept {
        web::websocket::client::SocketContextUpgradeFactory::link();
        web::websocket::client::SubProtocolFactorySelector::link("codex", createFrontendWebSocketClientFactory);
    }

} // namespace apps::codex_backend_client
