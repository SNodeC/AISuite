/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/Security.h"
#include "ai/openai/codex/frontend/internal/transport/JsonLineFramer.h"
#include "apps/codex-backend/FrontendStreamSocketContext.h"
#include "apps/codex-backend/FrontendStreamSocketContextFactory.h"
#include "apps/codex-backend/UnixPeerCredentials.h"
#include "core/SNodeC.h"
#include "core/socket/SocketAddress.h"
#include "core/socket/State.h"
#include "core/socket/stream/SocketConnection.h"
#include "core/socket/stream/SocketContext.h"
#include "core/socket/stream/SocketContextFactory.h"
#include "core/timer/Timer.h"
#include "net/in/SocketAddress.h"
#include "net/in/stream/legacy/SocketClient.h"
#include "net/in/stream/legacy/SocketServer.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "net/in/stream/tls/SocketClient.h"
#include "net/in/stream/tls/SocketServer.h"
#endif
#include "net/in6/SocketAddress.h"
#include "net/in6/stream/legacy/SocketClient.h"
#include "net/in6/stream/legacy/SocketServer.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "net/in6/stream/tls/SocketClient.h"
#include "net/in6/stream/tls/SocketServer.h"
#endif
#if defined(AISUITE_TEST_CODEX_FRONTEND_RFCOMM)
#include "net/rc/stream/legacy/SocketServer.h"
#include "net/rc/stream/tls/SocketServer.h"
#endif
#include "net/un/SocketAddress.h"
#include "net/un/stream/legacy/SocketClient.h"
#include "net/un/stream/legacy/SocketServer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace apps::codex_backend::detail {

    struct FrontendStreamSocketContextTestAccess {
        static void connect(FrontendStreamSocketContext& context) {
            context.onConnected();
        }

        static std::size_t receive(FrontendStreamSocketContext& context) {
            return context.onReceivedFromPeer();
        }

        static void disconnect(FrontendStreamSocketContext& context) {
            context.onDisconnected();
        }

        static ai::openai::codex::frontend::OutboundDeliveryStatus
        send(FrontendStreamSocketContext& context, const ai::openai::codex::frontend::OutboundMessage& message) {
            return context.send(message);
        }

        static void clearLifetime(FrontendStreamSocketContext& context) {
            context.lifetime.reset();
        }
    };

} // namespace apps::codex_backend::detail

namespace {

    namespace app = apps::codex_backend;
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;
    namespace jsonl = ai::openai::codex::frontend::internal::transport;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;

    constexpr std::string_view BearerToken = "a1-7b-native-loopback-token";
    const utils::Timeval EstablishedInactivityProbeTimeout(0.25);
    const utils::Timeval EstablishedIdleDuration(0.75);

    class ScopedClogCapture {
    public:
        ScopedClogCapture()
            : previous(std::clog.rdbuf(output.rdbuf())) {
        }

        ScopedClogCapture(const ScopedClogCapture&) = delete;
        ScopedClogCapture& operator=(const ScopedClogCapture&) = delete;

        ~ScopedClogCapture() {
            std::clog.rdbuf(previous);
        }

        [[nodiscard]] std::string str() const {
            return output.str();
        }

    private:
        std::ostringstream output;
        std::streambuf* previous;
    };

    enum class ClientKind : std::size_t { Unix, Ipv4, Ipv6, Tls, Count };

    constexpr std::size_t clientIndex(ClientKind kind) noexcept {
        return static_cast<std::size_t>(kind);
    }

    std::vector<ClientKind> activeClientKinds(bool ipv6LoopbackAvailable) {
        std::vector<ClientKind> kinds{ClientKind::Unix, ClientKind::Ipv4};
        if (ipv6LoopbackAvailable) {
            kinds.push_back(ClientKind::Ipv6);
        }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        kinds.push_back(ClientKind::Tls);
#endif
        return kinds;
    }

    std::string_view clientName(ClientKind kind) noexcept {
        switch (kind) {
            case ClientKind::Unix:
                return "Unix JSONL";
            case ClientKind::Ipv4:
                return "IPv4 JSONL";
            case ClientKind::Ipv6:
                return "IPv6 JSONL";
            case ClientKind::Tls:
                return "TLS JSONL";
            case ClientKind::Count:
                break;
        }
        return "unknown native transport";
    }

    frontend::FrontendTransportKind transportKind(ClientKind kind) noexcept {
        switch (kind) {
            case ClientKind::Unix:
                return frontend::FrontendTransportKind::Unix;
            case ClientKind::Ipv4:
                return frontend::FrontendTransportKind::Ipv4;
            case ClientKind::Ipv6:
                return frontend::FrontendTransportKind::Ipv6;
            case ClientKind::Tls:
                return frontend::FrontendTransportKind::TcpTls;
            case ClientKind::Count:
                break;
        }
        return frontend::FrontendTransportKind::InMemory;
    }

    bool hasCapability(const std::vector<frontend::FrontendCapability>& capabilities, frontend::FrontendCapability expected) {
        return std::find(capabilities.begin(), capabilities.end(), expected) != capabilities.end();
    }

    frontend::AuthenticationResult remoteAuthentication(const frontend::AuthenticationCredential& credential) {
        const auto* bearer = std::get_if<frontend::BearerCredential>(&credential);
        if (bearer == nullptr || bearer->token != BearerToken) {
            return frontend::AuthenticationFailure{bearer == nullptr ? frontend::AuthenticationFailureCode::AuthenticationRequired
                                                                     : frontend::AuthenticationFailureCode::AuthenticationFailed};
        }
        return frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
            "native-loopback",
            std::vector<frontend::FrontendScope>(frontend::DefaultRemoteScopes.begin(), frontend::DefaultRemoteScopes.end()),
            "default_remote",
            false}};
    }

    struct ClientObservation {
        std::size_t connected = 0;
        std::size_t disconnected = 0;
        std::size_t welcome = 0;
        std::size_t snapshot = 0;
        std::size_t syncComplete = 0;
        std::size_t protocolErrors = 0;
        bool advertisedMultiTransport = false;
        std::size_t availableMethods = 0;
        std::size_t permittedMethods = 0;
    };

    struct IntegrationState {
        IntegrationState(tests::support::TestResult& result, std::size_t expectedClients)
            : result(result)
            , expectedClients(expectedClients) {
        }

        void listenFailed(ClientKind kind, const core::socket::State& state) {
            ++listenFailures;
            result.expectTrue(false, std::string(clientName(kind)) + " listener binds: " + state.what());
            core::SNodeC::stop();
        }

        void connectResult(ClientKind kind, const core::socket::State& state) {
            if (state == core::socket::State::OK) {
                ++connectSuccesses;
            } else {
                ++connectFailures;
                result.expectTrue(false, std::string(clientName(kind)) + " client connects: " + state.what());
                core::SNodeC::stop();
            }
        }

        void connected(ClientKind kind) {
            ++clients[clientIndex(kind)].connected;
        }

        void disconnected(ClientKind kind) {
            ++clients[clientIndex(kind)].disconnected;
        }

        void decoded(ClientKind kind, frontend::ServerMessage message) {
            ClientObservation& observation = clients[clientIndex(kind)];
            if (const auto* welcome = std::get_if<frontend::Welcome>(&message)) {
                ++observation.welcome;
                observation.availableMethods = welcome->availableMethods ? welcome->availableMethods->size() : 0;
                observation.permittedMethods = welcome->permittedMethods ? welcome->permittedMethods->size() : 0;
                observation.advertisedMultiTransport = welcome->capabilities && hasCapability(welcome->capabilities->implemented,
                                                                                              frontend::FrontendCapability::MultiTransport);
            } else if (std::holds_alternative<frontend::Snapshot>(message)) {
                ++observation.snapshot;
            } else if (std::holds_alternative<frontend::SyncComplete>(message)) {
                if (observation.syncComplete == 0) {
                    ++completedClients;
                }
                ++observation.syncComplete;
                if (completedClients == expectedClients) {
                    idleTimer.emplace(core::timer::Timer::singleshotTimer(
                        [this] {
                            completed = true;
                            idleEstablishedSurvived = std::all_of(clients.begin(), clients.end(), [](const ClientObservation& client) {
                                return client.disconnected == 0;
                            });
                            core::SNodeC::stop();
                        },
                        EstablishedIdleDuration));
                }
            } else if (std::holds_alternative<frontend::ProtocolErrorMessage>(message)) {
                ++observation.protocolErrors;
                result.expectTrue(false, std::string(clientName(kind)) + " receives no protocol error");
                core::SNodeC::stop();
            }
        }

        void decodeFailed(ClientKind kind, std::string message) {
            ++decodeFailures;
            result.expectTrue(false, std::string(clientName(kind)) + " decode failed: " + std::move(message));
            core::SNodeC::stop();
        }

        void recordAuthenticatedPeer(const frontend::FrontendPeerContext& peer) {
            authenticatedPeers.push_back(peer);
        }

        tests::support::TestResult& result;
        std::array<ClientObservation, clientIndex(ClientKind::Count)> clients{};
        std::vector<frontend::FrontendPeerContext> authenticatedPeers;
        std::optional<frontend::FrontendPeerContext> verifiedUnixPeer;
        std::size_t listenSuccesses = 0;
        std::size_t listenFailures = 0;
        std::size_t connectSuccesses = 0;
        std::size_t connectFailures = 0;
        std::size_t decodeFailures = 0;
        std::size_t completedClients = 0;
        std::size_t expectedClients = 0;
        std::optional<core::timer::Timer> idleTimer;
        bool topologyObservedBeforeClients = false;
        bool completed = false;
        bool idleEstablishedSurvived = false;
        bool timedOut = false;
    };

    class TimeoutProbeSocketAddress final : public core::socket::SocketAddress {
    public:
        std::string toString(bool = true) const override {
            return "timeout-probe";
        }
    };

    class TimeoutProbeSocketConnection final : public core::socket::stream::SocketConnection {
    public:
        explicit TimeoutProbeSocketConnection(std::string input)
            : SocketConnection(-1, 1, "frontend-timeout-probe", nullptr)
            , input(std::move(input)) {
        }

        ~TimeoutProbeSocketConnection() override = default;

        int getFd() const override {
            return -1;
        }

        void sendToPeer(const char*, std::size_t) override {
        }

        core::socket::stream::QueueResult trySendToPeer(const char*, std::size_t chunkLength) override {
            ++trySendCalls;
            lastTrySendBytes = chunkLength;
            return nextQueueResult;
        }

        bool streamToPeer(core::pipe::Source*) override {
            return false;
        }

        void streamEof() override {
        }

        std::size_t readFromPeer(char* chunk, std::size_t chunkLength) override {
            const std::size_t size = std::min(chunkLength, input.size() - offset);
            std::copy_n(input.data() + offset, size, chunk);
            offset += size;
            return size;
        }

        void shutdownRead() override {
        }

        void shutdownWrite() override {
        }

        const core::socket::SocketAddress& getBindAddress() const override {
            return address;
        }

        const core::socket::SocketAddress& getLocalAddress() const override {
            return address;
        }

        const core::socket::SocketAddress& getRemoteAddress() const override {
            return address;
        }

        void close() override {
        }

        void setTimeout(const utils::Timeval& timeout) override {
            readTimeout = timeout;
            writeTimeout = timeout;
        }

        void setReadTimeout(const utils::Timeval& timeout) override {
            readTimeout = timeout;
            ++readTimeoutChanges;
        }

        void setWriteTimeout(const utils::Timeval& timeout) override {
            writeTimeout = timeout;
            ++writeTimeoutChanges;
        }

        std::size_t getTotalSent() const override {
            return totalSent;
        }

        std::size_t getTotalQueued() const override {
            return totalQueued;
        }

        std::size_t getTotalRead() const override {
            return offset;
        }

        std::size_t getTotalProcessed() const override {
            return offset;
        }

        utils::Timeval readTimeout = EstablishedInactivityProbeTimeout;
        utils::Timeval writeTimeout = EstablishedInactivityProbeTimeout;
        std::size_t readTimeoutChanges = 0;
        std::size_t writeTimeoutChanges = 0;
        core::socket::stream::QueueResult nextQueueResult = core::socket::stream::QueueResult::Queued;
        std::size_t totalSent = 0;
        std::size_t totalQueued = 0;
        std::size_t trySendCalls = 0;
        std::size_t lastTrySendBytes = 0;

    private:
        TimeoutProbeSocketAddress address;
        std::string input;
        std::size_t offset = 0;
    };

    void expectEstablishedTimeoutTransition(tests::support::TestResult& result) {
        frontend::Hello hello;
        hello.authentication = frontend::AuthenticationCredential{frontend::BearerCredential{std::string(BearerToken)}};
        const auto encoded = frontend::Codec::serializeClient(frontend::ClientMessage{std::move(hello)});
        result.expectTrue(static_cast<bool>(encoded), "the timeout transition probe serializes a frontend Hello");
        if (!encoded) {
            return;
        }

        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        FakeBackendCore backend({}, transport);
        frontend::FrontendServiceOptions options;
        options.authenticator = [](const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential& credential) {
            return remoteAuthentication(credential);
        };
        options.timerScheduler = [](std::uint64_t, std::function<void()>) {
            return frontend::FrontendTimerCancellation{[] {
            }};
        };
        frontend::FrontendService service(backend, std::move(options));

        TimeoutProbeSocketConnection socketConnection(encoded.value() + '\n');
        frontend::FrontendPeerContext peer;
        peer.transport = frontend::FrontendTransportKind::Ipv4;
        app::FrontendStreamSocketContext context(&socketConnection, service, std::move(peer), {});
        app::detail::FrontendStreamSocketContextTestAccess::connect(context);

        result.expectTrue(socketConnection.readTimeoutChanges == 0 && socketConnection.writeTimeoutChanges == 0 &&
                              socketConnection.readTimeout == EstablishedInactivityProbeTimeout &&
                              socketConnection.writeTimeout == EstablishedInactivityProbeTimeout &&
                              service.unauthenticatedConnectionCount() == 1,
                          "raw stream acceptance preserves the finite pre-authentication read and write inactivity timeouts");

        const std::size_t received = app::detail::FrontendStreamSocketContextTestAccess::receive(context);
        result.expectTrue(received == encoded.value().size() + 1 && service.authenticatedConnectionCount() == 1 &&
                              socketConnection.readTimeoutChanges == 1 && socketConnection.readTimeout == utils::Timeval({0, 0}),
                          "successful frontend establishment disables the SNode.C read inactivity timeout exactly once");
        result.expectTrue(socketConnection.writeTimeoutChanges == 1 && socketConnection.writeTimeout == utils::Timeval({0, 0}),
                          "successful frontend establishment disables the SNode.C write inactivity timeout exactly once");

        const frontend::OutboundMessage deliveryProbe{
            frontend::ServerMessage{frontend::SyncComplete{}}, "native-delivery-probe", std::string_view("native-delivery-probe").size()};
        socketConnection.totalQueued = app::DEFAULT_MAXIMUM_OUTBOUND_BYTES - 1;
        const frontend::OutboundDeliveryStatus backpressured =
            app::detail::FrontendStreamSocketContextTestAccess::send(context, deliveryProbe);
        result.expectTrue(backpressured == frontend::OutboundDeliveryStatus::Backpressured && socketConnection.trySendCalls == 0,
                          "a full native writer retains the ServerCore head without rebuilding or submitting its frame");

        socketConnection.totalQueued = 0;
        const frontend::OutboundDeliveryStatus accepted =
            app::detail::FrontendStreamSocketContextTestAccess::send(context, deliveryProbe);
        result.expectTrue(accepted == frontend::OutboundDeliveryStatus::Accepted && socketConnection.trySendCalls == 1 &&
                              socketConnection.lastTrySendBytes == deliveryProbe.serializedBytes + 1,
                          "native delivery submits and accepts the exact JSONL frame once bounded writer capacity is available");

        socketConnection.nextQueueResult = core::socket::stream::QueueResult::WouldExceedLimit;
        const frontend::OutboundDeliveryStatus permanentlyRejected =
            app::detail::FrontendStreamSocketContextTestAccess::send(context, deliveryProbe);
        result.expectTrue(permanentlyRejected == frontend::OutboundDeliveryStatus::Closed && socketConnection.trySendCalls == 2,
                          "an empty writer rejecting a frame is classified as a terminal transport-bound mismatch");

        app::detail::FrontendStreamSocketContextTestAccess::disconnect(context);
    }

    void expectTerminalSendCloseDiagnostics(tests::support::TestResult& result) {
        frontend::Hello hello;
        hello.authentication = frontend::AuthenticationCredential{frontend::BearerCredential{std::string(BearerToken)}};
        const auto encoded = frontend::Codec::serializeClient(frontend::ClientMessage{std::move(hello)});
        result.expectTrue(static_cast<bool>(encoded), "the transport close diagnostic fixture serializes a frontend Hello");
        if (!encoded) {
            return;
        }

        const auto authenticate = [](const frontend::FrontendPeerContext&,
                                     const frontend::AuthenticationCredential& credential) {
            return remoteAuthentication(credential);
        };

        const auto exerciseSend = [&](std::function<void(frontend::FrontendServiceOptions&)> configureService,
                                      std::function<void(TimeoutProbeSocketConnection&, app::FrontendStreamSocketContext&)> configureSocket,
                                      const frontend::OutboundMessage& message) {
            const auto transport = std::make_shared<tests::codex::FakeTransportState>();
            FakeBackendCore backend({}, transport);
            frontend::FrontendServiceOptions options;
            options.authenticator = authenticate;
            options.timerScheduler = [](std::uint64_t, std::function<void()>) {
                return frontend::FrontendTimerCancellation{[] {
                }};
            };
            configureService(options);
            frontend::FrontendService service(backend, std::move(options));

            TimeoutProbeSocketConnection socketConnection(encoded.value() + '\n');
            frontend::FrontendPeerContext peer;
            peer.transport = frontend::FrontendTransportKind::Ipv4;
            app::FrontendStreamSocketContext context(&socketConnection, service, std::move(peer), {});
            app::detail::FrontendStreamSocketContextTestAccess::connect(context);
            (void) app::detail::FrontendStreamSocketContextTestAccess::receive(context);
            configureSocket(socketConnection, context);
            return app::detail::FrontendStreamSocketContextTestAccess::send(context, message);
        };

        const frontend::OutboundMessage probe{
            frontend::ServerMessage{frontend::SyncComplete{}}, "native-delivery-probe", std::string_view("native-delivery-probe").size()};

        {
            ScopedClogCapture capture;
            const frontend::OutboundMessage oversized{
                frontend::ServerMessage{frontend::SyncComplete{}},
                std::string(app::DEFAULT_MAXIMUM_OUTBOUND_BYTES, 'x'),
                app::DEFAULT_MAXIMUM_OUTBOUND_BYTES};
            const frontend::OutboundDeliveryStatus status = exerciseSend(
                [](frontend::FrontendServiceOptions&) {},
                [](TimeoutProbeSocketConnection&, app::FrontendStreamSocketContext&) {},
                oversized);
            const std::string diagnostics = capture.str();
            result.expectTrue(
                status == frontend::OutboundDeliveryStatus::Closed &&
                    diagnostics.find("reason=frame-over-adapter-limit") != std::string::npos &&
                    diagnostics.find("frame-bytes=" + std::to_string(app::DEFAULT_MAXIMUM_OUTBOUND_BYTES + 1)) != std::string::npos &&
                    diagnostics.find("adapter-max-bytes=" + std::to_string(app::DEFAULT_MAXIMUM_OUTBOUND_BYTES)) != std::string::npos,
                "oversized native frames emit a distinct bounded close reason with exact writer limits");
        }

        {
            ScopedClogCapture capture;
            const frontend::OutboundDeliveryStatus status = exerciseSend(
                [](frontend::FrontendServiceOptions&) {},
                [](TimeoutProbeSocketConnection& socketConnection, app::FrontendStreamSocketContext&) {
                    socketConnection.nextQueueResult = core::socket::stream::QueueResult::WouldExceedLimit;
                },
                probe);
            const std::string diagnostics = capture.str();
            result.expectTrue(status == frontend::OutboundDeliveryStatus::Closed &&
                                  diagnostics.find("reason=empty-writer-rejected") != std::string::npos &&
                                  diagnostics.find("queue-result=would-exceed-limit") != std::string::npos,
                              "an empty native writer rejection emits the dedicated empty-writer diagnostic");
        }

        {
            ScopedClogCapture capture;
            const frontend::OutboundDeliveryStatus status = exerciseSend(
                [](frontend::FrontendServiceOptions&) {},
                [](TimeoutProbeSocketConnection& socketConnection, app::FrontendStreamSocketContext&) {
                    socketConnection.nextQueueResult = core::socket::stream::QueueResult::Closed;
                },
                probe);
            const std::string diagnostics = capture.str();
            result.expectTrue(status == frontend::OutboundDeliveryStatus::Closed &&
                                  diagnostics.find("reason=writer-closed") != std::string::npos &&
                                  diagnostics.find("queue-result=closed") != std::string::npos,
                              "a closed native writer emits the dedicated writer-closed diagnostic");
        }

        {
            ScopedClogCapture capture;
            const frontend::OutboundDeliveryStatus status = exerciseSend(
                [](frontend::FrontendServiceOptions&) {},
                [](TimeoutProbeSocketConnection& socketConnection, app::FrontendStreamSocketContext&) {
                    socketConnection.nextQueueResult = core::socket::stream::QueueResult::ShutdownInProgress;
                },
                probe);
            const std::string diagnostics = capture.str();
            result.expectTrue(status == frontend::OutboundDeliveryStatus::Closed &&
                                  diagnostics.find("reason=writer-shutdown") != std::string::npos &&
                                  diagnostics.find("queue-result=shutdown-in-progress") != std::string::npos,
                              "a shutting-down native writer emits the dedicated writer-shutdown diagnostic");
        }

        {
            ScopedClogCapture capture;
            const frontend::OutboundDeliveryStatus status = exerciseSend(
                [](frontend::FrontendServiceOptions&) {},
                [](TimeoutProbeSocketConnection& socketConnection, app::FrontendStreamSocketContext& context) {
                    socketConnection.totalQueued = app::DEFAULT_MAXIMUM_OUTBOUND_BYTES - 1;
                    app::detail::FrontendStreamSocketContextTestAccess::clearLifetime(context);
                },
                probe);
            const std::string diagnostics = capture.str();
            result.expectTrue(status == frontend::OutboundDeliveryStatus::Closed &&
                                  diagnostics.find("reason=delivery-retry-scheduling-failed") != std::string::npos &&
                                  diagnostics.find("frontend stream retry state: reason=socket-missing") != std::string::npos,
                              "a terminal retry failure emits both the terminal send-close reason and the retry-state cause");
        }
    }

    class FrontendProtocolClientContext final : public core::socket::stream::SocketContext {
    public:
        FrontendProtocolClientContext(core::socket::stream::SocketConnection* connection, IntegrationState& state, ClientKind kind)
            : core::socket::stream::SocketContext(connection)
            , state(state)
            , kind(kind) {
        }

    private:
        void onConnected() override {
            state.connected(kind);
            frontend::Hello hello;
            hello.capabilities = std::vector{frontend::FrontendCapability::MethodDiscovery, frontend::FrontendCapability::SecurityScopes};
            if (kind != ClientKind::Unix || !app::unixPeerCredentialsSupported()) {
                hello.authentication = frontend::AuthenticationCredential{frontend::BearerCredential{std::string(BearerToken)}};
            }
            const auto encoded = frontend::Codec::serializeClient(frontend::ClientMessage{std::move(hello)});
            if (!encoded) {
                state.decodeFailed(kind, "Hello serialization failed: " + encoded.error().message);
                return;
            }
            std::string frame = encoded.value();
            frame.push_back('\n');
            sendToPeer(frame.data(), frame.size());
        }

        void onDisconnected() override {
            state.disconnected(kind);
        }

        std::size_t onReceivedFromPeer() override {
            std::array<char, 16 * 1024> bytes{};
            const std::size_t size = readFromPeer(bytes.data(), bytes.size());
            receiveBuffer.append(bytes.data(), size);
            while (true) {
                const std::size_t delimiter = receiveBuffer.find('\n');
                if (delimiter == std::string::npos) {
                    break;
                }
                std::string frame = receiveBuffer.substr(0, delimiter);
                receiveBuffer.erase(0, delimiter + 1);
                if (!frame.empty() && frame.back() == '\r') {
                    frame.pop_back();
                }
                const auto decoded = frontend::Codec::decodeServer(std::string_view(frame));
                if (!decoded) {
                    state.decodeFailed(kind, decoded.error().message);
                    return size;
                }
                state.decoded(kind, decoded.value());
            }
            return size;
        }

        bool onSignal([[maybe_unused]] int signum) override {
            return true;
        }

        IntegrationState& state;
        ClientKind kind;
        std::string receiveBuffer;
    };

    class FrontendProtocolClientFactory final : public core::socket::stream::SocketContextFactory {
    public:
        FrontendProtocolClientFactory(IntegrationState& state, ClientKind kind)
            : state(state)
            , kind(kind) {
        }

        core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* connection) override {
            return new FrontendProtocolClientContext(connection, state, kind);
        }

    private:
        IntegrationState& state;
        ClientKind kind;
    };

    std::string unixSocketPath() {
        return "/tmp/aisuite-a1-7b-native-" + std::to_string(::getpid()) + ".sock";
    }

    bool expectedIpv6ResolverAbsence(int error) noexcept {
        if (error == EAI_NONAME || error == EAI_FAMILY) {
            return true;
        }
#if defined(EAI_ADDRFAMILY)
        if (error == EAI_ADDRFAMILY) {
            return true;
        }
#endif
        return false;
    }

    bool expectedIpv6SocketAbsence(int error) noexcept {
        return error == EAFNOSUPPORT || error == EPROTONOSUPPORT || error == EADDRNOTAVAIL;
    }

    bool ipv6LoopbackAvailable(tests::support::TestResult& result) {
        try {
            net::in6::SocketAddress address("::1", 0);
            address.init({.aiFlags = AI_PASSIVE | AI_NUMERICHOST, .aiSockType = SOCK_STREAM, .aiProtocol = IPPROTO_TCP});
        } catch (const core::socket::SocketAddress::BadSocketAddress& error) {
            if (expectedIpv6ResolverAbsence(error.getErrnum())) {
                std::cout << "IPv6 live loopback not run: SNode.C AI_ADDRCONFIG cannot resolve ::1 without a configured "
                             "non-loopback IPv6 address; IPv6 build, configuration, and factory coverage remains active.\n";
                return false;
            }
            result.expectTrue(false, std::string("unexpected IPv6 resolver failure: ") + error.what());
            return false;
        }

        const int descriptor = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
        if (descriptor < 0) {
            if (expectedIpv6SocketAbsence(errno)) {
                std::cout << "IPv6 live loopback not run: the platform has no usable IPv6 stream socket; IPv6 build, configuration, "
                             "and factory coverage remains active.\n";
                return false;
            }
            result.expectTrue(false, "unexpected IPv6 socket probe failure");
            return false;
        }

        sockaddr_in6 loopback{};
        loopback.sin6_family = AF_INET6;
        loopback.sin6_addr = in6addr_loopback;
        loopback.sin6_port = 0;
        const int bindResult = ::bind(descriptor, reinterpret_cast<const sockaddr*>(&loopback), sizeof(loopback));
        const int bindError = errno;
        ::close(descriptor);
        if (bindResult == 0) {
            return true;
        }
        if (expectedIpv6SocketAbsence(bindError)) {
            std::cout << "IPv6 live loopback not run: the platform cannot bind ::1; IPv6 build, configuration, and factory coverage "
                         "remains active.\n";
            return false;
        }
        result.expectTrue(false, "unexpected IPv6 bind probe failure");
        return false;
    }

    void expectTransportFacts(tests::support::TestResult& result) {
        result.expectTrue(app::isJsonLineStreamTransport(frontend::FrontendTransportKind::Unix) &&
                              app::isJsonLineStreamTransport(frontend::FrontendTransportKind::Ipv4) &&
                              app::isJsonLineStreamTransport(frontend::FrontendTransportKind::Ipv6) &&
                              app::isJsonLineStreamTransport(frontend::FrontendTransportKind::TcpTls) &&
                              app::isJsonLineStreamTransport(frontend::FrontendTransportKind::Rfcomm) &&
                              app::isJsonLineStreamTransport(frontend::FrontendTransportKind::RfcommTls) &&
                              !app::isJsonLineStreamTransport(frontend::FrontendTransportKind::WebSocket) &&
                              !app::isJsonLineStreamTransport(frontend::FrontendTransportKind::WebSocketTls),
                          "Unix, IP, TLS, and RFCOMM use one JSONL stream boundary while WebSocket does not");
        result.expectTrue(!app::isEncryptedTransport(frontend::FrontendTransportKind::Ipv4) &&
                              !app::isEncryptedTransport(frontend::FrontendTransportKind::Ipv6) &&
                              app::isEncryptedTransport(frontend::FrontendTransportKind::TcpTls) &&
                              !app::isEncryptedTransport(frontend::FrontendTransportKind::Rfcomm) &&
                              app::isEncryptedTransport(frontend::FrontendTransportKind::RfcommTls),
                          "encryption metadata is fixed by the successfully bound native listener family");

        std::vector<std::string> frames;
        jsonl::JsonLineFramer framer(64);
        const auto first = framer.push("{\"one\":1}\r", [&frames](std::string frame) {
            frames.push_back(std::move(frame));
        });
        const auto second = framer.push("\n{\"two\":2}\n", [&frames](std::string frame) {
            frames.push_back(std::move(frame));
        });
        result.expectTrue(first == jsonl::JsonLineFramer::Result::Accepted && second == jsonl::JsonLineFramer::Result::Accepted &&
                              frames == std::vector<std::string>{"{\"one\":1}", "{\"two\":2}"},
                          "the shared native stream framer preserves fragmentation, CRLF tolerance, and multiple frames per read");
    }

    void expectTransportCapabilityIsNotAdvertised(tests::support::TestResult& result) {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        FakeBackendCore backend({}, transport);
        frontend::FrontendServiceOptions options;
        options.maxUnauthenticatedConnections = 8;
        options.timerScheduler = [](std::uint64_t, std::function<void()>) {
            return frontend::FrontendTimerCancellation{[] {
            }};
        };
        frontend::FrontendService service(backend, std::move(options));
        std::vector<frontend::FrontendConnection> connections;
        for (std::size_t index = 0; index < 6; ++index) {
            frontend::FrontendPeerContext peer;
            peer.transport = frontend::FrontendTransportKind::Unix;
            connections.push_back(service.openConnection(std::move(peer), {{}, {}}));
        }
        result.expectTrue(service.connectionCount() == 6 && service.implementedCapabilities().size() == 15 &&
                              !hasCapability(service.implementedCapabilities(), frontend::FrontendCapability::MultiTransport),
                          "connections do not affect the fourteen static mechanisms, SDK product truth, or topology capability");
        for (frontend::FrontendConnection& connection : connections) {
            connection.close();
        }
    }

#if defined(AISUITE_TEST_CODEX_FRONTEND_RFCOMM)
    void expectRfcommComposition(tests::support::TestResult& result, frontend::FrontendService& service) {
        app::FrontendStreamSocketContextFactory rfcommFactory(
            service, app::FrontendStreamSocketContextFactoryOptions{frontend::FrontendTransportKind::Rfcomm, {}, {}});
        app::FrontendStreamSocketContextFactory rfcommTlsFactory(
            service, app::FrontendStreamSocketContextFactoryOptions{frontend::FrontendTransportKind::RfcommTls, {}, {}});
        result.expectTrue(rfcommFactory.serviceIdentity() == &service && rfcommTlsFactory.serviceIdentity() == &service &&
                              rfcommFactory.transport() == frontend::FrontendTransportKind::Rfcomm &&
                              rfcommTlsFactory.transport() == frontend::FrontendTransportKind::RfcommTls,
                          "legacy and TLS RFCOMM factories borrow the same FrontendService and preserve peer transport facts");

        auto rfcommServer = net::rc::stream::legacy::Server<app::FrontendStreamSocketContextFactory>(
            "a1-7b-rfcomm-composition",
            [](net::rc::stream::legacy::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Local::setBtAddress("00:00:00:00:00:00")->setChannel(7);
                config->Connection::setMaximumWriteQueueBytes(app::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
            },
            service,
            app::FrontendStreamSocketContextFactoryOptions{
                .transport = frontend::FrontendTransportKind::Rfcomm, .socket = {}, .resolvePeer = {}});
        auto rfcommTlsServer = net::rc::stream::tls::Server<app::FrontendStreamSocketContextFactory>(
            "a1-7b-rfcomm-tls-composition",
            [](net::rc::stream::tls::config::ConfigSocketServer* config) {
                config->Instance::setDisabled(true);
                config->Local::setBtAddress("00:00:00:00:00:00")->setChannel(8);
                config->Connection::setMaximumWriteQueueBytes(app::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
            },
            service,
            app::FrontendStreamSocketContextFactoryOptions{
                .transport = frontend::FrontendTransportKind::RfcommTls, .socket = {}, .resolvePeer = {}});
        rfcommServer.getConfig()->Instance::forceUnrequired();
        rfcommTlsServer.getConfig()->Instance::forceUnrequired();
        result.expectTrue(rfcommServer.getConfig()->Local::getBtAddress() == "00:00:00:00:00:00" &&
                              rfcommServer.getConfig()->Local::getChannel() == 7 && rfcommTlsServer.getConfig()->Local::getChannel() == 8,
                          "RFCOMM legacy/TLS configuration and factory construction are deterministic without Bluetooth hardware");

        const frontend::FrontendPeerContext legacyPeer =
            app::streamPeerContextFromFileDescriptor(-1, frontend::FrontendTransportKind::Rfcomm);
        const frontend::FrontendPeerContext tlsPeer =
            app::streamPeerContextFromFileDescriptor(-1, frontend::FrontendTransportKind::RfcommTls);
        result.expectTrue(legacyPeer.transport == frontend::FrontendTransportKind::Rfcomm && !legacyPeer.encrypted &&
                              !legacyPeer.localPeer && tlsPeer.transport == frontend::FrontendTransportKind::RfcommTls &&
                              tlsPeer.encrypted && !tlsPeer.localPeer,
                          "RFCOMM peer propagation never treats Bluetooth pairing as frontend authentication");
        std::cout << "RFCOMM runtime hardware exchange not run: ordinary validation has no deterministic virtual Bluetooth controller; "
                     "component discovery, link, configuration, framing, peer metadata, and shared-service composition were verified.\n";
    }
#endif

    int runNativeLoopbackIntegration(int argc, char* argv[], tests::support::TestResult& result) {
        const std::string socketPath = unixSocketPath();
        std::remove(socketPath.c_str());
        const bool hasIpv6Loopback = ipv6LoopbackAvailable(result);
        const std::vector<ClientKind> activeKinds = activeClientKinds(hasIpv6Loopback);
        IntegrationState state(result, activeKinds.size());

        core::SNodeC::init(argc, argv);
        int eventLoopResult = 1;
        {
            const auto transport = std::make_shared<tests::codex::FakeTransportState>();
            FakeBackendCore backend({}, transport);
            frontend::FrontendServiceOptions serviceOptions;
            serviceOptions.trustedLocalUserId = static_cast<std::uint64_t>(::geteuid());
            serviceOptions.authenticator = [&state](const frontend::FrontendPeerContext& peer,
                                                    const frontend::AuthenticationCredential& credential) {
                state.recordAuthenticatedPeer(peer);
                return remoteAuthentication(credential);
            };
            frontend::FrontendService service(backend, std::move(serviceOptions));

            app::FrontendStreamSocketContextFactory unixFactory(
                service,
                app::FrontendStreamSocketContextFactoryOptions{
                    frontend::FrontendTransportKind::Unix, {}, [&state](core::socket::stream::SocketConnection& connection) {
                        frontend::FrontendPeerContext peer =
                            app::verifiedUnixPeerContextFromFileDescriptor(connection.getFd(), static_cast<std::uint64_t>(::geteuid()));
                        state.verifiedUnixPeer = peer;
                        return peer;
                    }});
            app::FrontendStreamSocketContextFactory ipv4Factory(
                service, app::FrontendStreamSocketContextFactoryOptions{frontend::FrontendTransportKind::Ipv4, {}, {}});
            app::FrontendStreamSocketContextFactory ipv6Factory(
                service, app::FrontendStreamSocketContextFactoryOptions{frontend::FrontendTransportKind::Ipv6, {}, {}});
            result.expectTrue(unixFactory.serviceIdentity() == &service && ipv4Factory.serviceIdentity() == &service &&
                                  ipv6Factory.serviceIdentity() == &service &&
                                  ipv6Factory.transport() == frontend::FrontendTransportKind::Ipv6,
                              "Unix, IPv4, and IPv6 listener factories borrow one application-owned FrontendService");

            auto unixServer = net::un::stream::legacy::Server<app::FrontendStreamSocketContextFactory>(
                "a1-7b-native-unix-server",
                [&socketPath](net::un::stream::legacy::config::ConfigSocketServer* config) {
                    config->Local::setSunPath(socketPath);
                    config->Connection::setReadTimeout(EstablishedInactivityProbeTimeout);
                    config->Connection::setWriteTimeout(EstablishedInactivityProbeTimeout);
                },
                service,
                app::FrontendStreamSocketContextFactoryOptions{
                    frontend::FrontendTransportKind::Unix, {}, [&state](core::socket::stream::SocketConnection& connection) {
                        frontend::FrontendPeerContext peer =
                            app::verifiedUnixPeerContextFromFileDescriptor(connection.getFd(), static_cast<std::uint64_t>(::geteuid()));
                        state.verifiedUnixPeer = peer;
                        return peer;
                    }});
            auto ipv4Server = net::in::stream::legacy::Server<app::FrontendStreamSocketContextFactory>(
                "a1-7b-native-ipv4-server",
                [](net::in::stream::legacy::config::ConfigSocketServer* config) {
                    config->Instance::setDisabled(false);
                    config->Local::setHost("127.0.0.1")->setPort(0);
                    config->Connection::setReadTimeout(EstablishedInactivityProbeTimeout);
                    config->Connection::setWriteTimeout(EstablishedInactivityProbeTimeout);
                    config->Connection::setMaximumWriteQueueBytes(app::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
                },
                service,
                app::FrontendStreamSocketContextFactoryOptions{
                    .transport = frontend::FrontendTransportKind::Ipv4, .socket = {}, .resolvePeer = {}});
            auto ipv6Server = net::in6::stream::legacy::Server<app::FrontendStreamSocketContextFactory>(
                "a1-7b-native-ipv6-server",
                [](net::in6::stream::legacy::config::ConfigSocketServer* config) {
                    config->Instance::setDisabled(false);
                    config->Local::setHost("::1")->setPort(0);
                    config->Connection::setReadTimeout(EstablishedInactivityProbeTimeout);
                    config->Connection::setWriteTimeout(EstablishedInactivityProbeTimeout);
                    config->Connection::setMaximumWriteQueueBytes(app::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
                },
                service,
                app::FrontendStreamSocketContextFactoryOptions{
                    .transport = frontend::FrontendTransportKind::Ipv6, .socket = {}, .resolvePeer = {}});

            using UnixClient = net::un::stream::legacy::SocketClient<FrontendProtocolClientFactory, IntegrationState&, ClientKind>;
            using Ipv4Client = net::in::stream::legacy::SocketClient<FrontendProtocolClientFactory, IntegrationState&, ClientKind>;
            using Ipv6Client = net::in6::stream::legacy::SocketClient<FrontendProtocolClientFactory, IntegrationState&, ClientKind>;
            UnixClient unixClient("a1-7b-native-unix-client", state, ClientKind::Unix);
            Ipv4Client ipv4Client("a1-7b-native-ipv4-client", state, ClientKind::Ipv4);
            Ipv6Client ipv6Client("a1-7b-native-ipv6-client", state, ClientKind::Ipv6);

#if defined(AISUITE_CODEX_FRONTEND_TLS)
            app::FrontendStreamSocketContextFactory tlsFactory(
                service, app::FrontendStreamSocketContextFactoryOptions{frontend::FrontendTransportKind::TcpTls, {}, {}});
            result.expectTrue(tlsFactory.serviceIdentity() == &service,
                              "the TLS listener factory borrows the same application-owned FrontendService");
            auto ipv4TlsServer = net::in::stream::tls::Server<app::FrontendStreamSocketContextFactory>(
                "a1-7b-native-ipv4-tls-server",
                [](net::in::stream::tls::config::ConfigSocketServer* config) {
                    config->Instance::setDisabled(false);
                    config->Local::setHost("127.0.0.1")->setPort(0);
                    config->setCert(AISUITE_CODEX_TEST_TLS_CERT);
                    config->setCertKey(AISUITE_CODEX_TEST_TLS_KEY);
                    config->Connection::setReadTimeout(EstablishedInactivityProbeTimeout);
                    config->Connection::setWriteTimeout(EstablishedInactivityProbeTimeout);
                    config->Connection::setMaximumWriteQueueBytes(app::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
                },
                service,
                app::FrontendStreamSocketContextFactoryOptions{
                    .transport = frontend::FrontendTransportKind::TcpTls, .socket = {}, .resolvePeer = {}});
            auto ipv6TlsServer = net::in6::stream::tls::Server<app::FrontendStreamSocketContextFactory>(
                "a1-7b-native-ipv6-tls-server",
                [](net::in6::stream::tls::config::ConfigSocketServer* config) {
                    config->Instance::setDisabled(false);
                    config->Local::setHost("::1")->setPort(0);
                    config->setCert(AISUITE_CODEX_TEST_TLS_CERT);
                    config->setCertKey(AISUITE_CODEX_TEST_TLS_KEY);
                    config->Connection::setReadTimeout(EstablishedInactivityProbeTimeout);
                    config->Connection::setWriteTimeout(EstablishedInactivityProbeTimeout);
                    config->Connection::setMaximumWriteQueueBytes(app::DEFAULT_MAXIMUM_OUTBOUND_BYTES);
                },
                service,
                app::FrontendStreamSocketContextFactoryOptions{
                    .transport = frontend::FrontendTransportKind::TcpTls, .socket = {}, .resolvePeer = {}});
            using Ipv4TlsClient = net::in::stream::tls::SocketClient<FrontendProtocolClientFactory, IntegrationState&, ClientKind>;
            using Ipv6TlsClient = net::in6::stream::tls::SocketClient<FrontendProtocolClientFactory, IntegrationState&, ClientKind>;
            Ipv4TlsClient ipv4TlsClient("a1-7b-native-ipv4-tls-client", state, ClientKind::Tls);
            Ipv6TlsClient ipv6TlsClient("a1-7b-native-ipv6-tls-client", state, ClientKind::Tls);
            ipv4TlsClient.getConfig()->setCaCert(AISUITE_CODEX_TEST_TLS_CERT);
            ipv4TlsClient.getConfig()->setCaCertAcceptUnknown(false);
            ipv4TlsClient.getConfig()->setSni("localhost");
            ipv6TlsClient.getConfig()->setCaCert(AISUITE_CODEX_TEST_TLS_CERT);
            ipv6TlsClient.getConfig()->setCaCertAcceptUnknown(false);
            ipv6TlsClient.getConfig()->setSni("localhost");
#endif

            unixServer.getConfig()->Instance::forceUnrequired();
            ipv4Server.getConfig()->Instance::forceUnrequired();
            ipv6Server.getConfig()->Instance::forceUnrequired();
            unixClient.getConfig()->Instance::forceUnrequired();
            ipv4Client.getConfig()->Instance::forceUnrequired();
            ipv6Client.getConfig()->Instance::forceUnrequired();
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            ipv4TlsServer.getConfig()->Instance::forceUnrequired();
            ipv6TlsServer.getConfig()->Instance::forceUnrequired();
            ipv4TlsClient.getConfig()->Instance::forceUnrequired();
            ipv6TlsClient.getConfig()->Instance::forceUnrequired();
#endif

            std::optional<net::un::SocketAddress> unixAddress;
            std::optional<net::in::SocketAddress> ipv4Address;
            std::optional<net::in6::SocketAddress> ipv6Address;
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            std::optional<net::in::SocketAddress> ipv4TlsAddress;
            std::optional<net::in6::SocketAddress> ipv6TlsAddress;
#endif
            bool connecting = false;
            auto connectAllWhenBound = [&] {
                if (connecting || state.listenSuccesses != activeKinds.size()) {
                    return;
                }
                connecting = true;
                for (const ClientKind kind : activeKinds) {
                    service.declareTransportFamily(transportKind(kind));
                }
                state.topologyObservedBeforeClients =
                    service.connectionCount() == 0 && service.implementedCapabilities().size() == 16 &&
                    hasCapability(service.implementedCapabilities(), frontend::FrontendCapability::MultiTransport);

                unixClient.connect(*unixAddress, [&state](const net::un::SocketAddress&, core::socket::State status) {
                    state.connectResult(ClientKind::Unix, status);
                });
                ipv4Client.connect(*ipv4Address, [&state](const net::in::SocketAddress&, core::socket::State status) {
                    state.connectResult(ClientKind::Ipv4, status);
                });
                if (hasIpv6Loopback) {
                    ipv6Client.connect(*ipv6Address, [&state](const net::in6::SocketAddress&, core::socket::State status) {
                        state.connectResult(ClientKind::Ipv6, status);
                    });
                }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
                if (hasIpv6Loopback) {
                    ipv6TlsClient.connect(*ipv6TlsAddress, [&state](const net::in6::SocketAddress&, core::socket::State status) {
                        state.connectResult(ClientKind::Tls, status);
                    });
                } else {
                    ipv4TlsClient.connect(*ipv4TlsAddress, [&state](const net::in::SocketAddress&, core::socket::State status) {
                        state.connectResult(ClientKind::Tls, status);
                    });
                }
#endif
            };

            unixServer.listen([&](const net::un::SocketAddress& address, core::socket::State status) {
                if (status != core::socket::State::OK || ::chmod(socketPath.c_str(), S_IRUSR | S_IWUSR) != 0) {
                    state.listenFailed(ClientKind::Unix, status);
                    return;
                }
                unixAddress = address;
                ++state.listenSuccesses;
                connectAllWhenBound();
            });
            ipv4Server.listen([&](const net::in::SocketAddress& address, core::socket::State status) {
                if (status != core::socket::State::OK) {
                    state.listenFailed(ClientKind::Ipv4, status);
                    return;
                }
                ipv4Address = net::in::SocketAddress("127.0.0.1", address.getPort());
                ++state.listenSuccesses;
                connectAllWhenBound();
            });
            if (hasIpv6Loopback) {
                ipv6Server.listen([&](const net::in6::SocketAddress& address, core::socket::State status) {
                    if (status != core::socket::State::OK) {
                        state.listenFailed(ClientKind::Ipv6, status);
                        return;
                    }
                    ipv6Address = net::in6::SocketAddress("::1", address.getPort());
                    ++state.listenSuccesses;
                    connectAllWhenBound();
                });
            }
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            if (hasIpv6Loopback) {
                ipv6TlsServer.listen([&](const net::in6::SocketAddress& address, core::socket::State status) {
                    if (status != core::socket::State::OK) {
                        state.listenFailed(ClientKind::Tls, status);
                        return;
                    }
                    ipv6TlsAddress = net::in6::SocketAddress("::1", address.getPort());
                    ++state.listenSuccesses;
                    connectAllWhenBound();
                });
            } else {
                ipv4TlsServer.listen([&](const net::in::SocketAddress& address, core::socket::State status) {
                    if (status != core::socket::State::OK) {
                        state.listenFailed(ClientKind::Tls, status);
                        return;
                    }
                    ipv4TlsAddress = net::in::SocketAddress("127.0.0.1", address.getPort());
                    ++state.listenSuccesses;
                    connectAllWhenBound();
                });
            }
#endif

#if defined(AISUITE_TEST_CODEX_FRONTEND_RFCOMM)
            expectRfcommComposition(result, service);
#endif

            [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
                [&state] {
                    state.timedOut = true;
                    core::SNodeC::stop();
                },
                utils::Timeval({8, 0}));
            eventLoopResult = core::SNodeC::start(utils::Timeval({10, 0}));

            result.expectTrue(state.topologyObservedBeforeClients,
                              "multiple bound native families advertise multi_transport before any client connects");
            result.expectTrue(!state.timedOut && state.completed,
                              "all real native SNode.C loopback clients finish Frontend Protocol v1 synchronization before timeout");
            result.expectTrue(state.idleEstablishedSurvived,
                              "idle established native frontends remain connected beyond the configured SNode.C inactivity timeout");
            result.expectTrue(eventLoopResult == 0 && state.listenFailures == 0 && state.listenSuccesses == activeKinds.size() &&
                                  state.connectFailures == 0 && state.connectSuccesses == activeKinds.size() && state.decodeFailures == 0,
                              "Unix, IPv4, pinned-CA TLS, and available IPv6 listeners bind/connect/decode without transport-local "
                              "failures");

            for (const ClientKind kind : activeKinds) {
                const ClientObservation& observation = state.clients[clientIndex(kind)];
                result.expectTrue(
                    observation.connected == 1 && observation.welcome == 1 && observation.snapshot == 1 && observation.syncComplete == 1 &&
                        observation.protocolErrors == 0 && observation.advertisedMultiTransport && observation.availableMethods == 90,
                    std::string(clientName(kind)) + " carries one Hello/Welcome/snapshot/sync exchange over the shared FrontendService");
                const std::size_t expectedPermitted = kind == ClientKind::Unix && app::unixPeerCredentialsSupported() ? 90U : 53U;
                result.expectTrue(observation.permittedMethods == expectedPermitted,
                                  std::string(clientName(kind)) + " receives the principal-specific 90/90 or 53/90 method ceiling");
            }

            const bool expectedVerifiedUnix =
                state.verifiedUnixPeer.has_value() && state.verifiedUnixPeer->transport == frontend::FrontendTransportKind::Unix &&
                state.verifiedUnixPeer->localPeer && state.verifiedUnixPeer->unixUserId == static_cast<std::uint64_t>(::geteuid());
            const bool expectedUnverifiedUnix =
                state.verifiedUnixPeer.has_value() && !state.verifiedUnixPeer->localPeer && !state.verifiedUnixPeer->unixUserId.has_value();
            result.expectTrue(app::unixPeerCredentialsSupported() ? expectedVerifiedUnix : expectedUnverifiedUnix,
                              "the accepted Unix getFd path verifies credentials when supported and otherwise remains untrusted");
            const std::size_t expectedAuthenticatedPeers = activeKinds.size() - (app::unixPeerCredentialsSupported() ? 1U : 0U);
            result.expectTrue(state.authenticatedPeers.size() == expectedAuthenticatedPeers,
                              "verified Unix trust bypasses bearer authentication only on platforms with peer credentials");
            const bool everyRemotePeerBounded =
                std::all_of(state.authenticatedPeers.begin(), state.authenticatedPeers.end(), [](const auto& peer) {
                    return peer.transport == frontend::FrontendTransportKind::Unix ||
                           (peer.remoteAddress.has_value() && peer.remoteAddress->size() <= 256 && peer.loopback && !peer.localPeer &&
                            !peer.unixUserId.has_value());
                });
            const bool sawIpv4 = std::any_of(state.authenticatedPeers.begin(), state.authenticatedPeers.end(), [](const auto& peer) {
                return peer.transport == frontend::FrontendTransportKind::Ipv4 && !peer.encrypted;
            });
            const bool sawIpv6 = std::any_of(state.authenticatedPeers.begin(), state.authenticatedPeers.end(), [](const auto& peer) {
                return peer.transport == frontend::FrontendTransportKind::Ipv6 && !peer.encrypted;
            });
#if defined(AISUITE_CODEX_FRONTEND_TLS)
            const bool sawTls = std::any_of(state.authenticatedPeers.begin(), state.authenticatedPeers.end(), [](const auto& peer) {
                return peer.transport == frontend::FrontendTransportKind::TcpTls && peer.encrypted && peer.remoteAddress.has_value() &&
                       peer.loopback;
            });
#else
            const bool sawTls = true;
#endif
            result.expectTrue(
                everyRemotePeerBounded && sawIpv4 && sawIpv6 == hasIpv6Loopback && sawTls,
                "real accepted IPv4, available IPv6, and pinned-CA TLS connections propagate bounded loopback facts without false "
                "local trust");
        }

        core::SNodeC::free();
        std::remove(socketPath.c_str());
        return eventLoopResult;
    }

} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;

    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexFrontendNativeTransportTest");
        return tests::support::cTestSkipReturnCode;
    }

    expectTransportFacts(result);
    expectEstablishedTimeoutTransition(result);
    expectTerminalSendCloseDiagnostics(result);
    expectTransportCapabilityIsNotAdvertised(result);
    static_cast<void>(runNativeLoopbackIntegration(argc, argv, result));
    return result.processResult();
}
