/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/Api.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "component/codex/CodexBackendTestSupport.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <cerrno>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace codex = ai::openai::codex;
    namespace typed = ai::openai::codex::typed;

    class WireRunner {
    public:
        explicit WireRunner(tests::support::TestResult& result)
            : result(result)
            , transport(std::make_shared<tests::codex::FakeTransportState>()) {
        }

        void start() {
            tests::codex::installInitializingFake(transport);
            client = std::make_unique<tests::codex::FakeAppServerClient>(transport);
            client->requests().setOnRequest([this](const typed::TypedServerRequest& request) {
                ++typedCallbacks;
                if (const auto* value = std::get_if<typed::AttestationGenerateRequest>(&request)) {
                    if (std::holds_alternative<std::string>(value->requestId.value())) {
                        attestationSuccess = *value;
                    } else {
                        attestationError = *value;
                    }
                } else if (const auto* value = std::get_if<typed::DynamicToolCallRequest>(&request)) {
                    if (std::holds_alternative<std::int64_t>(value->requestId.value())) {
                        dynamicSuccess = *value;
                    } else {
                        dynamicError = *value;
                    }
                } else {
                    result.expectTrue(false, "attestation/dynamic-tool wire input decodes only to its two exact typed alternatives");
                }
                if (typedCallbacks == 4) {
                    answerOutOfOrder();
                }
            });
            client->setOnStateChanged([this](const codex::StateChange& change) {
                if (change.current == codex::State::Ready && !injected) {
                    injected = true;
                    injectRequests();
                } else if (change.current == codex::State::Stopped && stopping) {
                    core::EventReceiver::atNextTick([this]() {
                        client.reset();
                        finished = true;
                        core::SNodeC::stop();
                    });
                }
            });
            client->start();
        }

        [[nodiscard]] bool isFinished() const noexcept {
            return finished;
        }

        void verifyFinal() {
            result.expectEqual(std::size_t{4}, typedCallbacks, "all four attestation/dynamic-tool occurrences dispatch exactly once");
            std::vector<codex::Json> responses;
            for (const codex::Json& envelope : transport->outgoing) {
                if (envelope.contains("id") && (envelope.contains("result") || envelope.contains("error"))) {
                    responses.push_back(envelope);
                }
            }
            result.expectEqual(std::size_t{4}, responses.size(), "only four terminal wire responses are emitted");
            if (responses.size() == 4) {
                result.expectTrue(
                    responses[0].at("id") == 402 &&
                        responses[0].at("result") ==
                            codex::Json{
                                {"contentItems",
                                 codex::Json::array({{{"futureItem", true}, {"text", "synthetic tool output"}, {"type", "inputText"}}})},
                                {"futureResult", true},
                                {"success", true}},
                    "dynamic-tool success preserves its integer ID and exact typed result");
                result.expectTrue(responses[1].at("id") == 403 &&
                                      responses[1].at("error") == codex::Json{{"code", -32'403},
                                                                              {"data", {{"reason", "synthetic-attestation-decline"}}},
                                                                              {"message", "attestation declined"}},
                                  "attestation rejection preserves its integer ID and exact JSON-RPC error");
                result.expectTrue(responses[2].at("id") == "attestation-success" &&
                                      responses[2].at("result") ==
                                          codex::Json{{"futureResult", true}, {"token", "opaque-attestation-token"}},
                                  "attestation success preserves its string ID and exact typed result");
                result.expectTrue(responses[3].at("id") == "dynamic-error" &&
                                      responses[3].at("error") == codex::Json{{"code", -32'404},
                                                                              {"data", {{"reason", "synthetic-tool-decline"}}},
                                                                              {"message", "dynamic tool declined"}},
                                  "dynamic-tool rejection preserves its string ID and exact JSON-RPC error");
            }
        }

    private:
        void injectRequests() {
            transport->inject({
                {"id", "attestation-success"},
                {"method", "attestation/generate"},
                {"params", {{"futureAttestation", true}}},
            });
            transport->inject({
                {"id", 402},
                {"method", "item/tool/call"},
                {"params",
                 {{"arguments", {{"safe", true}}},
                  {"callId", "call-success"},
                  {"threadId", "thread-success"},
                  {"tool", "tool-success"},
                  {"turnId", "turn-success"}}},
            });
            transport->inject({
                {"id", 403},
                {"method", "attestation/generate"},
                {"params", codex::Json::object()},
            });
            transport->inject({
                {"id", "dynamic-error"},
                {"method", "item/tool/call"},
                {"params",
                 {{"arguments", nullptr},
                  {"callId", "call-error"},
                  {"namespace", nullptr},
                  {"threadId", "thread-error"},
                  {"tool", "tool-error"},
                  {"turnId", "turn-error"}}},
            });
        }

        void answerOutOfOrder() {
            result.expectTrue(attestationSuccess && dynamicSuccess && attestationError && dynamicError,
                              "both request kinds preserve integer and string occurrences before response");
            if (!attestationSuccess || !dynamicSuccess || !attestationError || !dynamicError) {
                stop();
                return;
            }

            // Start from a fully decoded request to avoid GCC 16's false-positive
            // maybe-uninitialized diagnostic for nested nlohmann::json aggregates.
            typed::AttestationGenerateRequest wrongMethod = *attestationSuccess;
            wrongMethod.requestId = dynamicSuccess->requestId;
            wrongMethod.requestToken = dynamicSuccess->requestToken;
            wrongMethod.raw = dynamicSuccess->raw;
            const auto wrongMethodResult =
                client->requests().respond(wrongMethod, typed::AttestationGenerateResponse{"not-sent", codex::Json::object()});
            result.expectTrue(!wrongMethodResult && wrongMethodResult.error && wrongMethodResult.error->code == EINVAL,
                              "method-bound ownership rejects a valid token attached to the wrong request kind");

            typed::DynamicToolCallResponse malformed;
            malformed.raw = nullptr;
            const auto malformedResult = client->requests().respond(*dynamicSuccess, malformed);
            result.expectTrue(!malformedResult && malformedResult.error &&
                                  malformedResult.error->category == codex::Error::Category::Protocol,
                              "malformed successful responses are rejected locally without retiring their occurrence");

            typed::DynamicToolCallResponse dynamicResponse;
            dynamicResponse.contentItems = {
                typed::InputTextDynamicToolCallOutputContentItem{"synthetic tool output", {{"futureItem", true}}, {}}};
            dynamicResponse.success = true;
            dynamicResponse.raw = {{"futureResult", true}};
            const auto dynamicResult = client->requests().respond(*dynamicSuccess, std::move(dynamicResponse));
            result.expectTrue(static_cast<bool>(dynamicResult), "dynamic-tool occurrence accepts its corrected typed response");
            const auto dynamicDuplicate = client->requests().respond(*dynamicSuccess, typed::DynamicToolCallResponse{});
            result.expectTrue(!dynamicDuplicate && dynamicDuplicate.error && dynamicDuplicate.error->code == ENOENT,
                              "a completed dynamic-tool occurrence rejects a duplicate response");

            const auto attestationReject = client->requests().reject(
                *attestationError,
                codex::ProtocolError{
                    -32'403, "attestation declined", std::optional<codex::Json>{codex::Json{{"reason", "synthetic-attestation-decline"}}}});
            result.expectTrue(static_cast<bool>(attestationReject), "attestation occurrence accepts an explicit typed JSON-RPC rejection");

            const auto attestationResult = client->requests().respond(*attestationSuccess,
                                                                      typed::AttestationGenerateResponse{
                                                                          "opaque-attestation-token",
                                                                          {{"futureResult", true}},
                                                                      });
            result.expectTrue(static_cast<bool>(attestationResult), "attestation occurrence accepts its typed successful response");

            const auto dynamicReject = client->requests().reject(
                *dynamicError,
                codex::ProtocolError{
                    -32'404, "dynamic tool declined", std::optional<codex::Json>{codex::Json{{"reason", "synthetic-tool-decline"}}}});
            result.expectTrue(static_cast<bool>(dynamicReject), "dynamic-tool occurrence accepts an explicit typed JSON-RPC rejection");
            stop();
        }

        void stop() {
            if (stopping) {
                return;
            }
            stopping = true;
            core::EventReceiver::atNextTick([this]() {
                client->stop();
            });
        }

        tests::support::TestResult& result;
        std::shared_ptr<tests::codex::FakeTransportState> transport;
        std::unique_ptr<tests::codex::FakeAppServerClient> client;
        std::optional<typed::AttestationGenerateRequest> attestationSuccess;
        std::optional<typed::DynamicToolCallRequest> dynamicSuccess;
        std::optional<typed::AttestationGenerateRequest> attestationError;
        std::optional<typed::DynamicToolCallRequest> dynamicError;
        std::size_t typedCallbacks = 0;
        bool injected = false;
        bool stopping = false;
        bool finished = false;
    };
} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult result;
    core::SNodeC::init(argc, argv);

    bool timedOut = false;
    WireRunner runner(result);
    [[maybe_unused]] core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
        [&timedOut]() {
            timedOut = true;
            core::SNodeC::stop();
        },
        utils::Timeval({7, 0}));

    runner.start();
    const int eventLoopResult = core::SNodeC::start(utils::Timeval({9, 0}));
    result.expectTrue(!timedOut && runner.isFinished(),
                      "the asynchronous attestation/dynamic-tool reverse-request scenario completes without polling or sleeps");
    result.expectEqual(0, eventLoopResult, "the attestation/dynamic-tool reverse-request scenario stops the SNode.C EventLoop cleanly");
    runner.verifyFinal();
    core::SNodeC::free();
    return result.processResult();
}
