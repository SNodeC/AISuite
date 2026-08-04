/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "support/TestResult.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;

    const frontend::Json& goldenFixtures() {
        static const frontend::Json fixtures = [] {
            std::ifstream stream(CODEX_FRONTEND_GOLDEN_FIXTURE);
            if (!stream) {
                throw std::runtime_error("unable to open generated frontend protocol fixtures");
            }
            return frontend::Json::parse(stream);
        }();
        return fixtures;
    }

    void testAllDefinedMethods(tests::support::TestResult& result) {
        bool roundTrip = true;
        bool malformedRejected = true;
        bool nullableRoundTrip = true;
        std::size_t count = 0;
        std::size_t nullableForms = 0;
        const frontend::Json& fixtures = goldenFixtures();
        for (const frontend::Json& fixture : fixtures.at("methods")) {
            const std::string method = fixture.at("method").get<std::string>();
            const std::optional<generated::MethodId> methodId = generated::definedMethodFromString(method);
            if (!methodId.has_value()) {
                roundTrip = false;
                continue;
            }
            for (const std::string_view form : {"minimalParams", "completeParams"}) {
                frontend::Json encoded{
                    {"protocol", frontend::ProtocolIdentity},
                    {"version", frontend::ProtocolVersion},
                    {"kind", "command"},
                    {"requestId", "request-" + std::to_string(count)},
                    {"method", method},
                    {"params", fixture.at(form)},
                    {"futureEnvelope", count},
                };
                encoded["params"]["futureParameter"] = count;
                const auto decoded = frontend::Codec::decodeDefinedCommand(encoded);
                const auto reencoded = decoded ? frontend::Codec::encodeDefinedCommand(decoded.value())
                                               : frontend::CodecResult<frontend::Json>{decoded.error()};
                const auto serialized = decoded ? frontend::Codec::serializeDefinedCommand(decoded.value())
                                                : frontend::CodecResult<std::string>{decoded.error()};
                roundTrip = roundTrip && decoded.hasValue() && reencoded.hasValue() && serialized.hasValue() &&
                            generated::commandMethod(decoded.value().parameters) == *methodId && reencoded.value() == encoded;
            }
            for (const std::string_view form : {"minimalResult", "completeResult"}) {
                const frontend::Json& resultValue = fixture.at(form);
                const auto decodedResult = frontend::Codec::decodeDefinedResult(*methodId, resultValue);
                const auto encodedResult = decodedResult ? frontend::Codec::encodeDefinedResult(decodedResult.value())
                                                         : frontend::CodecResult<frontend::Json>{decodedResult.error()};
                roundTrip = roundTrip && decodedResult.hasValue() && encodedResult.hasValue() &&
                            generated::commandMethod(decodedResult.value()) == *methodId && encodedResult.value() == resultValue;
            }
            for (const frontend::Json& params : fixture.at("nullableParams")) {
                const frontend::Json encoded{
                    {"protocol", frontend::ProtocolIdentity},
                    {"version", frontend::ProtocolVersion},
                    {"kind", "command"},
                    {"requestId", "nullable-" + std::to_string(nullableForms)},
                    {"method", method},
                    {"params", params},
                };
                const auto decoded = frontend::Codec::decodeDefinedCommand(encoded);
                const auto reencoded = decoded ? frontend::Codec::encodeDefinedCommand(decoded.value())
                                               : frontend::CodecResult<frontend::Json>{decoded.error()};
                nullableRoundTrip = nullableRoundTrip && decoded.hasValue() && reencoded.hasValue() && reencoded.value() == encoded;
                ++nullableForms;
            }
            for (const frontend::Json& resultValue : fixture.at("nullableResults")) {
                const auto decoded = frontend::Codec::decodeDefinedResult(*methodId, resultValue);
                const auto encoded = decoded ? frontend::Codec::encodeDefinedResult(decoded.value())
                                             : frontend::CodecResult<frontend::Json>{decoded.error()};
                nullableRoundTrip = nullableRoundTrip && decoded.hasValue() && encoded.hasValue() && encoded.value() == resultValue;
                ++nullableForms;
            }
            const frontend::Json malformedCommand{
                {"protocol", frontend::ProtocolIdentity},
                {"version", frontend::ProtocolVersion},
                {"kind", "command"},
                {"requestId", "malformed-" + std::to_string(count)},
                {"method", method},
                {"params", fixture.at("malformedParams")},
            };
            const auto malformedParameters = frontend::Codec::decodeDefinedCommand(malformedCommand);
            const auto malformedResult = frontend::Codec::decodeDefinedResult(*methodId, fixture.at("malformedResult"));
            malformedRejected = malformedRejected && !malformedParameters &&
                                malformedParameters.error().code == frontend::ErrorCode::InvalidField && !malformedResult &&
                                malformedResult.error().code == frontend::ErrorCode::InvalidField;
            ++count;
        }
        result.expectTrue(roundTrip && count == 105,
                          "the complete pure codec schema-validates and round-trips minimal and representative forms for all 105 "
                          "method/result identities while preserving future fields");
        result.expectTrue(nullableRoundTrip && nullableForms != 0,
                          "all generated omitted/value fixtures and every schema-permitted top-level null form round-trip exactly");
        result.expectTrue(malformedRejected,
                          "every one of the 105 methods rejects malformed parameter and result shapes through its exact schema");
    }

    void testRequiredFieldsAndExactLookup(tests::support::TestResult& result) {
        const auto threadRead = generated::definedMethodFromString("thread.read");
        result.expectTrue(threadRead.has_value(), "thread.read is present in the defined method table");
        if (!threadRead.has_value()) {
            return;
        }

        const frontend::Json missingRequired{
            {"protocol", frontend::ProtocolIdentity},
            {"version", frontend::ProtocolVersion},
            {"kind", "command"},
            {"requestId", "missing"},
            {"method", "thread.read"},
            {"params", frontend::Json::object()},
        };
        const auto missing = frontend::Codec::decodeDefinedCommand(missingRequired);
        result.expectTrue(!missing && missing.error().code == frontend::ErrorCode::MissingField,
                          "complete command decoding rejects a missing schema-required field");

        frontend::Json unknownSuffix = missingRequired;
        unknownSuffix["method"] = "thread.read.extra";
        const auto unknown = frontend::Codec::decodeDefinedCommand(unknownSuffix);
        result.expectTrue(!unknown && unknown.error().code == frontend::ErrorCode::UnknownMethod,
                          "complete command decoding uses exact matching and rejects unknown suffixes");

        generated::DefinedCommand conflict{
            "conflict",
            generated::makeParameters(*threadRead, {{"threadId", "thread-1"}}),
            frontend::Json::object(),
            {{"threadId", "thread-2"}},
        };
        const auto conflictResult = frontend::Codec::encodeDefinedCommand(conflict);
        result.expectTrue(!conflictResult && conflictResult.error().code == frontend::ErrorCode::InvalidField,
                          "parameter extensions cannot shadow a schema-defined field");

        frontend::Json sandboxConflict{{"protocol", frontend::ProtocolIdentity},
                                       {"version", frontend::ProtocolVersion},
                                       {"kind", "command"},
                                       {"requestId", "sandbox-conflict"},
                                       {"method", "thread.start"},
                                       {"params", {{"sandbox", "readOnly"}, {"sandboxMode", "readOnly"}}}};
        const auto rejectedSandboxConflict = frontend::Codec::decodeDefinedCommand(sandboxConflict);
        result.expectTrue(!rejectedSandboxConflict && rejectedSandboxConflict.error().code == frontend::ErrorCode::InvalidField,
                          "the canonical sandbox field and legacy sandboxMode alias cannot conflict");

        frontend::Json effortConflict{{"protocol", frontend::ProtocolIdentity},
                                      {"version", frontend::ProtocolVersion},
                                      {"kind", "command"},
                                      {"requestId", "effort-conflict"},
                                      {"method", "turn.start"},
                                      {"params",
                                       {{"threadId", "thread-1"},
                                        {"input", {{{"type", "text"}, {"text", "hello"}}}},
                                        {"effort", "medium"},
                                        {"reasoningEffort", "medium"}}}};
        const auto rejectedEffortConflict = frontend::Codec::decodeDefinedCommand(effortConflict);
        result.expectTrue(!rejectedEffortConflict && rejectedEffortConflict.error().code == frontend::ErrorCode::InvalidField,
                          "the canonical effort field and legacy reasoningEffort alias cannot conflict");

        frontend::Json wrongType = missingRequired;
        wrongType["params"] = {{"threadId", 7}};
        const auto malformedType = frontend::Codec::decodeDefinedCommand(wrongType);
        result.expectTrue(!malformedType && malformedType.error().code == frontend::ErrorCode::InvalidField,
                          "complete command decoding applies schema field types rather than only checking field presence");

        const auto modelList = generated::definedMethodFromString("model.list");
        const bool rejectsMalformedResult = modelList.has_value() && [&modelList] {
            const auto malformedResult = frontend::Codec::decodeDefinedResult(*modelList, frontend::Json::array());
            return !malformedResult && malformedResult.error().code == frontend::ErrorCode::InvalidField;
        }();
        result.expectTrue(rejectsMalformedResult, "complete result decoding rejects a value outside the exact result schema");

        frontend::Json invalidUtf8{
            {"protocol", frontend::ProtocolIdentity},
            {"version", frontend::ProtocolVersion},
            {"kind", "command"},
            {"requestId", "invalid-utf8"},
            {"method", "model.list"},
            {"params", {{"cursor", std::string(1, static_cast<char>(0xFF))}}},
        };
        const auto malformedUtf8 = frontend::Codec::decodeDefinedCommand(invalidUtf8);
        result.expectTrue(!malformedUtf8 && malformedUtf8.error().code == frontend::ErrorCode::InvalidField,
                          "programmatic JSON with malformed UTF-8 is rejected before serialization");

        const auto accountRead = generated::definedMethodFromString("account.read");
        frontend::Json credentialResultValue = frontend::Json::object();
        for (const frontend::Json& fixture : goldenFixtures().at("methods")) {
            if (fixture.at("method") == "account.read") {
                credentialResultValue = fixture.at("minimalResult");
                break;
            }
        }
        credentialResultValue["accessToken"] = "SECRET";
        const auto credentialResult =
            accountRead ? frontend::Codec::decodeDefinedResult(*accountRead, credentialResultValue)
                        : frontend::CodecResult<generated::CompleteCommandResult>{frontend::CodecError{
                              frontend::ErrorCode::UnknownMethod, "account.read is missing", false, {}, std::nullopt, std::nullopt}};
        result.expectTrue(!credentialResult && credentialResult.error().code == frontend::ErrorCode::InvalidField,
                          "credential-named unknown result fields are rejected by the generated safe-result schema");
    }

    void testRuntimeGate(tests::support::TestResult& result) {
        const frontend::Json additiveCommand{
            {"protocol", frontend::ProtocolIdentity},
            {"version", frontend::ProtocolVersion},
            {"kind", "command"},
            {"requestId", "unavailable"},
            {"method", "command.exec.resize"},
            {"params", {{"processId", "process-1"}, {"size", {{"cols", 80}, {"rows", 24}}}}},
        };

        const auto defined = frontend::Codec::decodeDefinedCommand(additiveCommand);
        const auto runtime = frontend::Codec::decodeClient(additiveCommand);
        result.expectTrue(defined.hasValue() &&
                              generated::commandMethod(defined.value().parameters) == generated::MethodId::CommandExecResize,
                          "the additive contract codec recognizes a defined A1.7a method");
        result.expectTrue(!runtime && runtime.error().code == frontend::ErrorCode::UnknownMethod,
                          "the production v1 decoder keeps every additive method unavailable until A1.7b");
    }

    void testExpandedStateAndEvents(tests::support::TestResult& result) {
        const frontend::Json& fixtures = goldenFixtures();
        const frontend::Json& snapshotFixture = fixtures.at("expandedSnapshot");
        const auto decodedSnapshot = frontend::Codec::decodeExpandedSnapshot(snapshotFixture);
        const auto encodedSnapshot = decodedSnapshot ? frontend::Codec::encodeExpandedSnapshot(decodedSnapshot.value())
                                                     : frontend::CodecResult<frontend::Json>{decodedSnapshot.error()};
        result.expectTrue(decodedSnapshot.hasValue() && encodedSnapshot.hasValue() && encodedSnapshot.value() == snapshotFixture,
                          "the complete bounded expanded snapshot fixture round-trips exactly");
        if (decodedSnapshot) {
            result.expectTrue(decodedSnapshot.value().state.items.has_value() && decodedSnapshot.value().state.items->size() == 18 &&
                                  decodedSnapshot.value().state.pendingRequests.has_value() &&
                                  decodedSnapshot.value().state.pendingRequests->size() == 10,
                              "expanded state preserves all 18 safe item projections and ten dedicated pending-request kinds");
            result.expectTrue(decodedSnapshot.value().state.items->front().agentText.has_value() &&
                                  decodedSnapshot.value().state.items->front().data.has_value(),
                              "expanded ThreadItem value types retain bounded renderable content and safe detail fields");

            const auto userInput = std::find_if(decodedSnapshot.value().state.pendingRequests->begin(),
                                                decodedSnapshot.value().state.pendingRequests->end(),
                                                [](const frontend::ExpandedPendingRequest& request) {
                                                    return request.kind == frontend::PendingRequestKind::UserInput;
                                                });
            const bool completeUserInput = userInput != decodedSnapshot.value().state.pendingRequests->end() &&
                                           userInput->questions.has_value() && userInput->questions->size() == 1 &&
                                           userInput->autoResolutionMs == 60'000 && userInput->questions->front().id == "question-1" &&
                                           userInput->questions->front().allowsFreeText && userInput->questions->front().isSecret &&
                                           userInput->questions->front().options.size() == 1 &&
                                           userInput->questions->front().options.front().label == "safe" &&
                                           userInput->questions->front().extensions.at("futureQuestionHint") == "preserved" &&
                                           userInput->questions->front().options.front().extensions.at("futureOptionHint") == true;
            result.expectTrue(completeUserInput,
                              "user-input pending requests retain bounded typed questions, options, the secret-input flag, "
                              "auto-resolution metadata, and safe future fields without retaining an answer");
        }

        bool eventsRoundTrip = true;
        std::size_t eventCount = 0;
        for (const frontend::Json& eventFixture : fixtures.at("expandedEvents")) {
            const auto decoded = frontend::Codec::decodeExpandedEvent(eventFixture);
            const auto encoded =
                decoded ? frontend::Codec::encodeExpandedEvent(decoded.value()) : frontend::CodecResult<frontend::Json>{decoded.error()};
            eventsRoundTrip = eventsRoundTrip && decoded.hasValue() && encoded.hasValue() && encoded.value() == eventFixture;
            ++eventCount;
        }
        result.expectTrue(eventsRoundTrip && eventCount == 25,
                          "all 25 complete-backend event families use exact bounded schema-valid codec fixtures");

        frontend::Json largeSnapshot = snapshotFixture;
        const frontend::Json representativeItem = largeSnapshot.at("state").at("items").at(0);
        largeSnapshot["state"]["items"] = frontend::Json::array();
        for (std::size_t index = 0; index < 2'000; ++index) {
            frontend::Json item = representativeItem;
            item["id"] = "bounded-item-" + std::to_string(index);
            largeSnapshot["state"]["items"].push_back(std::move(item));
        }
        const auto decodedLargeSnapshot = frontend::Codec::decodeExpandedSnapshot(largeSnapshot);
        result.expectTrue(decodedLargeSnapshot.hasValue() && decodedLargeSnapshot.value().state.items.has_value() &&
                              decodedLargeSnapshot.value().state.items->size() == 2'000,
                          "a schema-valid 2,000-item bounded snapshot remains below the deterministic validator work budget");

        frontend::Json credentialSnapshot = snapshotFixture;
        credentialSnapshot["state"]["accounts"] = {{"details", {{"accessToken", "SECRET"}}}};
        const auto decodedCredentialSnapshot = frontend::Codec::decodeExpandedSnapshot(credentialSnapshot);
        result.expectTrue(!decodedCredentialSnapshot && decodedCredentialSnapshot.error().code == frontend::ErrorCode::InvalidField,
                          "expanded safe state rejects credential-named fields at every bounded detail-object boundary");

        const auto userInputIndex = [&snapshotFixture]() -> std::optional<std::size_t> {
            const auto& requests = snapshotFixture.at("state").at("pendingRequests");
            for (std::size_t index = 0; index < requests.size(); ++index) {
                if (requests.at(index).at("kind") == "user_input") {
                    return index;
                }
            }
            return std::nullopt;
        }();
        bool boundedUserInput = userInputIndex.has_value();
        if (userInputIndex.has_value()) {
            frontend::Json missingQuestions = snapshotFixture;
            missingQuestions["state"]["pendingRequests"][*userInputIndex].erase("questions");
            const auto missing = frontend::Codec::decodeExpandedSnapshot(missingQuestions);

            frontend::Json misplacedQuestions = snapshotFixture;
            misplacedQuestions["state"]["pendingRequests"][0]["questions"] =
                snapshotFixture.at("state").at("pendingRequests").at(*userInputIndex).at("questions");
            const auto misplaced = frontend::Codec::decodeExpandedSnapshot(misplacedQuestions);

            frontend::Json credentialQuestion = snapshotFixture;
            credentialQuestion["state"]["pendingRequests"][*userInputIndex]["questions"][0]["accessToken"] = "SYNTHETIC_SENTINEL";
            const auto credential = frontend::Codec::decodeExpandedSnapshot(credentialQuestion);

            frontend::Json tooManyQuestions = snapshotFixture;
            const frontend::Json representativeQuestion =
                tooManyQuestions.at("state").at("pendingRequests").at(*userInputIndex).at("questions").at(0);
            tooManyQuestions["state"]["pendingRequests"][*userInputIndex]["questions"] = frontend::Json::array();
            for (std::size_t index = 0; index < 65; ++index) {
                frontend::Json question = representativeQuestion;
                question["id"] = "question-" + std::to_string(index);
                tooManyQuestions["state"]["pendingRequests"][*userInputIndex]["questions"].push_back(std::move(question));
            }
            const auto questionOverflow = frontend::Codec::decodeExpandedSnapshot(tooManyQuestions);

            frontend::Json tooManyOptions = snapshotFixture;
            const frontend::Json representativeOption =
                tooManyOptions.at("state").at("pendingRequests").at(*userInputIndex).at("questions").at(0).at("options").at(0);
            tooManyOptions["state"]["pendingRequests"][*userInputIndex]["questions"][0]["options"] = frontend::Json::array();
            for (std::size_t index = 0; index < 65; ++index) {
                frontend::Json option = representativeOption;
                option["label"] = "option-" + std::to_string(index);
                tooManyOptions["state"]["pendingRequests"][*userInputIndex]["questions"][0]["options"].push_back(std::move(option));
            }
            const auto optionOverflow = frontend::Codec::decodeExpandedSnapshot(tooManyOptions);

            boundedUserInput = !missing && missing.error().code == frontend::ErrorCode::MissingField && !misplaced &&
                               misplaced.error().code == frontend::ErrorCode::InvalidField && !credential &&
                               credential.error().code == frontend::ErrorCode::InvalidField && !questionOverflow &&
                               questionOverflow.error().code == frontend::ErrorCode::InvalidField && !optionOverflow &&
                               optionOverflow.error().code == frontend::ErrorCode::InvalidField;
        }
        result.expectTrue(boundedUserInput,
                          "the dedicated user-input contract requires questions only on user_input, bounds questions/options at 64, "
                          "and rejects credential-shaped nested fields");
    }

    void testDiscoveryAndErrorExtensions(tests::support::TestResult& result) {
        const frontend::CapabilityAdvertisement capabilities{
            {frontend::FrontendCapability::MethodDiscovery, frontend::FrontendCapability::SecurityScopes},
            {frontend::FrontendCapability::MethodDiscovery},
            {frontend::FrontendCapability::MethodDiscovery},
            {{"futureCapability", true}},
        };
        const frontend::Welcome welcome{
            "session-1",
            frontend::SessionRole::Observer,
            frontend::SequenceNumber{5},
            frontend::SyncMode::Snapshot,
            frontend::Json::object(),
            capabilities,
            std::vector<frontend::FrontendMethod>{"snapshot.get", "thread.list"},
            std::vector<frontend::FrontendMethod>{"snapshot.get"},
            "0.1.0",
        };
        const auto encoded = frontend::Codec::encodeServer(frontend::ServerMessage{welcome});
        const auto decoded =
            encoded ? frontend::Codec::decodeServer(encoded.value()) : frontend::CodecResult<frontend::ServerMessage>{encoded.error()};
        result.expectTrue(encoded.hasValue() && decoded.hasValue() && std::get<frontend::Welcome>(decoded.value()) == welcome,
                          "additive welcome discovery metadata round-trips while remaining optional");

        constexpr std::array AddedErrors{
            frontend::ErrorCode::AuthenticationRequired,
            frontend::ErrorCode::AuthenticationFailed,
            frontend::ErrorCode::OriginRejected,
            frontend::ErrorCode::TransportSecurityRequired,
            frontend::ErrorCode::RateLimited,
        };
        result.expectTrue(std::all_of(AddedErrors.begin(),
                                      AddedErrors.end(),
                                      [](frontend::ErrorCode code) {
                                          return frontend::errorCodeFromString(frontend::toString(code)) == code;
                                      }),
                          "all five additive A1.7a protocol error codes round-trip exactly");
    }
} // namespace

int main() {
    tests::support::TestResult result;

    static_assert(std::variant_size_v<generated::CompleteCommandParameters> == 105);
    testAllDefinedMethods(result);
    testRequiredFieldsAndExactLookup(result);
    testRuntimeGate(result);
    testExpandedStateAndEvents(result);
    testDiscoveryAndErrorExtensions(result);

    return result.processResult();
}
