/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "CodexBackendTestSupport.h"
#include "ai/openai/codex/backend/BackendCore.h"
#include "ai/openai/codex/detail/ApprovalCodec.h"
#include "ai/openai/codex/detail/McpReverseRequestCodec.h"
#include "ai/openai/codex/detail/ThreadCodec.h"
#include "ai/openai/codex/detail/TurnCodec.h"
#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/detail/BackendCommandMapper.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = frontend::generated;
    namespace mapping = frontend::detail;
    namespace typed = ai::openai::codex::typed;
    using Json = ai::openai::codex::Json;
    using FakeBackendCore = backend::BackendCore<tests::codex::FakeAppServerClient>;

    const Json* fixtureFor(const Json& fixtures, std::string_view method) {
        const auto& methods = fixtures.at("methods");
        const auto found = std::find_if(methods.begin(), methods.end(), [method](const Json& fixture) {
            return fixture.at("method").get_ref<const std::string&>() == method;
        });
        return found == methods.end() ? nullptr : &*found;
    }

    frontend::CodecResult<generated::DefinedCommand> decode(std::string_view method, const Json& params) {
        return frontend::Codec::decodeDefinedCommand(Json{{"protocol", frontend::ProtocolIdentity},
                                                          {"version", frontend::ProtocolVersion},
                                                          {"kind", "command"},
                                                          {"requestId", "mapping-test"},
                                                          {"method", method},
                                                          {"params", params}});
    }

    const Json& parameterValue(const generated::DefinedCommand& command) {
        return std::visit(
            [](const auto& parameters) -> const Json& {
                return parameters.value;
            },
            command.parameters);
    }

    bool usesUnitFacade(std::string_view method) {
        static constexpr std::string_view Methods[] = {
            "account/logout",
            "account/rateLimits/read",
            "account/usage/read",
            "account/workspaceMessages/read",
            "config/mcpServer/reload",
            "configRequirements/read",
            "externalAgentConfig/import/readHistories",
            "windowsSandbox/readiness",
        };
        return std::find(std::begin(Methods), std::end(Methods), method) != std::end(Methods);
    }

    std::string_view providerMethod(const generated::MethodMetadata& metadata) {
        if (metadata.category != generated::MethodCategory::ProviderOperation || metadata.registryKeys.size() != 1) {
            return {};
        }
        constexpr std::string_view Marker = ":method:";
        const std::string_view key = metadata.registryKeys.front();
        const std::size_t marker = key.rfind(Marker);
        return marker == std::string_view::npos ? std::string_view{} : key.substr(marker + Marker.size());
    }

    Json expectedProviderParams(const generated::MethodMetadata& metadata, const generated::DefinedCommand& command) {
        if (usesUnitFacade(providerMethod(metadata))) {
            return nullptr;
        }

        Json expected = parameterValue(command);
        if ((metadata.id == generated::MethodId::ThreadStart || metadata.id == generated::MethodId::ThreadResume) &&
            !expected.contains("sandbox") && command.parameterExtensions.contains("sandboxMode")) {
            expected["sandbox"] = command.parameterExtensions.at("sandboxMode");
        }
        if (metadata.id == generated::MethodId::TurnStart && !expected.contains("effort") &&
            command.parameterExtensions.contains("reasoningEffort")) {
            expected["effort"] = command.parameterExtensions.at("reasoningEffort");
        }
        if (metadata.id == generated::MethodId::TurnStart && expected.contains("input") && expected["input"].is_array()) {
            for (Json& input : expected["input"]) {
                if (input.is_object() && input.value("type", std::string{}) == "text" && !input.contains("text_elements")) {
                    // The original v1 adapter default-constructed provider TextInput,
                    // whose canonical wire form includes an empty text_elements array.
                    input["text_elements"] = Json::array();
                }
            }
        }
        if (expected.contains("sandboxPolicy") && expected["sandboxPolicy"].is_object() &&
            expected["sandboxPolicy"].value("type", std::string{}) == "dangerFullAccess") {
            // The stable provider union's dangerFullAccess alternative is deliberately fieldless.
            expected["sandboxPolicy"].erase("networkAccess");
        }
        return expected;
    }

    Json providerValidFixtureParams(const Json& params) {
        return params;
    }

    bool expectedNativeAction(generated::MethodId id, mapping::NativeServiceAction action) {
        switch (id) {
            case generated::MethodId::ControllerAcquire:
                return action == mapping::NativeServiceAction::ControllerAcquire;
            case generated::MethodId::ControllerRelease:
                return action == mapping::NativeServiceAction::ControllerRelease;
            case generated::MethodId::SnapshotGet:
                return action == mapping::NativeServiceAction::SnapshotGet;
            case generated::MethodId::EventsReplay:
                return action == mapping::NativeServiceAction::EventsReplay;
            case generated::MethodId::ProviderStart:
                return action == mapping::NativeServiceAction::ProviderStart;
            case generated::MethodId::ProviderStop:
                return action == mapping::NativeServiceAction::ProviderStop;
            case generated::MethodId::ProviderRestart:
                return action == mapping::NativeServiceAction::ProviderRestart;
            default:
                return false;
        }
    }

    void testCompleteTable(tests::support::TestResult& result, const Json& fixtures) {
        std::size_t nativeCount = 0;
        std::size_t backendCount = 0;
        std::size_t providerCount = 0;
        std::size_t reverseCount = 0;
        bool exact = true;

        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            const Json* fixture = fixtureFor(fixtures, metadata.method);
            exact = exact && fixture != nullptr;
            if (fixture == nullptr) {
                continue;
            }
            for (const std::string_view field : {"minimalParams", "completeParams"}) {
                const auto decoded = decode(metadata.method, fixture->at(field));
                exact = exact && decoded.hasValue();
                if (!decoded) {
                    continue;
                }
                const mapping::DefinedCommandMapping mapped = mapping::mapDefinedCommand(decoded.value());
                if (metadata.frontendNative) {
                    const auto* action = std::get_if<mapping::NativeCommandMapping>(&mapped);
                    exact = exact && action != nullptr && expectedNativeAction(metadata.id, action->action);
                    if (metadata.id == generated::MethodId::EventsReplay) {
                        exact = exact && action != nullptr && action->replayAfter == std::optional<std::uint64_t>{0};
                    } else {
                        exact = exact && action != nullptr && !action->replayAfter.has_value();
                    }
                } else {
                    const auto* command = std::get_if<backend::BackendCommand>(&mapped);
                    exact = exact && command != nullptr && command != nullptr &&
                            mapping::backendCommandTypeName(*command) == metadata.backendCommand;
                }
            }

            if (metadata.frontendNative) {
                ++nativeCount;
            } else {
                ++backendCount;
                if (metadata.category == generated::MethodCategory::ProviderOperation) {
                    ++providerCount;
                }
                if (metadata.category == generated::MethodCategory::ReverseResponse) {
                    ++reverseCount;
                }
            }
        }

        result.expectTrue(exact && nativeCount == 7 && backendCount == 98 && providerCount == 86 && reverseCount == 12,
                          "minimal and complete generated fixtures map to seven exact native actions, 86 provider commands, and 12 "
                          "reverse commands");
    }

    mapping::DefinedCommandMapping mapFixture(const Json& fixtures, std::string_view method, std::string_view field = "completeParams") {
        const Json* fixture = fixtureFor(fixtures, method);
        if (fixture == nullptr) {
            return mapping::BackendCommandMappingError{"fixture missing"};
        }
        auto decoded = decode(method, fixture->at(field));
        if (!decoded) {
            return mapping::BackendCommandMappingError{"fixture did not decode"};
        }
        return mapping::mapDefinedCommand(decoded.value());
    }

    template <typename ReverseCommand>
    Json withRequestId(const ReverseCommand& command, Json params) {
        params["pendingRequestId"] = std::to_string(command.requestId.value());
        return params;
    }

    std::optional<Json> encodeReverseCommand(const backend::BackendCommand& command) {
        return std::visit(
            []<typename Command>(const Command& value) -> std::optional<Json> {
                using T = std::remove_cvref_t<Command>;
                std::string error;

                if constexpr (std::is_same_v<T, backend::ApprovalRespond>) {
                    const auto* decision = std::get_if<typed::ApprovalDecision>(&value.response);
                    return decision ? std::optional<Json>{withRequestId(value, Json{{"decision", decision->value}})} : std::nullopt;
                } else if constexpr (std::is_same_v<T, backend::UserInputRespond>) {
                    const auto* answers = std::get_if<std::vector<typed::UserInputAnswer>>(&value.response);
                    if (answers == nullptr) {
                        return std::nullopt;
                    }
                    Json encodedAnswers = Json::array();
                    for (const typed::UserInputAnswer& answer : *answers) {
                        encodedAnswers.push_back({{"questionId", answer.questionId}, {"answers", answer.answers}});
                    }
                    return std::optional<Json>{withRequestId(value, Json{{"answers", std::move(encodedAnswers)}})};
                } else if constexpr (std::is_same_v<T, backend::AuthenticationRespond>) {
                    const auto* response = std::get_if<typed::AuthenticationResponse>(&value.response);
                    if (response == nullptr) {
                        return std::nullopt;
                    }
                    Json params{{"accessToken", response->accessToken}, {"chatgptAccountId", response->chatgptAccountId}};
                    if (response->chatgptPlanType.has_value()) {
                        params["chatgptPlanType"] = *response->chatgptPlanType;
                    }
                    return std::optional<Json>{withRequestId(value, std::move(params))};
                } else if constexpr (std::is_same_v<T, backend::UnknownRequestRespondRaw>) {
                    return std::optional<Json>{withRequestId(value, Json{{"result", value.result}})};
                } else if constexpr (std::is_same_v<T, backend::UnknownRequestReject>) {
                    Json params{{"code", value.error.code}, {"message", value.error.message}};
                    if (value.error.data.has_value()) {
                        params["data"] = *value.error.data;
                    }
                    return std::optional<Json>{withRequestId(value, std::move(params))};
                } else if constexpr (std::is_same_v<T, backend::ApplyPatchApprovalRespond>) {
                    const auto encoded = ai::openai::codex::detail::encodeApplyPatchApprovalResponse(value.response, error);
                    return encoded ? std::optional<Json>{withRequestId(value, Json{{"response", *encoded}})} : std::nullopt;
                } else if constexpr (std::is_same_v<T, backend::ExecCommandApprovalRespond>) {
                    const auto encoded = ai::openai::codex::detail::encodeExecCommandApprovalResponse(value.response, error);
                    return encoded ? std::optional<Json>{withRequestId(value, Json{{"response", *encoded}})} : std::nullopt;
                } else if constexpr (std::is_same_v<T, backend::PermissionsApprovalRespond>) {
                    const auto encoded = ai::openai::codex::detail::encodePermissionsRequestApprovalResponse(value.response, error);
                    return encoded ? std::optional<Json>{withRequestId(value, Json{{"response", *encoded}})} : std::nullopt;
                } else if constexpr (std::is_same_v<T, backend::AttestationGenerateRespond>) {
                    const auto encoded = ai::openai::codex::detail::encodeAttestationGenerateResponse(value.response, error);
                    return encoded ? std::optional<Json>{withRequestId(value, Json{{"response", *encoded}})} : std::nullopt;
                } else if constexpr (std::is_same_v<T, backend::DynamicToolCallRespond>) {
                    const auto encoded = ai::openai::codex::detail::encodeDynamicToolCallResponse(value.response, error);
                    return encoded ? std::optional<Json>{withRequestId(value, Json{{"response", *encoded}})} : std::nullopt;
                } else if constexpr (std::is_same_v<T, backend::McpServerElicitationRespond>) {
                    const auto encoded = ai::openai::codex::detail::encodeMcpServerElicitationRequestResponse(value.response, error);
                    return encoded ? std::optional<Json>{withRequestId(value, Json{{"response", *encoded}})} : std::nullopt;
                } else if constexpr (std::is_same_v<T, backend::KnownRequestReject>) {
                    Json encodedError{{"code", value.error.code}, {"message", value.error.message}};
                    if (value.error.data.has_value()) {
                        encodedError["data"] = *value.error.data;
                    }
                    return std::optional<Json>{withRequestId(value, Json{{"error", std::move(encodedError)}})};
                } else {
                    return std::nullopt;
                }
            },
            command);
    }

    void testReverseValueParity(tests::support::TestResult& result, const Json& fixtures) {
        std::size_t fixtureCount = 0;
        bool exact = true;
        std::string firstMismatch;
        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            if (metadata.category != generated::MethodCategory::ReverseResponse) {
                continue;
            }
            const Json* fixture = fixtureFor(fixtures, metadata.method);
            if (fixture == nullptr) {
                exact = false;
                firstMismatch = std::string(metadata.method) + " fixture missing";
                break;
            }
            for (const std::string_view field : {"minimalParams", "completeParams"}) {
                ++fixtureCount;
                const auto decoded = decode(metadata.method, fixture->at(field));
                if (!decoded) {
                    exact = false;
                    firstMismatch = std::string(metadata.method) + " " + std::string(field) + " did not decode";
                    break;
                }
                const mapping::DefinedCommandMapping mapped = mapping::mapDefinedCommand(decoded.value());
                const auto* command = std::get_if<backend::BackendCommand>(&mapped);
                const std::optional<Json> encoded = command ? encodeReverseCommand(*command) : std::nullopt;
                if (!encoded.has_value() || *encoded != fixture->at(field)) {
                    exact = false;
                    firstMismatch = std::string(metadata.method) + " " + std::string(field) + " lost or changed a typed value";
                    break;
                }
            }
            if (!exact) {
                break;
            }
        }
        result.expectTrue(exact && fixtureCount == 24,
                          "all 24 minimal/complete reverse fixtures retain every typed request ID, response, rejection, nullable, and "
                          "nested value" +
                              (firstMismatch.empty() ? std::string{} : ": " + firstMismatch));
    }

    class ProviderWireParityRunner {
    public:
        ProviderWireParityRunner(tests::support::TestResult& result, const Json& fixtures)
            : result(result)
            , fixtures(fixtures) {
        }

        void start() {
            transport = std::make_shared<tests::codex::FakeTransportState>();
            tests::codex::installInitializingFake(
                transport, [this](const Json& message, const ai::openai::codex::detail::TransportCallbacks& callbacks) {
                    handleProviderRequest(message, callbacks);
                });

            backend::BackendCoreOptions options;
            options.initialThreadListLimit = 1;
            core = std::make_unique<FakeBackendCore>(std::move(options), transport);

            backend::FrontendSessionCallbacks callbacks;
            callbacks.onCommandCompleted = [this](const backend::CommandCompletion& completion) {
                completedRequestIds.insert(completion.requestId);
                if (completion.result.error.has_value()) {
                    completionErrors.emplace(completion.requestId, completion.result.error->message);
                }
            };
            session.emplace(core->openSession(std::move(callbacks)));
            exact = static_cast<bool>(session->submit("wire-controller", backend::ControllerAcquire{}));
            if (!exact) {
                firstMismatch = "controller acquisition was not accepted";
                reportAndShutdown();
                return;
            }
            waitUntil(
                "wire-parity controller acquisition completes",
                [this]() {
                    return completedRequestIds.contains("wire-controller") && session->role() == backend::SessionRole::Controller;
                },
                [this]() {
                    core->start();
                    waitUntil(
                        "wire-parity BackendCore reaches Ready after bounded hydration",
                        [this]() {
                            const backend::Snapshot snapshot = core->snapshot();
                            return core->isReady() && snapshot.threadList.pagesLoaded == 1;
                        },
                        [this]() {
                            auditing = true;
                            submitNextFixture();
                        });
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

        void
        waitUntil(std::string description, std::function<bool()> predicate, std::function<void()> next, std::size_t remaining = 20'000) {
            waitingDescription = description;
            defer([this,
                   description = std::move(description),
                   predicate = std::move(predicate),
                   next = std::move(next),
                   remaining]() mutable {
                if (finished || finishing) {
                    return;
                }
                try {
                    if (predicate()) {
                        waitingDescription = "advancing after: " + description;
                        next();
                        return;
                    }
                    if (remaining == 0) {
                        exact = false;
                        if (firstMismatch.empty()) {
                            firstMismatch = description;
                        }
                        reportAndShutdown();
                        return;
                    }
                    waitUntil(std::move(description), std::move(predicate), std::move(next), remaining - 1);
                } catch (...) {
                    exact = false;
                    if (firstMismatch.empty()) {
                        firstMismatch = "an exception escaped a wire-parity event-loop stage";
                    }
                    reportAndShutdown();
                }
            });
        }

        void submitNextFixture() {
            while (methodIndex < generated::AllMethods.size() &&
                   generated::AllMethods[methodIndex].category != generated::MethodCategory::ProviderOperation) {
                ++methodIndex;
            }
            if (methodIndex == generated::AllMethods.size()) {
                reportAndShutdown();
                return;
            }

            const generated::MethodMetadata& metadata = generated::AllMethods[methodIndex];
            const std::string_view field = fieldIndex == 0 ? "minimalParams" : "completeParams";
            ++fixtureCount;
            const Json* fixture = fixtureFor(fixtures, metadata.method);
            if (fixture == nullptr) {
                fail(std::string(metadata.method) + " fixture missing");
                return;
            }
            const auto decoded = decode(metadata.method, providerValidFixtureParams(fixture->at(field)));
            if (!decoded) {
                fail(std::string(metadata.method) + " " + std::string(field) + " did not decode");
                return;
            }

            activeExpectedParams = expectedProviderParams(metadata, decoded.value());
            activeExpectedMethod = providerMethod(metadata);
            activeField = field;
            mapping::DefinedCommandMapping mapped = mapping::mapDefinedCommand(decoded.value());
            auto* command = std::get_if<backend::BackendCommand>(&mapped);
            activeOutgoingBefore = transport->outgoing.size();
            activeRequestId = "wire-" + std::to_string(fixtureCount);
            const bool accepted = command != nullptr && static_cast<bool>(session->submit(activeRequestId, std::move(*command)));
            if (!accepted) {
                fail(std::string(metadata.method) + " " + std::string(field) + " was not accepted by BackendCore");
                return;
            }

            waitUntil(
                "provider fixture completion " + std::to_string(fixtureCount),
                [this]() {
                    return completedRequestIds.contains(activeRequestId);
                },
                [this]() {
                    verifyActiveFixture();
                });
        }

        void verifyActiveFixture() {
            const generated::MethodMetadata& metadata = generated::AllMethods[methodIndex];
            const bool oneRequest = transport->outgoing.size() == activeOutgoingBefore + 1;
            const Json* outgoing = oneRequest ? &transport->outgoing.back() : nullptr;
            if (outgoing == nullptr || outgoing->value("method", std::string{}) != activeExpectedMethod || !outgoing->contains("params") ||
                outgoing->at("params") != activeExpectedParams) {
                fail(std::string(metadata.method) + " " + std::string(activeField) +
                     " did not preserve its canonical provider wire method/params; expected=" + activeExpectedParams.dump() +
                     ", actual=" + (outgoing != nullptr ? outgoing->dump() : "<missing>") +
                     ", outgoing delta=" + std::to_string(transport->outgoing.size() - activeOutgoingBefore) + ", completion error=" +
                     (completionErrors.contains(activeRequestId) ? completionErrors.at(activeRequestId) : std::string{"<none>"}));
                return;
            }

            if (fieldIndex == 0) {
                fieldIndex = 1;
            } else {
                fieldIndex = 0;
                ++methodIndex;
            }
            submitNextFixture();
        }

        void handleProviderRequest(const Json& message, const ai::openai::codex::detail::TransportCallbacks& callbacks) const {
            const auto method = message.find("method");
            const auto id = message.find("id");
            if (method == message.end() || !method->is_string() || id == message.end()) {
                return;
            }
            if (!auditing && *method == "thread/list") {
                tests::codex::inject(
                    callbacks,
                    Json{{"id", *id}, {"result", {{"data", Json::array()}, {"nextCursor", nullptr}, {"backwardsCursor", nullptr}}}});
            } else if (auditing) {
                tests::codex::inject(callbacks,
                                     Json{{"id", *id}, {"error", {{"code", -32000}, {"message", "mapping audit terminal response"}}}});
            }
        }

        void fail(std::string mismatch) {
            exact = false;
            if (firstMismatch.empty()) {
                firstMismatch = std::move(mismatch);
            }
            reportAndShutdown();
        }

        void reportAndShutdown() {
            if (reported) {
                return;
            }
            reported = true;
            result.expectTrue(
                exact && fixtureCount == 172,
                "all 172 minimal/complete provider fixture shapes use stable union discriminators and survive frontend "
                "decoding, typed BackendCommand mapping, and the direct AppServerClient facade as exact canonical wire params" +
                    (firstMismatch.empty() ? std::string{} : ": " + firstMismatch));
            beginShutdown();
        }

        void beginShutdown() {
            if (finishing || finished) {
                return;
            }
            finishing = true;
            waitingDescription = "waiting for wire-parity BackendCore shutdown";
            if (session.has_value()) {
                session->close("wire parity complete");
            }
            if (core) {
                core->stop();
            }
            waitForShutdown(20'000);
        }

        void waitForShutdown(std::size_t remaining) {
            defer([this, remaining]() {
                try {
                    if (!core || core->snapshot().provider.lifecycle == backend::ProviderLifecycle::Stopped || remaining == 0) {
                        result.expectTrue(remaining != 0, "wire-parity BackendCore stops within its deterministic cleanup bound");
                        session.reset();
                        core.reset();
                        finished = true;
                        core::SNodeC::stop();
                        return;
                    }
                    waitForShutdown(remaining - 1);
                } catch (...) {
                    session.reset();
                    core.reset();
                    finished = true;
                    core::SNodeC::stop();
                }
            });
        }

        tests::support::TestResult& result;
        const Json& fixtures;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        std::unique_ptr<FakeBackendCore> core;
        std::optional<backend::FrontendSession> session;
        std::set<std::string> completedRequestIds;
        std::map<std::string, std::string> completionErrors;
        Json activeExpectedParams;
        std::string activeExpectedMethod;
        std::string activeRequestId;
        std::string_view activeField;
        std::string firstMismatch;
        std::size_t methodIndex = 0;
        std::size_t fieldIndex = 0;
        std::size_t fixtureCount = 0;
        std::size_t activeOutgoingBefore = 0;
        bool auditing = false;
        bool exact = true;
        bool reported = false;
        bool finishing = false;
        bool finished = false;
        std::string waitingDescription = "not started";
    };

    void testExactValues(tests::support::TestResult& result, const Json& fixtures) {
        const auto threadStartMapping = mapFixture(fixtures, "thread.start");
        const auto* threadStartCommand = std::get_if<backend::BackendCommand>(&threadStartMapping);
        const auto* threadStart = threadStartCommand ? std::get_if<backend::ThreadStart>(threadStartCommand) : nullptr;
        const auto* futureApproval = threadStart != nullptr && threadStart->params.approvalPolicy.hasValue()
                                         ? std::get_if<typed::ApprovalPolicy>(&*threadStart->params.approvalPolicy)
                                         : nullptr;
        result.expectTrue(threadStart != nullptr && threadStart->params.cwd.hasValue() && *threadStart->params.cwd == "x" &&
                              threadStart->params.sandbox.hasValue() && threadStart->params.sandbox->value == "x" &&
                              threadStart->params.ephemeral.hasValue() && !*threadStart->params.ephemeral && futureApproval != nullptr &&
                              futureApproval->value == "x",
                          "legacy thread.start aliases and future scalar approval policy retain exact typed provider values");

        const auto turnStartMapping = mapFixture(fixtures, "turn.start");
        const auto* turnStartCommand = std::get_if<backend::BackendCommand>(&turnStartMapping);
        const auto* turnStart = turnStartCommand ? std::get_if<backend::TurnStart>(turnStartCommand) : nullptr;
        result.expectTrue(turnStart != nullptr && turnStart->params.effort.hasValue() && turnStart->params.effort->value == "x" &&
                              turnStart->params.input.size() == 1,
                          "legacy reasoningEffort maps to the exact typed TurnStart effort and retains typed input");

        const auto loginMapping = mapFixture(fixtures, "account.login.start");
        const auto* loginCommand = std::get_if<backend::BackendCommand>(&loginMapping);
        const auto* login = loginCommand ? std::get_if<backend::AccountLoginStart>(loginCommand) : nullptr;
        const auto* apiKey = login ? std::get_if<ai::openai::codex::typed::ApiKeyLoginAccountParams>(&login->params) : nullptr;
        result.expectTrue(apiKey != nullptr && apiKey->apiKey == "x", "tagged account login params retain the exact selected typed union");

        const auto fsMapping = mapFixture(fixtures, "fs.copy");
        const auto* fsCommand = std::get_if<backend::BackendCommand>(&fsMapping);
        const auto* fsCopy = fsCommand ? std::get_if<backend::FsCopy>(fsCommand) : nullptr;
        result.expectTrue(fsCopy != nullptr && fsCopy->params.destinationPath.value == "x" && fsCopy->params.sourcePath.value == "x" &&
                              fsCopy->params.recursive == std::optional<bool>{false},
                          "filesystem mapping retains exact typed paths and optional recursion state");

        const auto approvalMapping = mapFixture(fixtures, "request.applyPatchApproval.respond");
        const auto* approvalCommand = std::get_if<backend::BackendCommand>(&approvalMapping);
        const auto* approval = approvalCommand ? std::get_if<backend::ApplyPatchApprovalRespond>(approvalCommand) : nullptr;
        std::string encodeError;
        const auto encodedDecision =
            approval ? ai::openai::codex::detail::encodeReviewDecision(approval->response.decision, encodeError) : std::nullopt;
        result.expectTrue(approval != nullptr && approval->requestId.value() == 1 && encodedDecision == std::optional<Json>{"approved"},
                          "dedicated reverse mapping retains the pending occurrence and exact typed response union");
    }

    void testTypedLegacyMethodBoundary(tests::support::TestResult& result) {
        typed::ThreadStartParams startParams;
        startParams.approvalsReviewer = typed::OptionalNullable<typed::ApprovalsReviewer>::withValue(typed::ApprovalsReviewer::user());
        startParams.baseInstructions = typed::OptionalNullable<std::string>::withValue("base instructions");
        startParams.sandbox = typed::OptionalNullable<typed::SandboxMode>::withValue(typed::SandboxMode::readOnly());
        startParams.sessionStartSource = typed::OptionalNullable<typed::ThreadStartSource>::withValue(typed::ThreadStartSource::startup());

        std::string error;
        const std::optional<Json> encodedStart = ai::openai::codex::detail::encodeThreadStartParams(startParams, error);
        const auto decodedStart = decode("thread.start", encodedStart.value_or(Json::array()));
        const auto mappedStart = decodedStart ? mapping::mapDefinedCommand(decodedStart.value())
                                              : mapping::DefinedCommandMapping{mapping::BackendCommandMappingError{error}};
        const auto* startCommand = std::get_if<backend::BackendCommand>(&mappedStart);
        const auto* start = startCommand ? std::get_if<backend::ThreadStart>(startCommand) : nullptr;
        const bool modernStartFieldsAreDefined =
            decodedStart && parameterValue(decodedStart.value()).contains("approvalsReviewer") &&
            parameterValue(decodedStart.value()).contains("baseInstructions") && parameterValue(decodedStart.value()).contains("sandbox") &&
            parameterValue(decodedStart.value()).contains("sessionStartSource") && decodedStart.value().parameterExtensions.empty();
        result.expectTrue(modernStartFieldsAreDefined && start != nullptr && start->params.approvalsReviewer.hasValue() &&
                              start->params.approvalsReviewer->value == "user" && start->params.baseInstructions.hasValue() &&
                              *start->params.baseInstructions == "base instructions" && start->params.sandbox.hasValue() &&
                              start->params.sandbox->value == "read-only" && start->params.sessionStartSource.hasValue() &&
                              start->params.sessionStartSource->value == "startup",
                          "typed thread.start modern fields remain defined parameters and reach the exact backend command");

        typed::ThreadListParams listParams;
        listParams.sourceKinds = typed::OptionalNullable<std::vector<typed::ThreadSourceKind>>::withValue(
            {typed::ThreadSourceKind::cli(), typed::ThreadSourceKind::vscode()});
        listParams.cwd = typed::OptionalNullable<typed::ThreadListCwdFilter>::withValue(
            typed::ThreadListCwdFilter{std::vector<std::string>{"/one", "/two"}});
        listParams.sortDirection = typed::OptionalNullable<typed::SortDirection>::withValue(typed::SortDirection::ascending());
        listParams.useStateDbOnly = true;
        const std::optional<Json> encodedList = ai::openai::codex::detail::encodeThreadListParams(listParams, error);
        const auto decodedList = decode("thread.list", encodedList.value_or(Json::array()));
        const auto mappedList = decodedList ? mapping::mapDefinedCommand(decodedList.value())
                                            : mapping::DefinedCommandMapping{mapping::BackendCommandMappingError{error}};
        const auto* listCommand = std::get_if<backend::BackendCommand>(&mappedList);
        const auto* list = listCommand ? std::get_if<backend::ThreadList>(listCommand) : nullptr;
        const bool modernListFieldsAreDefined =
            decodedList && parameterValue(decodedList.value()).contains("sourceKinds") &&
            parameterValue(decodedList.value()).contains("cwd") && parameterValue(decodedList.value()).contains("sortDirection") &&
            parameterValue(decodedList.value()).contains("useStateDbOnly") && decodedList.value().parameterExtensions.empty();
        result.expectTrue(modernListFieldsAreDefined && list != nullptr && list->params.sourceKinds.hasValue() &&
                              list->params.sourceKinds->size() == 2 && list->params.cwd.hasValue() &&
                              std::holds_alternative<std::vector<std::string>>(list->params.cwd->value) &&
                              list->params.sortDirection.hasValue() && list->params.sortDirection->value == "asc" &&
                              list->params.useStateDbOnly == std::optional<bool>{true},
                          "typed thread.list modern filters remain defined parameters and reach the exact backend command");

        typed::TurnStartParams turnParams;
        turnParams.threadId = typed::ThreadId{"thread-modern"};
        typed::TextInput input;
        input.text = "hello";
        turnParams.input.emplace_back(std::move(input));
        turnParams.effort = typed::OptionalNullable<typed::ReasoningEffort>::withValue(typed::ReasoningEffort::high());
        turnParams.personality = typed::OptionalNullable<typed::Personality>::withValue(typed::Personality::friendly());
        const std::optional<Json> encodedTurn = ai::openai::codex::detail::encodeTurnStartParams(turnParams, error);
        const auto decodedTurn = decode("turn.start", encodedTurn.value_or(Json::array()));
        const auto mappedTurn = decodedTurn ? mapping::mapDefinedCommand(decodedTurn.value())
                                            : mapping::DefinedCommandMapping{mapping::BackendCommandMappingError{error}};
        const auto* turnCommand = std::get_if<backend::BackendCommand>(&mappedTurn);
        const auto* turn = turnCommand ? std::get_if<backend::TurnStart>(turnCommand) : nullptr;
        const bool modernTurnFieldsAreDefined = decodedTurn && parameterValue(decodedTurn.value()).contains("effort") &&
                                                parameterValue(decodedTurn.value()).contains("personality") &&
                                                decodedTurn.value().parameterExtensions.empty();
        result.expectTrue(modernTurnFieldsAreDefined && turn != nullptr && turn->params.effort.hasValue() &&
                              turn->params.effort->value == "high" && turn->params.personality.hasValue() &&
                              turn->params.personality->value == "friendly",
                          "typed turn.start effort and other modern fields remain defined parameters and reach the exact backend command");

        const auto legacyStart = decode("thread.start", Json{{"sandboxMode", "workspace-write"}});
        const auto legacyTurn = decode("turn.start",
                                       Json{{"threadId", "thread-legacy"},
                                            {"input", Json::array({Json{{"type", "text"}, {"text", "hello"}}})},
                                            {"reasoningEffort", "medium"}});
        const auto mappedLegacyStart = legacyStart ? mapping::mapDefinedCommand(legacyStart.value())
                                                   : mapping::DefinedCommandMapping{mapping::BackendCommandMappingError{""}};
        const auto mappedLegacyTurn = legacyTurn ? mapping::mapDefinedCommand(legacyTurn.value())
                                                 : mapping::DefinedCommandMapping{mapping::BackendCommandMappingError{""}};
        const auto* legacyStartCommand = std::get_if<backend::BackendCommand>(&mappedLegacyStart);
        const auto* legacyStartValue = legacyStartCommand ? std::get_if<backend::ThreadStart>(legacyStartCommand) : nullptr;
        const auto* legacyTurnCommand = std::get_if<backend::BackendCommand>(&mappedLegacyTurn);
        const auto* legacyTurnValue = legacyTurnCommand ? std::get_if<backend::TurnStart>(legacyTurnCommand) : nullptr;
        result.expectTrue(legacyStart && legacyTurn && legacyStart.value().parameterExtensions.contains("sandboxMode") &&
                              !parameterValue(legacyStart.value()).contains("sandboxMode") &&
                              legacyTurn.value().parameterExtensions.contains("reasoningEffort") &&
                              !parameterValue(legacyTurn.value()).contains("reasoningEffort") && legacyStartValue != nullptr &&
                              legacyStartValue->params.sandbox.hasValue() && legacyStartValue->params.sandbox->value == "workspace-write" &&
                              legacyTurnValue != nullptr && legacyTurnValue->params.effort.hasValue() &&
                              legacyTurnValue->params.effort->value == "medium",
                          "sandboxMode and reasoningEffort remain compatibility-only aliases merged into canonical typed fields");

        typed::ThreadStartParams granularParams;
        granularParams.approvalPolicy = typed::OptionalNullable<typed::AskForApproval>::withValue(
            typed::GranularAskForApproval{{true, true, false, true, false}, Json::object(), {}});
        const std::optional<Json> encodedGranular = ai::openai::codex::detail::encodeThreadStartParams(granularParams, error);
        const auto rejectedGranular = decode("thread.start", encodedGranular.value_or(Json::array()));
        result.expectTrue(encodedGranular && encodedGranular->at("approvalPolicy").is_object() && !rejectedGranular &&
                              rejectedGranular.error().code == frontend::ErrorCode::InvalidField,
                          "the legacy frontend approvalPolicy schema rejects a granular typed value locally instead of dropping it");
    }

    void testAliasConflicts(tests::support::TestResult& result, const Json& fixtures) {
        Json threadStart = fixtureFor(fixtures, "thread.start")->at("completeParams");
        threadStart["sandbox"] = "canonical-sandbox";
        Json threadResume = fixtureFor(fixtures, "thread.resume")->at("completeParams");
        threadResume["sandbox"] = "canonical-sandbox";
        Json turnStart = fixtureFor(fixtures, "turn.start")->at("completeParams");
        turnStart["effort"] = "canonical-effort";

        const auto startConflict = decode("thread.start", threadStart);
        const auto resumeConflict = decode("thread.resume", threadResume);
        const auto effortConflict = decode("turn.start", turnStart);
        result.expectTrue(!startConflict && !resumeConflict && !effortConflict,
                          "canonical sandbox/effort fields cannot conflict with legacy sandboxMode/reasoningEffort aliases");
    }

    void testSafeFailure(tests::support::TestResult& result) {
        constexpr std::string_view SecretSentinel = "A17B_MAPPING_SECRET_SENTINEL";
        generated::DefinedCommand command;
        command.requestId = "invalid";
        command.parameters = generated::makeParameters(generated::MethodId::FsReadFile, Json::object());
        command.parameterExtensions = Json{{"note", SecretSentinel}};
        const auto mapped = mapping::mapDefinedCommand(command);
        const auto* error = std::get_if<mapping::BackendCommandMappingError>(&mapped);
        result.expectTrue(error != nullptr && error->message.find(SecretSentinel) == std::string::npos,
                          "mapping failures are bounded and never echo parameter or extension values");
    }
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    int returnCode = tests::support::cTestSkipReturnCode;
    const std::filesystem::path fixturePath =
        std::filesystem::path(__FILE__).parent_path() / "fixtures" / "frontend-protocol-v1.generated.json";
    std::ifstream input(fixturePath);
    const Json fixtures = Json::parse(input, nullptr, false);
    result.expectTrue(!input.fail() && !fixtures.is_discarded(), "the generated frontend fixture corpus is readable");
    if (!input || fixtures.is_discarded()) {
        return result.processResult();
    }

    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexFrontendRuntimeBackendMappingTest");
    } else {
        core::SNodeC::init(argc, argv);
        testCompleteTable(result, fixtures);
        testReverseValueParity(result, fixtures);
        testExactValues(result, fixtures);
        testTypedLegacyMethodBoundary(result);
        testAliasConflicts(result, fixtures);
        testSafeFailure(result);

        bool timedOut = false;
        ProviderWireParityRunner runner(result, fixtures);
        [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
            [&timedOut]() {
                timedOut = true;
                core::SNodeC::stop();
            },
            utils::Timeval({10, 0}));
        runner.start();
        const int eventLoopResult = core::SNodeC::start(utils::Timeval({12, 0}));
        result.expectTrue(!timedOut,
                          "provider wire-parity audit completes before the watchdog (last stage: " + runner.waitingStage() + ")");
        result.expectTrue(runner.isFinished(), "provider wire-parity audit reaches a clean terminal state");
        result.expectEqual(0, eventLoopResult, "provider wire-parity event loop exits cleanly");

        core::SNodeC::free();
        returnCode = result.processResult();
    }
    return returnCode;
}
