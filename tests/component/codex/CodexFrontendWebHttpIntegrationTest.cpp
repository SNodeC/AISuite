/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "apps/codex-backend/FrontendWebApplication.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/socket/State.h"
#include "core/socket/stream/SocketConnection.h"
#include "core/socket/stream/SocketContext.h"
#include "core/timer/Timer.h"
#include "express/legacy/in/WebApp.h"
#include "net/in/SocketAddress.h"
#include "net/in/stream/legacy/SocketClient.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"
#include "web/http/ConfigHttpParser.h"
#include "web/http/client/Response.h"
#include "web/http/legacy/in/Client.h"
#include "web/http/server/ConfigHttpServer.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

    namespace app = apps::codex_backend;
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;
    using HttpClient = web::http::legacy::in::Client;
    using MasterRequest = HttpClient::MasterRequest;

    constexpr std::string_view SecretSentinel = "a1-7b-http-static-secret-sentinel";
    constexpr std::string_view IndexBody = "<!doctype html><title>AISuite frontend</title>\n";
    constexpr std::string_view ScriptBody = "export const frontendReady = true;\n";
    constexpr std::string_view ContentSecurityPolicyHeader = "Content-Security-Policy";
    constexpr std::string_view ContentTypeOptionsHeader = "X-Content-Type-Options";
    constexpr std::string_view ReferrerPolicyHeader = "Referrer-Policy";
    constexpr std::size_t FailedPipeProbeCount = 8;

    std::string largeStaticBody() {
        std::string body(16U * 1024U * 6U + 37U, '\0');
        for (std::size_t index = 0; index < body.size(); ++index) {
            body[index] = static_cast<char>('!' + index % 80);
        }
        return body;
    }

    class TemporaryDirectory {
    public:
        TemporaryDirectory() {
            std::string pattern = (std::filesystem::temp_directory_path() / "aisuite-web-http-live-XXXXXX").string();
            if (char* created = ::mkdtemp(pattern.data()); created != nullptr) {
                path = created;
            }
        }

        TemporaryDirectory(const TemporaryDirectory&) = delete;
        TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

        ~TemporaryDirectory() {
            std::error_code error;
            std::filesystem::remove_all(path, error);
        }

        [[nodiscard]] const std::filesystem::path& get() const noexcept {
            return path;
        }

    private:
        std::filesystem::path path;
    };

    bool writeFile(const std::filesystem::path& path, std::string_view content) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        return output.good();
    }

    struct StaticFixture {
        explicit StaticFixture(const std::filesystem::path& parent)
            : root(parent / "public")
            , outside(parent / "outside-secret.txt") {
            std::error_code error;
            std::filesystem::create_directories(root / "assets", error);
            ready = !error && writeFile(root / "index.html", IndexBody) && writeFile(root / "bundle.js", ScriptBody) &&
                    writeFile(root / "large.txt", largeStaticBody()) && writeFile(root / "pipe-failure.txt", IndexBody) &&
                    writeFile(root / "payload.exe", "unsupported executable payload\n") && writeFile(outside, SecretSentinel);
            if (!ready) {
                return;
            }
            std::filesystem::create_symlink(outside, root / "escape.txt", error);
            ready = !error;
        }

        std::filesystem::path root;
        std::filesystem::path outside;
        bool ready = false;
    };

    enum class Listener { StaticRoot, NoRoot };
    enum class Origin { None, SameOrigin, Rejected };
    enum class CredentialChannel { None, Authorization, Cookie };

    struct Case {
        Case() = default;

        Case(std::string name,
             Listener listener,
             std::string method,
             std::string target,
             Origin origin,
             std::string expectedStatus,
             std::optional<std::string> expectedBody,
             std::optional<std::string> expectedContentType,
             std::optional<std::size_t> expectedContentLength,
             CredentialChannel credentialChannel = CredentialChannel::None,
             std::string requestBody = {},
             bool webSocketUpgrade = false,
             bool expectSecurityHeaders = true,
             bool expectRouteDispatch = true)
            : name(std::move(name))
            , listener(listener)
            , method(std::move(method))
            , target(std::move(target))
            , origin(origin)
            , expectedStatus(std::move(expectedStatus))
            , expectedBody(std::move(expectedBody))
            , expectedContentType(std::move(expectedContentType))
            , expectedContentLength(expectedContentLength)
            , credentialChannel(credentialChannel)
            , requestBody(std::move(requestBody))
            , webSocketUpgrade(webSocketUpgrade)
            , expectSecurityHeaders(expectSecurityHeaders)
            , expectRouteDispatch(expectRouteDispatch) {
        }

        std::string name;
        Listener listener = Listener::StaticRoot;
        std::string method = "GET";
        std::string target;
        Origin origin = Origin::None;
        std::string expectedStatus;
        std::optional<std::string> expectedBody;
        std::optional<std::string> expectedContentType;
        std::optional<std::size_t> expectedContentLength;
        CredentialChannel credentialChannel = CredentialChannel::None;
        std::string requestBody;
        bool webSocketUpgrade = false;
        bool expectSecurityHeaders = true;
        bool expectRouteDispatch = true;
    };

    struct Observation {
        Case testCase;
        std::string status;
        std::string body;
        std::string contentType;
        std::string contentLength;
        std::string contentSecurityPolicy;
        std::string contentTypeOptions;
        std::string referrerPolicy;
        std::string responseMaterial;
    };

    struct RawProbeObservation {
        RawProbeObservation() = default;

        explicit RawProbeObservation(std::string request)
            : request(std::move(request)) {
        }

        std::string request;
        std::string response;
        bool connected = false;
        bool complete = false;
        bool connectFailed = false;
    };

    struct State {
        State() {
            for (RawProbeObservation& probe : failedPipeProbes) {
                probe.request = "GET /pipe-failure.txt HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                                "X-AISuite-Test-Failed-Pipe: 1\r\nConnection: close\r\n\r\n";
            }
        }

        std::size_t listenerSuccesses = 0;
        std::size_t listenerFailures = 0;
        std::size_t clientConnectSuccesses = 0;
        std::size_t httpConnections = 0;
        std::size_t responses = 0;
        std::size_t parseErrors = 0;
        std::size_t routeDispatches = 0;
        std::size_t failedPipeRouteDispatches = 0;
        std::size_t failedPipeProbeCompletions = 0;
        std::size_t failedPipeDescriptorBaseline = 0;
        std::size_t failedPipeDescriptorFinal = 0;
        std::size_t authenticationAttempts = 0;
        std::size_t unexpectedStates = 0;
        bool timedOut = false;
        bool caseRunnerComplete = false;
        bool serviceStayedAtSurvivorBaseline = true;
        bool responseObservedRetainedAdmission = false;
        bool failedPipeCleanupDrained = false;
        bool survivorOpenAfterPipeFailures = false;
        bool survivorCommandAcceptedAfterPipeFailures = false;
        std::size_t survivorOutboundBeforePipeFailures = 0;
        std::size_t survivorOutboundMessages = 0;
        RawProbeObservation headProbe{"HEAD /index.html HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n"};
        RawProbeObservation headBodyProbe{
            "HEAD /index.html HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 1\r\nConnection: close\r\n\r\nx"};
        RawProbeObservation chunkedProbe{
            "GET /index.html HTTP/1.1\r\nHost: 127.0.0.1\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n1\r\nx\r\n0\r\n\r\n"};
        std::array<RawProbeObservation, FailedPipeProbeCount> failedPipeProbes;
        std::vector<Observation> observations;
    };

    std::size_t openDescriptorCount() {
        std::error_code error;
        std::size_t count = 0;
        std::filesystem::directory_iterator entry("/proc/self/fd", error);
        const std::filesystem::directory_iterator end;
        while (!error && entry != end) {
            ++count;
            entry.increment(error);
        }
        return error ? 0 : count;
    }

    std::string responseBody(const web::http::client::Response& response) {
        return {response.body.begin(), response.body.end()};
    }

    std::string observedResponseMaterial(const web::http::client::Response& response) {
        std::string material = response.statusCode + response.reason;
        material.append(response.body.begin(), response.body.end());
        for (const auto& [name, value] : response.headers) {
            material += name;
            material += value;
        }
        return material;
    }

    class CaseRunner {
    public:
        CaseRunner(State& state, frontend::FrontendService& service, std::deque<Case> cases, std::function<void()> onFinished)
            : state(state)
            , service(service)
            , cases(std::move(cases))
            , onFinished(std::move(onFinished)) {
        }

        void start(std::uint16_t staticRootPort, std::uint16_t noRootPort) {
            this->staticRootPort = staticRootPort;
            this->noRootPort = noRootPort;
            dispatchNext();
        }

        void clearClients() {
            clients.clear();
        }

    private:
        [[nodiscard]] std::uint16_t portFor(Listener listener) const noexcept {
            return listener == Listener::StaticRoot ? staticRootPort : noRootPort;
        }

        void fail() {
            ++state.unexpectedStates;
            core::SNodeC::stop();
        }

        void recordSessionState() {
            state.responseObservedRetainedAdmission = state.responseObservedRetainedAdmission ||
                                                      (service.connectionCount() != 0 && service.unauthenticatedConnectionCount() != 0);
            state.serviceStayedAtSurvivorBaseline = state.serviceStayedAtSurvivorBaseline && service.authenticatedConnectionCount() == 1 &&
                                                    !service.currentController().has_value();
        }

        void dispatchNext() {
            if (cases.empty()) {
                recordSessionState();
                onFinished();
                return;
            }

            const Case current = std::move(cases.front());
            cases.pop_front();
            const std::uint16_t port = portFor(current.listener);
            if (port == 0) {
                fail();
                return;
            }

            clients.push_back(std::make_shared<HttpClient>(
                "a1-7b-frontend-http-live-client-" + std::to_string(clients.size()),
                [this, current, port](const std::shared_ptr<MasterRequest>& request) {
                    ++state.httpConnections;
                    request->method = current.method;
                    request->url = current.target;
                    request->set("Connection", "close");
                    if (current.target == "/frontend") {
                        request->set("Sec-WebSocket-Protocol", std::string(app::FrontendWebSocketSubProtocolName));
                    }
                    if (current.webSocketUpgrade) {
                        request->set("Connection", "Upgrade");
                        request->set("Upgrade", "websocket");
                        request->set("Sec-WebSocket-Version", "13");
                        request->set("Sec-WebSocket-Key", "dGhlIHNhbXBsZSBub25jZQ==");
                    }
                    if (current.origin == Origin::SameOrigin) {
                        request->set("Origin", "http://127.0.0.1:" + std::to_string(port));
                    } else if (current.origin == Origin::Rejected) {
                        request->set("Origin", "https://rejected-origin.example");
                    }
                    if (current.credentialChannel == CredentialChannel::Authorization) {
                        request->set("Authorization", "Bearer " + std::string(SecretSentinel));
                    } else if (current.credentialChannel == CredentialChannel::Cookie) {
                        request->set("Cookie", "frontend=" + std::string(SecretSentinel));
                    }
                    const auto onResponse = [this, current](const auto&, const auto& response) {
                        ++state.responses;
                        Observation observation;
                        observation.testCase = current;
                        observation.status = response->statusCode;
                        observation.body = responseBody(*response);
                        observation.contentType = response->get("Content-Type");
                        observation.contentLength = response->get("Content-Length");
                        observation.contentSecurityPolicy = response->get(std::string(ContentSecurityPolicyHeader));
                        observation.contentTypeOptions = response->get(std::string(ContentTypeOptionsHeader));
                        observation.referrerPolicy = response->get(std::string(ReferrerPolicyHeader));
                        observation.responseMaterial = observedResponseMaterial(*response);
                        state.observations.push_back(std::move(observation));
                        recordSessionState();
                        dispatchNext();
                    };
                    const auto onParseError = [this](const auto&, const std::string&) {
                        ++state.parseErrors;
                        fail();
                    };
                    const bool queued = current.requestBody.empty() ? request->end(onResponse, onParseError)
                                                                    : request->send(current.requestBody, onResponse, onParseError);
                    if (!queued) {
                        fail();
                    }
                },
                [](const std::shared_ptr<MasterRequest>&) {
                }));

            const std::shared_ptr<HttpClient>& client = clients.back();
            client->getConfig()->Instance::forceUnrequired();
            client->connect(net::in::SocketAddress("127.0.0.1", port), [this](const auto&, core::socket::State connectState) {
                if (connectState == core::socket::State::OK) {
                    ++state.clientConnectSuccesses;
                } else {
                    fail();
                }
            });
        }

        State& state;
        frontend::FrontendService& service;
        std::deque<Case> cases;
        std::vector<std::shared_ptr<HttpClient>> clients;
        std::function<void()> onFinished;
        std::uint16_t staticRootPort = 0;
        std::uint16_t noRootPort = 0;
    };

    class RawProbeSocketContext final : public core::socket::stream::SocketContext {
    public:
        RawProbeSocketContext(core::socket::stream::SocketConnection* connection,
                              RawProbeObservation& observation,
                              std::function<void()> onComplete)
            : core::socket::stream::SocketContext(connection)
            , observation(observation)
            , onComplete(std::move(onComplete)) {
        }

    private:
        void onConnected() override {
            observation.connected = true;
            sendToPeer(observation.request.data(), observation.request.size());
        }

        void onDisconnected() override {
            observation.complete = true;
            if (onComplete) {
                onComplete();
            }
        }

        std::size_t onReceivedFromPeer() override {
            std::array<char, 16U * 1024U> chunk{};
            const std::size_t size = readFromPeer(chunk.data(), chunk.size());
            if (size <= 64U * 1024U - observation.response.size()) {
                observation.response.append(chunk.data(), size);
            } else {
                observation.connectFailed = true;
                close();
            }
            return size;
        }

        bool onSignal([[maybe_unused]] int signum) override {
            return true;
        }

    private:
        RawProbeObservation& observation;
        std::function<void()> onComplete;
    };

    class RawProbeSocketContextFactory final : public core::socket::stream::SocketContextFactory {
    public:
        RawProbeSocketContextFactory(RawProbeObservation& observation, std::function<void()> onComplete)
            : observation(observation)
            , onComplete(std::move(onComplete)) {
        }

    private:
        core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* connection) override {
            return new RawProbeSocketContext(connection, observation, onComplete);
        }

        RawProbeObservation& observation;
        std::function<void()> onComplete;
    };

    std::deque<Case> testCases() {
        return {
            {"static GET",
             Listener::StaticRoot,
             "GET",
             "/index.html",
             Origin::None,
             "200",
             std::string(IndexBody),
             "text/html; charset=utf-8",
             IndexBody.size()},
            {"static GET larger than frontend writer capacity",
             Listener::StaticRoot,
             "GET",
             "/large.txt",
             Origin::None,
             "200",
             largeStaticBody(),
             "text/plain; charset=utf-8",
             largeStaticBody().size()},
            {"no static root",
             Listener::NoRoot,
             "GET",
             "/index.html",
             Origin::None,
             "404",
             "not_found",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"raw traversal",
             Listener::StaticRoot,
             "GET",
             "/../outside-secret.txt",
             Origin::None,
             "404",
             "not_found",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"encoded traversal",
             Listener::StaticRoot,
             "GET",
             "/%2e%2e/outside-secret.txt",
             Origin::None,
             "404",
             "not_found",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"double encoded traversal",
             Listener::StaticRoot,
             "GET",
             "/%252e%252e/outside-secret.txt",
             Origin::None,
             "404",
             "not_found",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"symlink escape",
             Listener::StaticRoot,
             "GET",
             "/escape.txt",
             Origin::None,
             "404",
             "not_found",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"directory listing",
             Listener::StaticRoot,
             "GET",
             "/assets",
             Origin::None,
             "404",
             "not_found",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"unsupported MIME",
             Listener::StaticRoot,
             "GET",
             "/payload.exe",
             Origin::None,
             "415",
             "unsupported_media_type",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"safe MIME",
             Listener::StaticRoot,
             "GET",
             "/bundle.js",
             Origin::None,
             "200",
             std::string(ScriptBody),
             "text/javascript; charset=utf-8",
             ScriptBody.size()},
            {"one-byte static GET body",
             Listener::StaticRoot,
             "GET",
             "/index.html",
             Origin::None,
             "400",
             "request_body_rejected",
             "text/plain; charset=utf-8",
             std::nullopt,
             CredentialChannel::None,
             "x"},
            {"one-byte WebSocket upgrade body",
             Listener::StaticRoot,
             "GET",
             "/frontend",
             Origin::None,
             "400",
             "request_body_rejected",
             "text/plain; charset=utf-8",
             std::nullopt,
             CredentialChannel::None,
             "x",
             true},
            {"oversized static GET body",
             Listener::StaticRoot,
             "GET",
             "/index.html",
             Origin::None,
             "413",
             std::nullopt,
             std::nullopt,
             std::nullopt,
             CredentialChannel::None,
             "xx",
             false,
             false,
             false},
            {"endpoint trailing slash",
             Listener::StaticRoot,
             "GET",
             "/frontend/",
             Origin::None,
             "404",
             "not_found",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"endpoint descendant",
             Listener::StaticRoot,
             "GET",
             "/frontend/extra",
             Origin::None,
             "404",
             "not_found",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"same origin admission",
             Listener::StaticRoot,
             "GET",
             "/frontend",
             Origin::SameOrigin,
             "400",
             "invalid_websocket_upgrade",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"query credential channel",
             Listener::StaticRoot,
             "GET",
             "/frontend?access_token=" + std::string(SecretSentinel),
             Origin::None,
             "400",
             "websocket_credential_channel_rejected",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"authorization credential channel",
             Listener::StaticRoot,
             "GET",
             "/frontend",
             Origin::None,
             "400",
             "websocket_credential_channel_rejected",
             "text/plain; charset=utf-8",
             std::nullopt,
             CredentialChannel::Authorization},
            {"cookie credential channel",
             Listener::StaticRoot,
             "GET",
             "/frontend",
             Origin::None,
             "400",
             "websocket_credential_channel_rejected",
             "text/plain; charset=utf-8",
             std::nullopt,
             CredentialChannel::Cookie},
            {"rejected origin one",
             Listener::StaticRoot,
             "GET",
             "/frontend",
             Origin::Rejected,
             "403",
             "origin_rejected",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"rejected origin two",
             Listener::StaticRoot,
             "GET",
             "/frontend",
             Origin::Rejected,
             "403",
             "origin_rejected",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"rejected origin three",
             Listener::StaticRoot,
             "GET",
             "/frontend",
             Origin::Rejected,
             "403",
             "origin_rejected",
             "text/plain; charset=utf-8",
             std::nullopt},
            {"rejected origin rate limited",
             Listener::StaticRoot,
             "GET",
             "/frontend",
             Origin::Rejected,
             "429",
             "rate_limited",
             "text/plain; charset=utf-8",
             std::nullopt},
        };
    }

} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexFrontendWebHttpIntegrationTest");
        return tests::support::cTestSkipReturnCode;
    }

    TemporaryDirectory temporary;
    StaticFixture fixture(temporary.get());
    result.expectTrue(!temporary.get().empty() && fixture.ready,
                      "the live HTTP test creates an isolated static root, outside secret, and symlink escape fixture");
    if (temporary.get().empty() || !fixture.ready) {
        return result.processResult();
    }

    core::SNodeC::init(argc, argv);
    State state;
    int eventLoopResult = 1;
    {
        const auto transport = std::make_shared<tests::codex::FakeTransportState>();
        FakeBackendCore backend({}, transport);
        frontend::FrontendServiceOptions serviceOptions;
        serviceOptions.authenticator = [&state](const frontend::FrontendPeerContext& peer, const auto&) -> frontend::AuthenticationResult {
            if (peer.transport == frontend::FrontendTransportKind::InMemory) {
                return frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
                    "failed-pipe-survivor",
                    std::vector<frontend::FrontendScope>{frontend::FrontendScope::Observe, frontend::FrontendScope::Control},
                    "default_remote",
                    false}};
            }
            ++state.authenticationAttempts;
            return frontend::AuthenticationFailure{frontend::AuthenticationFailureCode::AuthenticationFailed};
        };
        frontend::FrontendService service(backend, std::move(serviceOptions));
        frontend::FrontendPeerContext survivorPeer;
        survivorPeer.transport = frontend::FrontendTransportKind::InMemory;
        frontend::FrontendConnection survivor = service.openConnection(std::move(survivorPeer),
                                                                       {[&state](const frontend::OutboundMessage&) {
                                                                            ++state.survivorOutboundMessages;
                                                                            return true;
                                                                        },
                                                                        {}});
        result.expectTrue(survivor.receive(frontend::Hello{std::nullopt, frontend::Json::object()}).accepted(),
                          "an unrelated in-memory frontend authenticates before failed static-pipe probes");

        app::FrontendWebApplication staticApplication(service,
                                                      app::FrontendWebApplicationOptions{
                                                          .endpoint = "/frontend",
                                                          .staticRoot = fixture.root,
                                                          .allowedOrigins = {},
                                                          .transport = frontend::FrontendTransportKind::WebSocket,
                                                          .encrypted = false,
                                                      });
        app::FrontendWebApplication rootlessApplication(service,
                                                        app::FrontendWebApplicationOptions{
                                                            .endpoint = "/frontend",
                                                            .staticRoot = std::nullopt,
                                                            .allowedOrigins = {},
                                                            .transport = frontend::FrontendTransportKind::WebSocket,
                                                            .encrypted = false,
                                                        });
        express::legacy::in::WebApp staticWebApp("a1-7b-frontend-http-static-server");
        express::legacy::in::WebApp rootlessWebApp("a1-7b-frontend-http-rootless-server");
        staticWebApp.use([&state] MIDDLEWARE(request, response, next) {
            ++state.routeDispatches;
            next();
        });
        staticWebApp.use([&state] MIDDLEWARE(request, response, next) {
            if (request->get("x-aisuite-test-failed-pipe") == "1") {
                ++state.failedPipeRouteDispatches;
                if (response->getSocketContext() != nullptr && response->getSocketContext()->getSocketConnection() != nullptr) {
                    response->getSocketContext()->getSocketConnection()->close();
                }
            }
            next();
        });
        rootlessWebApp.use([&state] MIDDLEWARE(request, response, next) {
            ++state.routeDispatches;
            next();
        });
        staticApplication.configure(staticWebApp);
        rootlessApplication.configure(rootlessWebApp);
        staticWebApp.getConfig()->Instance::forceUnrequired();
        rootlessWebApp.getConfig()->Instance::forceUnrequired();

        auto* httpPolicy = staticWebApp.getConfig()->net::config::ConfigInstance::getSubCommand<web::http::server::ConfigHttpServer>();
        httpPolicy->setMaximumPendingRequests(1)->setAllowChunkedTransfer(false)->setAllowPipelining(false);
        httpPolicy->getParserConfig()
            ->setMaximumStartLineBytes(8192)
            ->setMaximumHeaderLineBytes(8192)
            ->setMaximumHeaderBytes(65536)
            ->setMaximumHeaderFields(128)
            ->setMaximumBodyBytes(1);
        const web::http::ParserLimits parserLimits = httpPolicy->getParserLimits();
        const web::http::server::HttpServerPolicy serverPolicy = httpPolicy->getServerPolicy();
        result.expectTrue(parserLimits.maximumStartLineBytes == 8192 && parserLimits.maximumHeaderLineBytes == 8192 &&
                              parserLimits.maximumHeaderBytes == 65536 && parserLimits.maximumHeaderFields == 128 &&
                              parserLimits.maximumBodyBytes == 1 && serverPolicy.maximumPendingRequests == 1 &&
                              !serverPolicy.allowChunkedTransfer && !serverPolicy.allowPipelining,
                          "SNode.C owns the exact HTTP parser and single-request admission policy");

        result.expectTrue(staticApplication.serviceIdentity() == &service && rootlessApplication.serviceIdentity() == &service,
                          "both production HTTP applications borrow the one application-owned FrontendService");

        const auto maybeStop = [&state] {
            if (state.caseRunnerComplete && state.headProbe.complete && state.headBodyProbe.complete && state.chunkedProbe.complete) {
                core::SNodeC::stop();
            }
        };
        CaseRunner runner(state, service, testCases(), [&] {
            state.caseRunnerComplete = true;
            maybeStop();
        });
        using RawProbeClient =
            net::in::stream::legacy::SocketClient<RawProbeSocketContextFactory, RawProbeObservation&, std::function<void()>>;
        RawProbeClient headProbe("a1-7b-frontend-http-head-probe", state.headProbe, maybeStop);
        RawProbeClient headBodyProbe("a1-7b-frontend-http-head-body-probe", state.headBodyProbe, maybeStop);
        RawProbeClient chunkedProbe("a1-7b-frontend-http-chunked-probe", state.chunkedProbe, maybeStop);
        std::optional<std::uint16_t> staticPort;
        std::optional<std::uint16_t> rootlessPort;
        std::vector<std::unique_ptr<RawProbeClient>> failedPipeProbes;
        failedPipeProbes.reserve(FailedPipeProbeCount);
        const auto startNormalProbes = [&] {
            runner.start(*staticPort, *rootlessPort);
            const auto connectProbe = [&](RawProbeClient& probe, RawProbeObservation& observation) {
                probe.connect(net::in::SocketAddress("127.0.0.1", *staticPort),
                              [&, observationPtr = &observation](const auto&, core::socket::State connectState) {
                                  if (connectState != core::socket::State::OK) {
                                      observationPtr->connectFailed = true;
                                      observationPtr->complete = true;
                                      maybeStop();
                                  }
                              });
            };
            connectProbe(headProbe, state.headProbe);
            connectProbe(headBodyProbe, state.headBodyProbe);
            connectProbe(chunkedProbe, state.chunkedProbe);
        };
        const auto failedPipeComplete = [&] {
            ++state.failedPipeProbeCompletions;
            if (state.failedPipeProbeCompletions == FailedPipeProbeCount) {
                core::EventReceiver::atNextTick([&] {
                    core::EventReceiver::atNextTick([&] {
                        state.failedPipeDescriptorFinal = openDescriptorCount();
                        state.failedPipeCleanupDrained = true;
                        state.survivorOpenAfterPipeFailures = survivor.isOpen();
                        state.survivorCommandAcceptedAfterPipeFailures =
                            survivor.receive(frontend::Command{"survivor-snapshot", frontend::SnapshotGet{}}).accepted();
                        startNormalProbes();
                    });
                });
            }
        };
        for (std::size_t index = 0; index < FailedPipeProbeCount; ++index) {
            failedPipeProbes.push_back(std::make_unique<RawProbeClient>(
                "a1-7b-frontend-http-failed-pipe-probe-" + std::to_string(index), state.failedPipeProbes[index], failedPipeComplete));
            failedPipeProbes.back()->getConfig()->Instance::forceUnrequired();
        }
        headProbe.getConfig()->Instance::forceUnrequired();
        headBodyProbe.getConfig()->Instance::forceUnrequired();
        chunkedProbe.getConfig()->Instance::forceUnrequired();
        bool runnerStarted = false;
        const auto startWhenReady = [&] {
            if (!runnerStarted && staticPort.has_value() && rootlessPort.has_value()) {
                runnerStarted = true;
                state.failedPipeDescriptorBaseline = openDescriptorCount();
                state.survivorOutboundBeforePipeFailures = state.survivorOutboundMessages;
                for (std::size_t index = 0; index < failedPipeProbes.size(); ++index) {
                    failedPipeProbes[index]->connect(net::in::SocketAddress("127.0.0.1", *staticPort),
                                                     [&, index](const auto&, core::socket::State connectState) {
                                                         if (connectState != core::socket::State::OK) {
                                                             state.failedPipeProbes[index].connectFailed = true;
                                                             state.failedPipeProbes[index].complete = true;
                                                             failedPipeComplete();
                                                         }
                                                     });
                }
            }
        };

        staticWebApp.listen(net::in::SocketAddress("127.0.0.1", 0), [&](const auto& address, core::socket::State listenState) {
            if (listenState != core::socket::State::OK || address.getPort() == 0) {
                ++state.listenerFailures;
                core::SNodeC::stop();
                return;
            }
            ++state.listenerSuccesses;
            staticPort = address.getPort();
            startWhenReady();
        });
        rootlessWebApp.listen(net::in::SocketAddress("127.0.0.1", 0), [&](const auto& address, core::socket::State listenState) {
            if (listenState != core::socket::State::OK || address.getPort() == 0) {
                ++state.listenerFailures;
                core::SNodeC::stop();
                return;
            }
            ++state.listenerSuccesses;
            rootlessPort = address.getPort();
            startWhenReady();
        });

        [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
            [&state] {
                state.timedOut = true;
                ++state.unexpectedStates;
                core::SNodeC::stop();
            },
            utils::Timeval({5, 0}));
        eventLoopResult = core::SNodeC::start();
        runner.clearClients();
        service.close("live HTTP integration complete");
    }
    core::SNodeC::free();

    const std::deque<Case> expectedCases = testCases();
    result.expectEqual(0, eventLoopResult, "the live IPv4 HTTP event loop exits successfully");
    result.expectTrue(!state.timedOut, "all deterministic live HTTP cases finish before the watchdog");
    result.expectEqual(std::size_t{2}, state.listenerSuccesses, "the static-root and rootless production applications both bind");
    result.expectEqual(std::size_t{0}, state.listenerFailures, "neither production HTTP application reports a bind failure");
    result.expectEqual(expectedCases.size(), state.clientConnectSuccesses, "one exact SNode.C HTTP client connection succeeds per case");
    result.expectEqual(expectedCases.size(), state.httpConnections, "every client reaches the HTTP-connected callback");
    result.expectEqual(expectedCases.size(), state.responses, "every live HTTP request receives one terminal response");
    result.expectEqual(std::size_t{0}, state.parseErrors, "the exact SNode.C HTTP client parses every production response");
    result.expectEqual(expectedCases.size(), state.observations.size(), "the live response inventory remains complete");

    const std::size_t headDelimiter = state.headProbe.response.find("\r\n\r\n");
    const std::string headHeaders =
        headDelimiter == std::string::npos ? std::string{} : state.headProbe.response.substr(0, headDelimiter + 4);
    const std::string headBody =
        headDelimiter == std::string::npos ? state.headProbe.response : state.headProbe.response.substr(headDelimiter + 4);
    result.expectTrue(state.headProbe.connected && state.headProbe.complete && !state.headProbe.connectFailed &&
                          headHeaders.starts_with("HTTP/1.1 200 ") &&
                          headHeaders.find("Content-Length: " + std::to_string(IndexBody.size()) + "\r\n") != std::string::npos &&
                          headHeaders.find("Content-Type: text/html; charset=utf-8\r\n") != std::string::npos &&
                          headHeaders.find("Content-Security-Policy:") != std::string::npos &&
                          headHeaders.find("frame-ancestors 'none'") != std::string::npos &&
                          headHeaders.find("X-Content-Type-Options: nosniff\r\n") != std::string::npos &&
                          headHeaders.find("Referrer-Policy: no-referrer\r\n") != std::string::npos && headBody.empty(),
                      "a raw SNode.C stream probe receives exact HEAD metadata and no representation body");

    const std::size_t headBodyDelimiter = state.headBodyProbe.response.find("\r\n\r\n");
    const std::string headBodyHeaders =
        headBodyDelimiter == std::string::npos ? std::string{} : state.headBodyProbe.response.substr(0, headBodyDelimiter + 4);
    const std::string rejectedHeadBody =
        headBodyDelimiter == std::string::npos ? state.headBodyProbe.response : state.headBodyProbe.response.substr(headBodyDelimiter + 4);
    result.expectTrue(state.headBodyProbe.connected && state.headBodyProbe.complete && !state.headBodyProbe.connectFailed &&
                          headBodyHeaders.starts_with("HTTP/1.1 400 ") &&
                          headBodyHeaders.find("Content-Security-Policy:") != std::string::npos && rejectedHeadBody.empty(),
                      "a one-byte HEAD body reaches bounded application policy, is rejected, and emits no response body");
    result.expectTrue(state.chunkedProbe.connected && state.chunkedProbe.complete && !state.chunkedProbe.connectFailed &&
                          state.chunkedProbe.response.find("HTTP/1.1 501 ") != std::string::npos,
                      "SNode.C rejects chunked request transfer before Express route dispatch");

    bool responseContractsMatch = state.observations.size() == expectedCases.size();
    bool securityHeadersPresent = responseContractsMatch;
    bool responseSentinelAbsent = responseContractsMatch;
    for (std::size_t index = 0; index < state.observations.size() && index < expectedCases.size(); ++index) {
        const Observation& observation = state.observations[index];
        const Case& expected = expectedCases[index];
        bool observationMatches = observation.testCase.name == expected.name && observation.status == expected.expectedStatus &&
                                  (!expected.expectedBody.has_value() || observation.body == *expected.expectedBody);
        if (expected.expectedContentType.has_value()) {
            observationMatches = observationMatches && observation.contentType == *expected.expectedContentType;
        }
        if (expected.expectedContentLength.has_value()) {
            observationMatches = observationMatches && observation.contentLength == std::to_string(*expected.expectedContentLength);
        }
        if (!observationMatches) {
            std::cerr << "HTTP contract mismatch in " << expected.name << ": status=" << observation.status
                      << ", body-bytes=" << observation.body.size() << ", content-type=" << observation.contentType
                      << ", content-length=" << observation.contentLength << '\n';
        }
        responseContractsMatch = responseContractsMatch && observationMatches;
        if (expected.expectSecurityHeaders) {
            securityHeadersPresent = securityHeadersPresent &&
                                     observation.contentSecurityPolicy.find("frame-ancestors 'none'") != std::string::npos &&
                                     observation.contentTypeOptions == "nosniff" && observation.referrerPolicy == "no-referrer";
        }
        responseSentinelAbsent = responseSentinelAbsent && observation.responseMaterial.find(SecretSentinel) == std::string::npos;
    }

    result.expectTrue(responseContractsMatch, "live static, bounded-body, rootless, traversal, MIME, endpoint, and Origin responses match");
    const bool everyFailedPipeProbeCompleted =
        std::all_of(state.failedPipeProbes.begin(), state.failedPipeProbes.end(), [](const RawProbeObservation& probe) {
            return probe.connected && probe.complete && !probe.connectFailed;
        });
    result.expectTrue(everyFailedPipeProbeCompleted && state.failedPipeProbeCompletions == FailedPipeProbeCount &&
                          state.failedPipeRouteDispatches == FailedPipeProbeCount && state.failedPipeCleanupDrained,
                      "every disconnected-response probe reaches the real static route and drains FileReader cleanup");
    result.expectTrue(state.failedPipeDescriptorBaseline != 0 && state.failedPipeDescriptorFinal <= state.failedPipeDescriptorBaseline,
                      "repeated failed Response::pipe lifecycles do not accumulate descriptors");
    result.expectTrue(state.survivorOpenAfterPipeFailures && state.survivorCommandAcceptedAfterPipeFailures &&
                          state.survivorOutboundMessages > state.survivorOutboundBeforePipeFailures,
                      "failed static pipes remain connection-local and an unrelated frontend continues processing commands");

    const std::size_t expectedRouteDispatches =
        2U + FailedPipeProbeCount +
        static_cast<std::size_t>(std::count_if(expectedCases.begin(), expectedCases.end(), [](const Case& testCase) {
            return testCase.expectRouteDispatch;
        }));
    result.expectEqual(expectedRouteDispatches,
                       state.routeDispatches,
                       "oversized and chunked bodies are rejected by SNode.C before Express route dispatch");
    result.expectTrue(securityHeadersPresent,
                      "every live static and admission response carries CSP frame-ancestors, nosniff, and no-referrer headers");
    result.expectTrue(responseSentinelAbsent, "the outside-file secret sentinel appears in no AISuite HTTP response");
    result.expectEqual(std::size_t{0},
                       state.authenticationAttempts,
                       "static, malformed-upgrade, and Origin-rejected requests never reach Hello authentication");
    result.expectTrue(state.serviceStayedAtSurvivorBaseline,
                      "HTTP admission creates no session, controller, or journal state beyond the unrelated authenticated survivor");
    result.expectTrue(!state.responseObservedRetainedAdmission,
                      "ordinary HTTP responses create no FrontendConnection before a successful WebSocket upgrade");
    result.expectEqual(std::size_t{0}, state.unexpectedStates, "the live HTTP scenario reports no unexpected state");

    return result.processResult();
}
