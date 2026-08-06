/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/frontend/FrontendService.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;

    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;
    using Json = ai::openai::codex::Json;
    using ai::openai::codex::detail::TransportCallbacks;

    constexpr std::uint64_t TrustedUserId = 7001;

    struct Observations {
        std::vector<frontend::ServerMessage> messages;
        std::vector<std::string> compactJson;
        std::vector<std::string> closeReasons;
    };

    frontend::FrontendConnectionCallbacks callbacksFor(Observations& observations) {
        return {[&observations](const frontend::OutboundMessage& message) {
                    observations.messages.push_back(message.message);
                    observations.compactJson.push_back(message.compactJson);
                    return true;
                },
                [&observations](const std::string& reason) {
                    observations.closeReasons.push_back(reason);
                }};
    }

    frontend::FrontendPeerContext verifiedLocalPeer() {
        frontend::FrontendPeerContext peer;
        peer.transport = frontend::FrontendTransportKind::Unix;
        peer.loopback = true;
        peer.localPeer = true;
        peer.unixUserId = TrustedUserId;
        return peer;
    }

    frontend::FrontendPeerContext remotePeer(std::string address) {
        frontend::FrontendPeerContext peer;
        peer.transport = frontend::FrontendTransportKind::Ipv4;
        peer.loopback = true;
        peer.remoteAddress = std::move(address);
        return peer;
    }

    frontend::ClientMessage legacyHello() {
        return frontend::Hello{std::nullopt, Json::object()};
    }

    frontend::ClientMessage bearerHello() {
        return frontend::Hello{
            std::nullopt,
            Json::object(),
            std::vector{frontend::FrontendCapability::MethodDiscovery, frontend::FrontendCapability::SecurityScopes},
            frontend::AuthenticationCredential{frontend::BearerCredential{"runtime-test-token"}},
        };
    }

    Json command(std::string requestId, std::string method, Json params = Json::object()) {
        return {
            {"protocol", frontend::ProtocolIdentity},
            {"version", frontend::ProtocolVersion},
            {"kind", "command"},
            {"requestId", std::move(requestId)},
            {"method", std::move(method)},
            {"params", std::move(params)},
        };
    }

    const frontend::Welcome* welcome(const Observations& observations) {
        for (const frontend::ServerMessage& message : observations.messages) {
            if (const auto* value = std::get_if<frontend::Welcome>(&message)) {
                return value;
            }
        }
        return nullptr;
    }

    const frontend::Response* response(const Observations& observations, std::string_view requestId) {
        for (auto iterator = observations.messages.rbegin(); iterator != observations.messages.rend(); ++iterator) {
            if (const auto* value = std::get_if<frontend::Response>(&*iterator); value && value->requestId == requestId) {
                return value;
            }
        }
        return nullptr;
    }

    bool successful(const Observations& observations, std::string_view requestId, const Json& expected = Json::object()) {
        const frontend::Response* value = response(observations, requestId);
        return value != nullptr && value->ok && value->result == std::optional<Json>{expected};
    }

    bool
    failed(const Observations& observations, std::string_view requestId, frontend::ErrorCode code, std::string_view messageFragment = {}) {
        const frontend::Response* value = response(observations, requestId);
        return value != nullptr && !value->ok && value->error.has_value() && value->error->code == code &&
               (messageFragment.empty() || value->error->message.find(messageFragment) != std::string::npos);
    }

    class RuntimeActivationRunner {
    public:
        explicit RuntimeActivationRunner(tests::support::TestResult& result)
            : result(result) {
        }

        void start() {
            transport = std::make_shared<tests::codex::FakeTransportState>();
            tests::codex::installInitializingFake(transport, [this](const Json& message, const TransportCallbacks& callbacks) {
                handleProviderRequest(message, callbacks);
            });

            backend::BackendCoreOptions backendOptions;
            backendOptions.initialThreadListLimit = 1;
            core = std::make_unique<FakeBackendCore>(std::move(backendOptions), transport);

            frontend::FrontendServiceOptions serviceOptions;
            serviceOptions.trustedLocalUserId = TrustedUserId;
            serviceOptions.maxOutstandingCommandsPerConnection = 1;
            serviceOptions.authenticator = [](const frontend::FrontendPeerContext&, const frontend::AuthenticationCredential& credential) {
                const auto* bearer = std::get_if<frontend::BearerCredential>(&credential);
                if (bearer == nullptr || bearer->token != "runtime-test-token") {
                    return frontend::AuthenticationResult{
                        frontend::AuthenticationFailure{frontend::AuthenticationFailureCode::AuthenticationFailed}};
                }
                return frontend::AuthenticationResult{frontend::AuthenticationSuccess{frontend::FrontendPrincipal{
                    "default-remote-test",
                    std::vector<frontend::FrontendScope>{frontend::DefaultRemoteScopes.begin(), frontend::DefaultRemoteScopes.end()},
                    "default_remote",
                    false,
                }}};
            };
            service = std::make_unique<frontend::FrontendService>(*core, std::move(serviceOptions));

            local.emplace(service->openConnection(verifiedLocalPeer(), callbacksFor(localObservations)));
            localObserver.emplace(service->openConnection(verifiedLocalPeer(), callbacksFor(localObserverObservations)));
            remote.emplace(service->openConnection(remotePeer("127.0.0.20"), callbacksFor(remoteObservations)));

            expect(local->receive(legacyHello()).accepted(),
                   "a verified same-user Unix peer retains the original credential-free Hello contract");
            expect(localObserver->receive(legacyHello()).accepted(),
                   "a second verified local peer remains an observer in the shared controller universe");
            expect(remote->receive(bearerHello()).accepted(), "a remote connection authenticates through the additive Hello bearer object");

            waitUntil(
                "all three frontend connections complete authentication and Welcome synchronization",
                [this]() {
                    return local->principal().has_value() && localObserver->principal().has_value() && remote->principal().has_value() &&
                           welcome(localObservations) != nullptr && welcome(localObserverObservations) != nullptr &&
                           welcome(remoteObservations) != nullptr;
                },
                [this]() {
                    verifyAuthenticationAndCatalog();
                    acquireController();
                });
        }

        bool isFinished() const noexcept {
            return finished;
        }

        const std::string& waitingStage() const noexcept {
            return waitingDescription;
        }

    private:
        static void defer(std::function<void()> callback) {
            core::EventReceiver::atNextTick(std::move(callback));
        }

        void expect(bool condition, const std::string& message) {
            result.expectTrue(condition, message);
        }

        void
        waitUntil(std::string description, std::function<bool()> predicate, std::function<void()> next, std::size_t remaining = 8'000) {
            waitingDescription = description;
            defer([this,
                   description = std::move(description),
                   predicate = std::move(predicate),
                   next = std::move(next),
                   remaining]() mutable {
                if (finished) {
                    return;
                }
                if (predicate()) {
                    waitingDescription = "advancing after: " + description;
                    next();
                    return;
                }
                if (remaining == 0) {
                    expect(false, description);
                    finish();
                    return;
                }
                waitUntil(std::move(description), std::move(predicate), std::move(next), remaining - 1);
            });
        }

        void verifyAuthenticationAndCatalog() {
            expect(local->principal()->localTrusted && local->principal()->profile == "local_trusted" &&
                       remote->principal()->profile == "default_remote",
                   "legacy verified trust and bearer authentication produce their exact reviewed principals");

            const frontend::Welcome* remoteWelcome = welcome(remoteObservations);
            expect(service->definedMethods().size() == 105 && service->implementedMethods().size() == 105 &&
                       service->availableMethods().size() == 90 && service->permittedMethods(*local->principal()).size() == 90 &&
                       service->permittedMethods(*remote->principal()).size() == 53,
                   "the public service reports defined 105, implemented 105, available 90, local_trusted 90, and default_remote 53");
            expect(remoteWelcome->availableMethods.has_value() && remoteWelcome->availableMethods->size() == 90 &&
                       remoteWelcome->permittedMethods.has_value() && remoteWelcome->permittedMethods->size() == 53,
                   "Welcome freezes the same 90 available and 53 default_remote permitted methods at handshake");
            frontend::Json expectedScopes = frontend::Json::array();
            for (const frontend::FrontendScope scope : frontend::DefaultRemoteScopes) {
                expectedScopes.push_back(std::string(frontend::toString(scope)));
            }
            expect(remoteWelcome->extensions.contains("permittedScopes") &&
                       remoteWelcome->extensions.at("permittedScopes") == expectedScopes,
                   "Welcome freezes the exact default_remote permitted-scope set at handshake");
        }

        void acquireController() {
            expect(local->receive(command("controller", "controller.acquire")).accepted(),
                   "the local trusted observer explicitly acquires the one global controller");
            waitUntil(
                "controller acquisition completes asynchronously",
                [this]() {
                    return response(localObservations, "controller") != nullptr;
                },
                [this]() {
                    const frontend::Response* acquired = response(localObservations, "controller");
                    expect(acquired->ok && acquired->result.has_value() && acquired->result->value("role", "") == "controller" &&
                               acquired->result->contains("controllerSessionId") && service->currentController().has_value(),
                           "controller acquisition returns the exact ControllerResult before lifecycle routing");
                    startProvider();
                });
        }

        void startProvider() {
            expect(local->receive(command("provider-start", "provider.start")).accepted(),
                   "provider.start routes through the native FrontendService lifecycle action");
            waitUntil(
                "provider.start reaches Ready and returns its callback-last response",
                [this]() {
                    return response(localObservations, "provider-start") != nullptr && core->isReady();
                },
                [this]() {
                    expect(successful(localObservations, "provider-start") && transport->startCount == 1,
                           "provider.start invokes the single BackendCore lifecycle and leaves it Ready");
                    testAdditiveProviderResult();
                });
        }

        void testAdditiveProviderResult() {
            modelListBaseline = methodCount("model/list");
            expect(remote->receive(command("model-list", "model.list")).accepted(),
                   "an additive observer method is accepted through exact generated runtime dispatch");
            waitUntil(
                "model.list completes through the typed provider callback",
                [this]() {
                    return response(remoteObservations, "model-list") != nullptr;
                },
                [this]() {
                    expect(methodCount("model/list") == modelListBaseline + 1 &&
                               successful(remoteObservations, "model-list", Json{{"data", Json::array()}}),
                           "model.list reaches the exact provider facade and returns its exact tagged safe result");
                    testPrivilegedDenial();
                });
        }

        void testPrivilegedDenial() {
            outgoingBaseline = transport->outgoing.size();
            expect(remote->receive(command("privileged", "config.batchWrite", Json{{"edits", Json::array()}})).status ==
                       frontend::ConnectionReceiveStatus::Rejected,
                   "default_remote cannot submit a privileged configuration mutation");
            waitUntil(
                "privileged configuration denial is correlated",
                [this]() {
                    return response(remoteObservations, "privileged") != nullptr;
                },
                [this]() {
                    expect(failed(remoteObservations, "privileged", frontend::ErrorCode::PermissionDenied, "required scope") &&
                               transport->outgoing.size() == outgoingBaseline,
                           "privileged denial occurs before BackendCore submission");
                    testConditionalDenial();
                });
        }

        void testConditionalDenial() {
            outgoingBaseline = transport->outgoing.size();
            const frontend::ConnectionReceiveResult denied =
                local->receive(command("conditional", "fs.readFile", Json{{"path", "/tmp/a17b"}}));
            expect(denied.status == frontend::ConnectionReceiveStatus::Rejected && denied.error.has_value() &&
                       denied.error->code == frontend::ErrorCode::UnknownMethod,
                   "an implemented conditional method remains unavailable while its deployment gate is disabled");
            expect(transport->outgoing.size() == outgoingBaseline,
                   "default-disabled conditional dispatch returns unknown_method without reaching BackendCore");
            defer([this]() {
                testOverlappingAccountReads();
            });
        }

        void testOverlappingAccountReads() {
            expect(remote->receive(command("account-omitted", "account.read")).accepted() &&
                       remote->receive(command("account-false", "account.read", Json{{"refreshToken", false}})).status ==
                           frontend::ConnectionReceiveStatus::Rejected,
                   "the omitted observer form is accepted while outstanding capacity remains connection-local");
            waitUntil(
                "overlapping account.read commands produce one result and one capacity rejection",
                [this]() {
                    return response(remoteObservations, "account-omitted") != nullptr &&
                           response(remoteObservations, "account-false") != nullptr;
                },
                [this]() {
                    expect(successful(remoteObservations, "account-omitted", Json{{"requiresOpenaiAuth", false}}) &&
                               failed(remoteObservations, "account-false", frontend::ErrorCode::CapacityExceeded),
                           "an accepted observer account.read returns exactly while the overlapping request is capacity bounded");
                    testFalseAccountReadRetry();
                });
        }

        void testFalseAccountReadRetry() {
            expect(remote->receive(command("account-false-retry", "account.read", Json{{"refreshToken", false}})).accepted(),
                   "refreshToken=false remains observer-readable after capacity releases");
            waitUntil(
                "refreshToken=false account.read completes",
                [this]() {
                    return response(remoteObservations, "account-false-retry") != nullptr;
                },
                [this]() {
                    expect(successful(remoteObservations, "account-false-retry", Json{{"requiresOpenaiAuth", false}}),
                           "refreshToken=false uses the exact observer policy and result");
                    testDeniedTokenRefreshes();
                });
        }

        void testDeniedTokenRefreshes() {
            accountReadBaseline = methodCount("account/read");
            expect(remote->receive(command("account-true-remote", "account.read", Json{{"refreshToken", true}})).status ==
                       frontend::ConnectionReceiveStatus::Rejected,
                   "refreshToken=true is denied to default_remote without AccountManagement");
            expect(localObserver->receive(command("account-true-observer", "account.read", Json{{"refreshToken", true}})).status ==
                       frontend::ConnectionReceiveStatus::Rejected,
                   "refreshToken=true is denied to a fully scoped observer that does not own the controller");
            waitUntil(
                "both stronger account.read branches are denied before provider submission",
                [this]() {
                    return response(remoteObservations, "account-true-remote") != nullptr &&
                           response(localObserverObservations, "account-true-observer") != nullptr;
                },
                [this]() {
                    expect(failed(remoteObservations, "account-true-remote", frontend::ErrorCode::PermissionDenied, "required scope") &&
                               failed(localObserverObservations,
                                      "account-true-observer",
                                      frontend::ErrorCode::PermissionDenied,
                                      "current controller") &&
                               methodCount("account/read") == accountReadBaseline,
                           "the stronger account.read branch checks scope then controller before provider submission");
                    testPermittedTokenRefresh();
                });
        }

        void testPermittedTokenRefresh() {
            expect(local->receive(command("account-true-controller", "account.read", Json{{"refreshToken", true}})).accepted(),
                   "the fully scoped current controller may request token refresh");
            waitUntil(
                "the permitted token-refresh account.read completes",
                [this]() {
                    return response(localObservations, "account-true-controller") != nullptr;
                },
                [this]() {
                    expect(successful(localObservations, "account-true-controller", Json{{"requiresOpenaiAuth", false}}),
                           "the fully permitted parameter-sensitive branch returns its exact result");
                    testDeferredProviderCommand();
                });
        }

        void testDeferredProviderCommand() {
            deferNextModelList = true;
            expect(remote->receive(command("deferred-model", "model.list")).accepted(),
                   "one deferred additive provider command occupies the frontend correlation slot");
            waitUntil(
                "the fake provider retains exactly one deterministic completion",
                [this]() {
                    return deferredProviderId.has_value();
                },
                [this]() {
                    expect(remote->receive(command("deferred-model", "model.list")).status == frontend::ConnectionReceiveStatus::Rejected,
                           "a duplicate active request ID is rejected before capacity accounting");
                    expect(remote->receive(command("outstanding-model", "model.list")).status ==
                               frontend::ConnectionReceiveStatus::Rejected,
                           "a distinct request is rejected by the configured outstanding-command bound");
                    waitUntil(
                        "duplicate and outstanding failures are delivered",
                        [this]() {
                            return failed(remoteObservations, "deferred-model", frontend::ErrorCode::DuplicateRequestId) &&
                                   response(remoteObservations, "outstanding-model") != nullptr;
                        },
                        [this]() {
                            expect(failed(remoteObservations, "deferred-model", frontend::ErrorCode::DuplicateRequestId) &&
                                       failed(remoteObservations, "outstanding-model", frontend::ErrorCode::CapacityExceeded),
                                   "duplicate and outstanding failures are terminal, correlated, and connection-local");
                            completeDeferredModelList();
                            waitUntil(
                                "the original deferred command completes",
                                [this]() {
                                    return successful(remoteObservations, "deferred-model", Json{{"data", Json::array()}});
                                },
                                [this]() {
                                    expect(true, "the original deferred command receives exactly one successful provider result");
                                    restartProvider();
                                });
                        });
                });
        }

        void restartProvider() {
            startsBeforeLifecycle = transport->startCount;
            stopsBeforeLifecycle = transport->stopCount;
            expect(local->receive(command("provider-restart", "provider.restart")).accepted(),
                   "provider.restart routes through the native lifecycle table");
            waitUntil(
                "provider.restart returns and the BackendCore reaches Ready",
                [this]() {
                    return response(localObservations, "provider-restart") != nullptr && core->isReady() &&
                           transport->startCount == startsBeforeLifecycle + 1 && transport->stopCount == stopsBeforeLifecycle + 1;
                },
                [this]() {
                    expect(successful(localObservations, "provider-restart"),
                           "provider.restart reuses the single BackendCore stop/start recovery path");
                    stopProvider();
                });
        }

        void stopProvider() {
            stopsBeforeLifecycle = transport->stopCount;
            expect(local->receive(command("provider-stop", "provider.stop")).accepted(),
                   "provider.stop routes through the native lifecycle table");
            waitUntil(
                "provider.stop returns and the BackendCore reaches Stopped",
                [this]() {
                    return response(localObservations, "provider-stop") != nullptr && !core->isReady() &&
                           transport->stopCount == stopsBeforeLifecycle + 1;
                },
                [this]() {
                    expect(successful(localObservations, "provider-stop") && service->isOpen() && local->isOpen() && remote->isOpen(),
                           "provider.stop completes once while FrontendService and all connections remain alive");
                    finish();
                });
        }

        void handleProviderRequest(const Json& message, const TransportCallbacks& callbacks) {
            const auto method = message.find("method");
            const auto id = message.find("id");
            if (method == message.end() || !method->is_string() || id == message.end()) {
                return;
            }
            if (*method == "thread/list") {
                tests::codex::inject(
                    callbacks,
                    Json{{"id", *id}, {"result", {{"data", Json::array()}, {"nextCursor", nullptr}, {"backwardsCursor", nullptr}}}});
            } else if (*method == "model/list") {
                if (std::exchange(deferNextModelList, false)) {
                    deferredProviderId = *id;
                    deferredProviderCallbacks = callbacks;
                } else {
                    tests::codex::inject(callbacks, Json{{"id", *id}, {"result", {{"data", Json::array()}}}});
                }
            } else if (*method == "account/read") {
                tests::codex::inject(callbacks, Json{{"id", *id}, {"result", {{"requiresOpenaiAuth", false}}}});
            }
        }

        std::size_t methodCount(std::string_view method) const {
            return static_cast<std::size_t>(
                std::count_if(transport->outgoing.begin(), transport->outgoing.end(), [method](const Json& value) {
                    const auto found = value.find("method");
                    return found != value.end() && found->is_string() && found->get_ref<const std::string&>() == method;
                }));
        }

        void completeDeferredModelList() {
            if (!deferredProviderId.has_value()) {
                return;
            }
            tests::codex::inject(deferredProviderCallbacks, Json{{"id", *deferredProviderId}, {"result", {{"data", Json::array()}}}});
            deferredProviderId.reset();
            deferredProviderCallbacks = {};
        }

        void finish() {
            if (finished) {
                return;
            }
            finished = true;
            if (service) {
                service->close("runtime activation runner finished");
            }
            if (core) {
                core->stop();
            }
            local.reset();
            localObserver.reset();
            remote.reset();
            service.reset();
            core.reset();
            core::SNodeC::stop();
        }

        tests::support::TestResult& result;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        std::unique_ptr<FakeBackendCore> core;
        std::unique_ptr<frontend::FrontendService> service;
        Observations localObservations;
        Observations localObserverObservations;
        Observations remoteObservations;
        std::optional<frontend::FrontendConnection> local;
        std::optional<frontend::FrontendConnection> localObserver;
        std::optional<frontend::FrontendConnection> remote;
        bool deferNextModelList = false;
        std::optional<Json> deferredProviderId;
        TransportCallbacks deferredProviderCallbacks;
        std::size_t modelListBaseline = 0;
        std::size_t accountReadBaseline = 0;
        std::size_t outgoingBaseline = 0;
        std::size_t startsBeforeLifecycle = 0;
        std::size_t stopsBeforeLifecycle = 0;
        bool finished = false;
        std::string waitingDescription = "not started";
    };
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    int returnCode = tests::support::cTestSkipReturnCode;

    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexFrontendRuntimeActivationTest");
    } else {
        core::SNodeC::init(argc, argv);
        bool timedOut = false;
        RuntimeActivationRunner runner(result);
        [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
            [&timedOut]() {
                timedOut = true;
                core::SNodeC::stop();
            },
            utils::Timeval({10, 0}));
        runner.start();
        const int eventLoopResult = core::SNodeC::start(utils::Timeval({12, 0}));
        result.expectTrue(!timedOut,
                          "frontend runtime activation completes before the watchdog (last stage: " + runner.waitingStage() + ")");
        result.expectTrue(runner.isFinished(), "frontend runtime activation reaches a clean terminal state");
        result.expectEqual(0, eventLoopResult, "frontend runtime activation event loop exits cleanly");

        core::SNodeC::free();
        returnCode = result.processResult();
    }
    return returnCode;
}
