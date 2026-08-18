/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "ai/openai/codex/frontend/internal/server/ServerCore.h"
#include "support/TestResult.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace model = ai::openai::codex::frontend::internal::model;
    namespace server = ai::openai::codex::frontend::internal::server;

    class Backend final : public server::BackendPort {
    public:
        [[nodiscard]] bool providerReady() const noexcept override {
            return ready;
        }

        [[nodiscard]] model::CanonicalSnapshot snapshot() const override {
            return state;
        }

        [[nodiscard]] server::BackendSubmitStatus submit(server::BackendInvocation invocation) override {
            ++submissions;
            if (core && (invocation.token.method == generated::MethodId::ControllerAcquire ||
                         invocation.token.method == generated::MethodId::ControllerRelease)) {
                const bool acquire = invocation.token.method == generated::MethodId::ControllerAcquire;
                static_cast<void>(core->complete(server::BackendCompletion{
                    invocation.token,
                    server::BackendCommandSuccess{generated::makeResult(
                        invocation.token.method,
                        acquire ? frontend::Json{{"controllerSessionId", invocation.session.value()}, {"role", "controller"}}
                                : frontend::Json{{"role", "observer"}})}}));
            }
            return server::BackendSubmitStatus::Accepted;
        }

        void bind(server::ServerCore& boundCore) noexcept override {
            core = &boundCore;
        }

        void unbind(server::ServerCore& boundCore) noexcept override {
            if (core == &boundCore) {
                core = nullptr;
            }
        }

        [[nodiscard]] bool performProviderLifecycleAction(server::ProviderLifecycleAction) override {
            return true;
        }

        void sessionOpened(const model::SessionIdentity&, const frontend::FrontendPrincipal& principal) override {
            principalIds.push_back(principal.id);
        }

        model::CanonicalSnapshot state;
        bool ready = true;
        std::size_t submissions = 0;
        std::vector<std::string> principalIds;
        server::ServerCore* core = nullptr;
    };

    struct Sink {
        std::vector<frontend::ServerMessage> messages;
        std::vector<server::ConnectionClose> closes;

        server::ConnectionCallbacks callbacks() {
            return {[this](server::SerializedServerMessage outbound) {
                        messages.push_back(std::move(outbound.message));
                        return true;
                    },
                    [this](const server::ConnectionClose& close) {
                        closes.push_back(close);
                    }};
        }
    };

    const frontend::CommandError* latestCommandError(const Sink& sink) {
        if (sink.messages.empty()) {
            return nullptr;
        }
        const auto* response = std::get_if<frontend::Response>(&sink.messages.back());
        return response && response->error ? &*response->error : nullptr;
    }

    const frontend::ProtocolErrorMessage* latestProtocolError(const Sink& sink) {
        return sink.messages.empty() ? nullptr : std::get_if<frontend::ProtocolErrorMessage>(&sink.messages.back());
    }

    frontend::FrontendPrincipal allScopesPrincipal(std::string id = "security-principal") {
        frontend::FrontendPrincipal principal;
        principal.id = std::move(id);
        principal.profile = "test";
        principal.scopes.assign(frontend::LocalTrustedScopes.begin(), frontend::LocalTrustedScopes.end());
        return principal;
    }

    std::map<std::string, frontend::Json, std::less<>> minimalParameters() {
        const std::filesystem::path fixture =
            std::filesystem::path(__FILE__).parent_path() / "fixtures" / "frontend-protocol-v1.generated.json";
        std::ifstream input(fixture);
        frontend::Json document;
        if (input) {
            input >> document;
        }
        std::map<std::string, frontend::Json, std::less<>> parameters;
        if (document.is_object() && document.contains("methods") && document.at("methods").is_array()) {
            for (const frontend::Json& method : document.at("methods")) {
                parameters.emplace(method.at("method").get<std::string>(), method.at("minimalParams"));
            }
        }
        return parameters;
    }

    std::optional<generated::DefinedCommand> command(std::string requestId,
                                                     const generated::MethodMetadata& metadata,
                                                     const std::map<std::string, frontend::Json, std::less<>>& parameters) {
        const auto found = parameters.find(std::string(metadata.method));
        if (found == parameters.end()) {
            return std::nullopt;
        }
        const frontend::Json wire{{"protocol", frontend::ProtocolIdentity},
                                  {"version", frontend::ProtocolVersion},
                                  {"kind", frontend::kind::Command},
                                  {"requestId", std::move(requestId)},
                                  {"method", metadata.method},
                                  {"params", found->second}};
        const auto decoded = frontend::Codec::decodeDefinedCommand(wire);
        return decoded ? std::optional<generated::DefinedCommand>{decoded.value()} : std::nullopt;
    }

    server::ServerCoreOptions conditionalOptions(frontend::FrontendPrincipal principal, bool permitPolicy = true) {
        server::ServerCoreOptions options;
        options.authenticator = [principal = std::move(principal)](const auto&, const auto&) -> frontend::AuthenticationResult {
            return frontend::AuthenticationSuccess{principal};
        };
        options.maxInboundBurst = 1000;
        options.maxInboundMessagesPerSecond = 1000;
        options.enableFilesystemReadMethods = true;
        options.enableFilesystemWriteMethods = true;
        options.enableCommandExecutionMethods = true;
        options.filesystemReadPolicy = [permitPolicy](const auto&, std::string_view, const frontend::Json&) {
            return permitPolicy;
        };
        options.filesystemWritePolicy = options.filesystemReadPolicy;
        options.commandExecutionPolicy = options.filesystemReadPolicy;
        return options;
    }

    void testLocalTrust(tests::support::TestResult& result) {
        Backend backend;
        server::ServerCoreOptions options;
        options.trustedLocalUserId = 42;
        server::ServerCore core(backend, std::move(options));
        core.start();

        frontend::FrontendPeerContext unixPeer;
        unixPeer.transport = frontend::FrontendTransportKind::Unix;
        unixPeer.localPeer = true;
        unixPeer.unixUserId = 42;
        Sink trusted;
        const auto trustedConnection = core.openConnection(unixPeer, trusted.callbacks());
        result.expectTrue(core.receive(*trustedConnection, frontend::ClientMessage{frontend::Hello{}}).accepted() &&
                              backend.principalIds == std::vector<std::string>{"unix-user-42"},
                          "verified trust requires an explicitly configured matching Unix uid and uses the frozen principal identity");

        frontend::FrontendPeerContext tcpPeer = unixPeer;
        tcpPeer.transport = frontend::FrontendTransportKind::Ipv4;
        Sink rejected;
        const auto rejectedConnection = core.openConnection(tcpPeer, rejected.callbacks());
        const auto rejectedResult = core.receive(*rejectedConnection, frontend::ClientMessage{frontend::Hello{}});
        const auto* rejectedError =
            !rejected.messages.empty() ? std::get_if<frontend::ProtocolErrorMessage>(&rejected.messages.front()) : nullptr;
        result.expectTrue(rejectedResult.status == server::ReceiveStatus::Closing && rejectedError &&
                              rejectedError->code == frontend::ErrorCode::AuthenticationRequired && rejectedError->closeConnection,
                          "matching local peer facts on a non-Unix transport do not grant verified-local trust");

        Backend insecureBackend;
        server::ServerCoreOptions insecureOptions;
        insecureOptions.allowVerifiedLocalTrust = false;
        insecureOptions.allowInsecureLocalTrust = true;
        server::ServerCore insecure(insecureBackend, std::move(insecureOptions));
        insecure.start();
        frontend::FrontendPeerContext insecurePeer;
        insecurePeer.transport = frontend::FrontendTransportKind::Unix;
        Sink insecureSink;
        const auto insecureConnection = insecure.openConnection(insecurePeer, insecureSink.callbacks());
        result.expectTrue(insecure.receive(*insecureConnection, frontend::ClientMessage{frontend::Hello{}}).accepted() &&
                              insecureBackend.principalIds == std::vector<std::string>{"insecure-local-override"},
                          "the explicit insecure override is limited to Unix and has its own auditable principal identity");
    }

    void testAuthenticationBudgetAndSanitization(tests::support::TestResult& result) {
        Backend backend;
        std::uint64_t now = 0;
        std::size_t authenticatorCalls = 0;
        server::ServerCoreOptions options;
        options.maxConnections = 2;
        options.maximumFailedAuthenticationsPerPeer = 1;
        options.failedAuthenticationWindowMs = 100;
        options.monotonicClockMs = [&now] {
            return now;
        };
        options.authenticator = [&authenticatorCalls](const auto&, const auto&) -> frontend::AuthenticationResult {
            ++authenticatorCalls;
            return frontend::AuthenticationFailure{frontend::AuthenticationFailureCode::AuthenticationFailed};
        };
        server::ServerCore core(backend, std::move(options));
        core.start();

        const auto attempt =
            [&core](std::string address, Sink& sink, frontend::FrontendTransportKind transport = frontend::FrontendTransportKind::Ipv4) {
                frontend::FrontendPeerContext peer;
                peer.transport = transport;
                peer.remoteAddress = std::move(address);
                const auto connection = core.openConnection(std::move(peer), sink.callbacks());
                return core.receive(*connection, frontend::ClientMessage{frontend::Hello{}});
            };

        Sink first;
        Sink samePeer;
        (void) attempt("192.0.2.1:4000", first);
        (void) attempt("192.0.2.1:4001", samePeer);
        const auto* limited =
            !samePeer.messages.empty() ? std::get_if<frontend::ProtocolErrorMessage>(&samePeer.messages.front()) : nullptr;
        result.expectTrue(authenticatorCalls == 1 && limited && limited->code == frontend::ErrorCode::RateLimited,
                          "failed-authentication accounting strips ephemeral ports and rejects before invoking the authenticator");

        now = 101;
        Sink expired;
        (void) attempt("192.0.2.1:4002", expired);
        Sink secondKey;
        (void) attempt("192.0.2.2:4000", secondKey);
        Sink accountingFull;
        (void) attempt("192.0.2.3:4000", accountingFull);
        const auto* full =
            !accountingFull.messages.empty() ? std::get_if<frontend::ProtocolErrorMessage>(&accountingFull.messages.front()) : nullptr;
        result.expectTrue(authenticatorCalls == 3 && full && full->code == frontend::ErrorCode::RateLimited,
                          "failure windows expire deterministically and one-off peer accounting is bounded by maxConnections");

        now = 202;
        Sink rawIpv6;
        Sink bracketedIpv6;
        (void) attempt("2001:db8::1", rawIpv6, frontend::FrontendTransportKind::Ipv6);
        (void) attempt("[2001:db8::1]:4400", bracketedIpv6, frontend::FrontendTransportKind::Ipv6);
        const auto* ipv6Limited =
            !bracketedIpv6.messages.empty() ? std::get_if<frontend::ProtocolErrorMessage>(&bracketedIpv6.messages.front()) : nullptr;
        result.expectTrue(authenticatorCalls == 4 && ipv6Limited && ipv6Limited->code == frontend::ErrorCode::RateLimited,
                          "raw and bracketed IPv6 peer forms share one failed-authentication admission key");

        now = 303;
        Sink rfcomm;
        Sink rfcommTls;
        (void) attempt("01:23:45:67:89:AB:7", rfcomm, frontend::FrontendTransportKind::Rfcomm);
        (void) attempt("01:23:45:67:89:AB:19", rfcommTls, frontend::FrontendTransportKind::RfcommTls);
        const auto* rfcommLimited =
            !rfcommTls.messages.empty() ? std::get_if<frontend::ProtocolErrorMessage>(&rfcommTls.messages.front()) : nullptr;
        result.expectTrue(authenticatorCalls == 5 && rfcommLimited && rfcommLimited->code == frontend::ErrorCode::RateLimited,
                          "RFCOMM and RFCOMM/TLS channel forms share one Bluetooth-address failed-authentication key");

        Backend sanitizedBackend;
        server::ServerCore sanitized(sanitizedBackend);
        sanitized.start();
        Sink malformedPreauthenticationSink;
        const auto malformedPreauthenticationConnection = sanitized.openConnection({}, malformedPreauthenticationSink.callbacks());
        const frontend::Json malformedPreauthenticationCommand{{"protocol", frontend::ProtocolIdentity},
                                                               {"version", frontend::ProtocolVersion},
                                                               {"kind", frontend::kind::Command},
                                                               {"requestId", "preauthentication-malformed"},
                                                               {"method", "fs.readFile"},
                                                               {"params", frontend::Json{{"path", 42}}}};
        const auto malformedPreauthenticationResult =
            sanitized.receive(*malformedPreauthenticationConnection, malformedPreauthenticationCommand);
        const auto* malformedPreauthenticationError = latestProtocolError(malformedPreauthenticationSink);
        result.expectTrue(malformedPreauthenticationResult.status == server::ReceiveStatus::Closing && malformedPreauthenticationError &&
                              malformedPreauthenticationError->code == frontend::ErrorCode::AuthenticationRequired &&
                              !malformedPreauthenticationError->requestId,
                          "authentication precedes command method and parameter validation for a current protocol envelope");

        Sink sanitizedSink;
        const auto connection = sanitized.openConnection({}, sanitizedSink.callbacks());
        const auto failure = sanitized.receiveError(
            *connection, frontend::CodecError{frontend::ErrorCode::UnknownMethod, "privileged method detail", false, {}, {}, {}});
        const auto* message =
            !sanitizedSink.messages.empty() ? std::get_if<frontend::ProtocolErrorMessage>(&sanitizedSink.messages.front()) : nullptr;
        result.expectTrue(failure.status == server::ReceiveStatus::Closing && message &&
                              message->code == frontend::ErrorCode::InvalidField &&
                              message->message == "frontend handshake message is invalid" && message->closeConnection &&
                              message->supportedVersions == std::vector<std::uint32_t>{frontend::ProtocolVersion},
                          "pre-authentication failures close and collapse privileged codec vocabulary to the frozen ceiling");
    }

    void testRateScopesAndController(tests::support::TestResult& result) {
        Backend rateBackend;
        std::uint64_t now = 0;
        server::ServerCoreOptions rateOptions;
        rateOptions.maxInboundBurst = 1;
        rateOptions.maxInboundMessagesPerSecond = 2;
        rateOptions.monotonicClockMs = [&now] {
            return now;
        };
        rateOptions.authenticator = [](const auto&, const auto&) -> frontend::AuthenticationResult {
            return frontend::AuthenticationSuccess{allScopesPrincipal("rate-principal")};
        };
        server::ServerCore rateCore(rateBackend, std::move(rateOptions));
        rateCore.start();
        Sink rateSink;
        const auto rateConnection = rateCore.openConnection({}, rateSink.callbacks());
        (void) rateCore.receive(*rateConnection, frontend::ClientMessage{frontend::Hello{}});
        now = 500;
        generated::DefinedCommand first{"rate-1", generated::makeParameters(generated::MethodId::SnapshotGet, frontend::Json::object())};
        generated::DefinedCommand second{"rate-2", generated::makeParameters(generated::MethodId::SnapshotGet, frontend::Json::object())};
        const bool refilled = rateCore.receiveDefinedCommand(*rateConnection, first).accepted();
        const auto limited = rateCore.receiveDefinedCommand(*rateConnection, second);
        const auto* rateError =
            !rateSink.messages.empty() ? std::get_if<frontend::ProtocolErrorMessage>(&rateSink.messages.back()) : nullptr;
        result.expectTrue(refilled && limited.status == server::ReceiveStatus::Closing && rateError &&
                              rateError->code == frontend::ErrorCode::RateLimited,
                          "the millitoken bucket refills elapsedMs*messagesPerSecond, caps at burst, and charges 1000 per message");

        Backend controlBackend;
        server::ServerCoreOptions controlOptions;
        controlOptions.authenticator = [](const auto&, const auto&) -> frontend::AuthenticationResult {
            return frontend::AuthenticationSuccess{allScopesPrincipal("controller-principal")};
        };
        server::ServerCore controlCore(controlBackend, std::move(controlOptions));
        controlCore.start();
        Sink controlSink;
        const auto controlConnection = controlCore.openConnection({}, controlSink.callbacks());
        (void) controlCore.receive(*controlConnection, frontend::ClientMessage{frontend::Hello{}});
        generated::DefinedCommand startBeforeController{
            "thread-before-controller", generated::makeParameters(generated::MethodId::ThreadStart, frontend::Json::object())};
        const auto denied = controlCore.receiveDefinedCommand(*controlConnection, startBeforeController);
        generated::DefinedCommand acquire{"controller",
                                          generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
        generated::DefinedCommand startAfterController{
            "thread-after-controller", generated::makeParameters(generated::MethodId::ThreadStart, frontend::Json::object())};
        const bool acquired = controlCore.receiveDefinedCommand(*controlConnection, acquire).accepted();
        const bool submitted = controlCore.receiveDefinedCommand(*controlConnection, startAfterController).accepted();
        result.expectTrue(
            denied.status == server::ReceiveStatus::Rejected && acquired && submitted && controlBackend.submissions == 2,
            "scope possession does not imply controller ownership, while explicit acquisition enables controller-required dispatch");
    }

    void testConditionalMethods(tests::support::TestResult& result) {
        const auto parameters = minimalParameters();
        std::vector<const generated::MethodMetadata*> conditional;
        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            if (!metadata.defaultEnabled) {
                conditional.push_back(&metadata);
            }
        }

        const auto connect = [](server::ServerCore& core, Sink& sink, bool acquireController) {
            core.start();
            const auto connection = core.openConnection({}, sink.callbacks());
            if (!connection || !core.receive(*connection, frontend::ClientMessage{frontend::Hello{}}).accepted()) {
                return std::optional<server::ConnectionIdentity>{};
            }
            if (acquireController) {
                generated::DefinedCommand acquire{
                    "conditional-controller", generated::makeParameters(generated::MethodId::ControllerAcquire, frontend::Json::object())};
                if (!core.receiveDefinedCommand(*connection, acquire).accepted()) {
                    return std::optional<server::ConnectionIdentity>{};
                }
            }
            return connection;
        };

        Backend acceptedBackend;
        server::ServerCore acceptedCore(acceptedBackend, conditionalOptions(allScopesPrincipal("conditional-accepted")));
        Sink acceptedSink;
        const auto acceptedConnection = connect(acceptedCore, acceptedSink, true);

        Backend disabledBackend;
        server::ServerCoreOptions disabledOptions = conditionalOptions(allScopesPrincipal("conditional-disabled"));
        disabledOptions.enableFilesystemReadMethods = false;
        disabledOptions.enableFilesystemWriteMethods = false;
        disabledOptions.enableCommandExecutionMethods = false;
        server::ServerCore disabledCore(disabledBackend, std::move(disabledOptions));
        Sink disabledSink;
        const auto disabledConnection = connect(disabledCore, disabledSink, true);
        const frontend::Json malformedDisabledCommand{{"protocol", frontend::ProtocolIdentity},
                                                      {"version", frontend::ProtocolVersion},
                                                      {"kind", frontend::kind::Command},
                                                      {"requestId", "conditional-disabled-malformed"},
                                                      {"method", "fs.readFile"},
                                                      {"params", frontend::Json{{"path", 42}}}};
        const auto malformedDisabledResult =
            disabledConnection ? disabledCore.receive(*disabledConnection, malformedDisabledCommand) : server::ReceiveResult{};
        const frontend::ProtocolErrorMessage* malformedDisabledError = latestProtocolError(disabledSink);
        const bool disabledPrecedesSchema =
            malformedDisabledResult.status == server::ReceiveStatus::Rejected && malformedDisabledError &&
            malformedDisabledError->code == frontend::ErrorCode::UnknownMethod &&
            malformedDisabledError->requestId == std::optional<std::string>{"conditional-disabled-malformed"};
        frontend::Json invalidRequestDisabledCommand = malformedDisabledCommand;
        invalidRequestDisabledCommand["requestId"] = "";
        const auto invalidRequestDisabledResult =
            disabledConnection ? disabledCore.receive(*disabledConnection, invalidRequestDisabledCommand) : server::ReceiveResult{};
        const frontend::ProtocolErrorMessage* invalidRequestDisabledError = latestProtocolError(disabledSink);
        const bool commandEnvelopePrecedesDeployment = invalidRequestDisabledResult.status == server::ReceiveStatus::Rejected &&
                                                       invalidRequestDisabledError &&
                                                       invalidRequestDisabledError->code == frontend::ErrorCode::InvalidField;

        Backend wrongProtocolBackend;
        server::ServerCoreOptions wrongProtocolOptions = conditionalOptions(allScopesPrincipal("conditional-wrong-protocol"));
        wrongProtocolOptions.enableFilesystemReadMethods = false;
        server::ServerCore wrongProtocolCore(wrongProtocolBackend, std::move(wrongProtocolOptions));
        Sink wrongProtocolSink;
        const auto wrongProtocolConnection = connect(wrongProtocolCore, wrongProtocolSink, false);
        frontend::Json wrongProtocolCommand = malformedDisabledCommand;
        wrongProtocolCommand["protocol"] = "wrong.frontend.protocol";
        const auto wrongProtocolResult =
            wrongProtocolConnection ? wrongProtocolCore.receive(*wrongProtocolConnection, wrongProtocolCommand) : server::ReceiveResult{};
        const frontend::ProtocolErrorMessage* wrongProtocolError = latestProtocolError(wrongProtocolSink);
        const bool protocolEnvelopePrecedesDeployment = wrongProtocolResult.status == server::ReceiveStatus::Closing &&
                                                        wrongProtocolError &&
                                                        wrongProtocolError->code == frontend::ErrorCode::WrongProtocol;

        Backend policyBackend;
        policyBackend.ready = false;
        server::ServerCore policyCore(policyBackend, conditionalOptions(allScopesPrincipal("conditional-policy"), false));
        Sink policySink;
        const auto policyConnection = connect(policyCore, policySink, true);

        bool policyReceivedExtensions = false;
        Backend extensionPolicyBackend;
        extensionPolicyBackend.ready = false;
        server::ServerCoreOptions extensionPolicyOptions = conditionalOptions(allScopesPrincipal("conditional-policy-extensions"));
        extensionPolicyOptions.filesystemReadPolicy =
            [&policyReceivedExtensions](const auto&, std::string_view method, const frontend::Json& policyParameters) {
                policyReceivedExtensions = method == "fs.readFile" && policyParameters.value("path", "") == "/tmp/policy" &&
                                           policyParameters.value("traceNote", "") == "validated-extension";
                return true;
            };
        server::ServerCore extensionPolicyCore(extensionPolicyBackend, std::move(extensionPolicyOptions));
        Sink extensionPolicySink;
        const auto extensionPolicyConnection = connect(extensionPolicyCore, extensionPolicySink, true);
        const frontend::Json extensionPolicyCommand{
            {"protocol", frontend::ProtocolIdentity},
            {"version", frontend::ProtocolVersion},
            {"kind", frontend::kind::Command},
            {"requestId", "conditional-policy-extension"},
            {"method", "fs.readFile"},
            {"params", frontend::Json{{"path", "/tmp/policy"}, {"traceNote", "validated-extension"}}}};
        const auto extensionPolicyResult = extensionPolicyConnection
                                               ? extensionPolicyCore.receive(*extensionPolicyConnection, extensionPolicyCommand)
                                               : server::ReceiveResult{};
        const frontend::CommandError* extensionPolicyError = latestCommandError(extensionPolicySink);
        const bool completePolicyParameters = extensionPolicyResult.status == server::ReceiveStatus::Rejected && policyReceivedExtensions &&
                                              extensionPolicyError && extensionPolicyError->code == frontend::ErrorCode::BackendUnavailable;

        frontend::FrontendPrincipal observeOnly;
        observeOnly.id = "conditional-scope";
        observeOnly.profile = "test";
        observeOnly.scopes = {frontend::FrontendScope::Observe};
        Backend scopeBackend;
        scopeBackend.ready = false;
        server::ServerCore scopeCore(scopeBackend, conditionalOptions(std::move(observeOnly)));
        Sink scopeSink;
        const auto scopeConnection = connect(scopeCore, scopeSink, false);

        Backend providerBackend;
        providerBackend.ready = false;
        server::ServerCore providerCore(providerBackend, conditionalOptions(allScopesPrincipal("conditional-provider")));
        Sink providerSink;
        const auto providerConnection = connect(providerCore, providerSink, true);

        Backend controllerBackend;
        server::ServerCore controllerCore(controllerBackend, conditionalOptions(allScopesPrincipal("conditional-controller-missing")));
        Sink controllerSink;
        const auto controllerConnection = connect(controllerCore, controllerSink, false);

        std::size_t accepted = 0;
        std::size_t disabledRejected = 0;
        std::size_t policyRejected = 0;
        std::size_t scopeRejected = 0;
        std::size_t providerRejected = 0;
        std::size_t controllerCorrect = 0;
        for (std::size_t index = 0; index < conditional.size(); ++index) {
            const generated::MethodMetadata& metadata = *conditional[index];
            const auto invoke = [&](server::ServerCore& core,
                                    const std::optional<server::ConnectionIdentity>& connection,
                                    std::string_view suffix) {
                const auto defined = command("conditional-" + std::to_string(index) + "-" + std::string(suffix), metadata, parameters);
                return defined && connection ? core.receiveDefinedCommand(*connection, *defined).accepted() : false;
            };
            accepted += invoke(acceptedCore, acceptedConnection, "accepted") ? 1U : 0U;
            disabledRejected += !invoke(disabledCore, disabledConnection, "disabled") ? 1U : 0U;
            policyRejected += !invoke(policyCore, policyConnection, "policy") ? 1U : 0U;
            scopeRejected += !invoke(scopeCore, scopeConnection, "scope") ? 1U : 0U;
            providerRejected += !invoke(providerCore, providerConnection, "provider") ? 1U : 0U;
            const bool withoutController = invoke(controllerCore, controllerConnection, "controller");
            controllerCorrect += withoutController != metadata.controllerRequired ? 1U : 0U;
        }

        const frontend::ProtocolErrorMessage* disabledError = latestProtocolError(disabledSink);
        const frontend::CommandError* policyError = latestCommandError(policySink);
        const frontend::CommandError* scopeError = latestCommandError(scopeSink);
        const frontend::CommandError* providerError = latestCommandError(providerSink);

        const auto controllerMethod = std::find_if(conditional.begin(), conditional.end(), [](const auto* metadata) {
            return metadata->controllerRequired;
        });
        Backend orderedControllerBackend;
        orderedControllerBackend.ready = false;
        server::ServerCore orderedControllerCore(
            orderedControllerBackend, conditionalOptions(allScopesPrincipal("conditional-ordered-controller")));
        Sink orderedControllerSink;
        const auto orderedControllerConnection = connect(orderedControllerCore, orderedControllerSink, false);
        if (controllerMethod != conditional.end() && orderedControllerConnection) {
            const auto defined = command("conditional-ordered-controller", **controllerMethod, parameters);
            if (defined) {
                static_cast<void>(orderedControllerCore.receiveDefinedCommand(*orderedControllerConnection, *defined));
            }
        }
        const frontend::CommandError* controllerError = latestCommandError(orderedControllerSink);
        const bool exactOrderedErrors =
            disabledError && disabledError->code == frontend::ErrorCode::UnknownMethod &&
            disabledError->message == "frontend command method is unavailable" && !disabledError->closeConnection && policyError &&
            policyError->code == frontend::ErrorCode::PermissionDenied &&
            policyError->message == "frontend deployment policy denied the command" && scopeError &&
            scopeError->code == frontend::ErrorCode::PermissionDenied &&
            scopeError->message == "frontend principal lacks a required scope" && controllerError &&
            controllerError->code == frontend::ErrorCode::PermissionDenied &&
            controllerError->message == "the current controller is required" && providerError &&
            providerError->code == frontend::ErrorCode::BackendUnavailable &&
            providerError->message == "the Codex App Server is not ready";

        result.expectTrue(
            parameters.size() == generated::AllMethods.size() && conditional.size() == 15 && accepted == conditional.size() &&
                disabledRejected == conditional.size() && policyRejected == conditional.size() && scopeRejected == conditional.size() &&
                providerRejected == conditional.size() && controllerCorrect == conditional.size() && exactOrderedErrors &&
                disabledPrecedesSchema && commandEnvelopePrecedesDeployment && protocolEnvelopePrecedesDeployment &&
                completePolicyParameters,
            "all 15 non-default generated methods apply the exact oracle deployment, policy, scope, controller, and provider order");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testLocalTrust(result);
    testAuthenticationBudgetAndSanitization(result);
    testRateScopesAndController(result);
    testConditionalMethods(result);
    return result.processResult();
}
