/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *               2020, 2021, 2022, 2023, 2024, 2025, 2026
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/AppServerClient.h"
#include "ai/openai/codex/detail/ProtocolCodec.h"
#include "support/TestResult.h"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
    using ai::openai::codex::ClientInfo;
    using ai::openai::codex::InitializeResult;
    using ai::openai::codex::Json;
    using ai::openai::codex::ProtocolError;
    using ai::openai::codex::detail::ProtocolCodec;
    using ai::openai::codex::detail::ProtocolId;
    using ai::openai::codex::detail::ProtocolMessage;
    using ai::openai::codex::typed::InitializeCapabilities;
    using ai::openai::codex::typed::InitializeClientInfo;
    using ai::openai::codex::typed::InitializeParams;
    using ai::openai::codex::typed::InitializeResponse;
    using ai::openai::codex::typed::OptionalNullable;

    bool hasIntegerId(const std::optional<ProtocolId>& id, std::int64_t expected) {
        return id && std::holds_alternative<std::int64_t>(*id) && std::get<std::int64_t>(*id) == expected;
    }

    bool hasStringId(const std::optional<ProtocolId>& id, const std::string& expected) {
        return id && std::holds_alternative<std::string>(*id) && std::get<std::string>(*id) == expected;
    }

    std::optional<ProtocolMessage> decode(const Json& message, std::string& errorMessage) {
        return ProtocolCodec::decode(message.dump(), errorMessage);
    }

    void expectWireRejected(tests::support::TestResult& testResult, const std::string& wireMessage, const std::string& description) {
        std::string errorMessage;
        const std::optional<ProtocolMessage> decoded = ProtocolCodec::decode(wireMessage, errorMessage);
        testResult.expectTrue(!decoded, description + " is rejected");
        testResult.expectTrue(!errorMessage.empty(), description + " reports a decode reason");
    }

    void expectRejected(tests::support::TestResult& testResult, const Json& message, const std::string& description) {
        expectWireRejected(testResult, message.dump(), description);
    }

    void testRequestsAndNotifications(tests::support::TestResult& testResult) {
        const Json integerRequest = {
            {"method", "thread/list"},
            {"id", 17},
            {"params", {{"cursor", nullptr}}},
            {"futureField", Json::array({1, "two"})},
        };
        std::string errorMessage;
        const std::optional<ProtocolMessage> decodedIntegerRequest = decode(integerRequest, errorMessage);
        testResult.expectTrue(decodedIntegerRequest && decodedIntegerRequest->kind == ProtocolMessage::Kind::Request,
                              "integer-ID request is decoded as a request");
        testResult.expectTrue(decodedIntegerRequest && hasIntegerId(decodedIntegerRequest->id, 17),
                              "integer request ID is preserved as signed 64-bit");
        testResult.expectTrue(decodedIntegerRequest && decodedIntegerRequest->method == "thread/list",
                              "integer-ID request method is preserved");
        testResult.expectTrue(decodedIntegerRequest && decodedIntegerRequest->params == integerRequest["params"],
                              "request object parameters are preserved");
        testResult.expectTrue(decodedIntegerRequest && decodedIntegerRequest->raw == integerRequest,
                              "request raw envelope and unknown fields are preserved");

        const Json stringRequest = {
            {"method", "item/commandExecution/requestApproval"},
            {"id", "approval-123"},
            {"params", Json::array({"first", 2, false, nullptr})},
        };
        errorMessage = "stale";
        const std::optional<ProtocolMessage> decodedStringRequest = decode(stringRequest, errorMessage);
        testResult.expectTrue(decodedStringRequest && decodedStringRequest->kind == ProtocolMessage::Kind::Request,
                              "string-ID request is decoded as a request");
        testResult.expectTrue(decodedStringRequest && hasStringId(decodedStringRequest->id, "approval-123"),
                              "string request ID is preserved exactly");
        testResult.expectTrue(decodedStringRequest && decodedStringRequest->params == stringRequest["params"],
                              "request array parameters are accepted");
        testResult.expectTrue(decodedStringRequest && decodedStringRequest->raw == stringRequest,
                              "string-ID request retains its complete raw envelope");
        testResult.expectTrue(errorMessage.empty(), "successful request decoding clears a previous error");

        const Json notification = {
            {"method", "future/notification"},
            {"params", true},
            {"jsonrpc", "2.0"},
            {"extension", {{"enabled", true}}},
        };
        const std::optional<ProtocolMessage> decodedNotification = decode(notification, errorMessage);
        testResult.expectTrue(decodedNotification && decodedNotification->kind == ProtocolMessage::Kind::Notification,
                              "notification without ID is decoded as a notification");
        testResult.expectTrue(decodedNotification && !decodedNotification->id, "notification does not synthesize a request ID");
        testResult.expectTrue(decodedNotification && decodedNotification->method == "future/notification",
                              "unknown notification method is accepted");
        testResult.expectTrue(decodedNotification && decodedNotification->params == true, "notification Boolean parameters are accepted");
        testResult.expectTrue(decodedNotification && decodedNotification->raw == notification,
                              "notification raw envelope, JSON-RPC marker, and extensions are preserved");

        const Json notificationWithoutParams = {{"method", "future/withoutParams"}};
        const std::optional<ProtocolMessage> decodedWithoutParams = decode(notificationWithoutParams, errorMessage);
        testResult.expectTrue(decodedWithoutParams && decodedWithoutParams->kind == ProtocolMessage::Kind::Notification &&
                                  decodedWithoutParams->params.is_null(),
                              "missing notification params decode as null without rejecting an unknown method");

        const Json minimumIntegerRequest = {
            {"method", "future/minimumId"},
            {"id", std::numeric_limits<std::int64_t>::min()},
        };
        const std::optional<ProtocolMessage> decodedMinimumIntegerRequest = decode(minimumIntegerRequest, errorMessage);
        testResult.expectTrue(decodedMinimumIntegerRequest &&
                                  hasIntegerId(decodedMinimumIntegerRequest->id, std::numeric_limits<std::int64_t>::min()),
                              "minimum signed 64-bit request ID is accepted");

        const Json maximumIntegerRequest = {
            {"method", "future/maximumId"},
            {"id", std::numeric_limits<std::int64_t>::max()},
        };
        const std::optional<ProtocolMessage> decodedMaximumIntegerRequest = decode(maximumIntegerRequest, errorMessage);
        testResult.expectTrue(decodedMaximumIntegerRequest &&
                                  hasIntegerId(decodedMaximumIntegerRequest->id, std::numeric_limits<std::int64_t>::max()),
                              "maximum signed 64-bit request ID is accepted");
    }

    void testResultResponses(tests::support::TestResult& testResult) {
        const std::vector<std::pair<std::string, Json>> resultCases = {
            {"object", Json{{"data", Json::array({1, 2})}}},
            {"array", Json::array({Json{{"id", "a"}}, Json{{"id", "b"}}})},
            {"string scalar", Json("complete")},
            {"number scalar", Json(42.5)},
            {"Boolean", Json(false)},
            {"null", Json(nullptr)},
        };

        std::int64_t id = 100;
        for (const auto& [description, result] : resultCases) {
            const Json envelope = {
                {"id", id},
                {"result", result},
                {"futureResponseField", {{"case", description}}},
            };
            std::string errorMessage;
            const std::optional<ProtocolMessage> decoded = decode(envelope, errorMessage);
            testResult.expectTrue(decoded && decoded->kind == ProtocolMessage::Kind::Response,
                                  description + " result is decoded as a response");
            testResult.expectTrue(decoded && hasIntegerId(decoded->id, id), description + " response retains its integer ID");
            testResult.expectTrue(decoded && !decoded->error, description + " result is not misclassified as a remote error");
            testResult.expectTrue(decoded && decoded->result == result, description + " result JSON is preserved exactly");
            testResult.expectTrue(decoded && decoded->raw == envelope,
                                  description + " response preserves the full raw envelope and unknown fields");
            ++id;
        }

        const Json stringIdResponse = {
            {"id", "server-originated-id"},
            {"result", Json::array()},
        };
        std::string errorMessage;
        const std::optional<ProtocolMessage> decodedStringIdResponse = decode(stringIdResponse, errorMessage);
        testResult.expectTrue(decodedStringIdResponse && decodedStringIdResponse->kind == ProtocolMessage::Kind::Response,
                              "string-ID response is structurally valid");
        testResult.expectTrue(decodedStringIdResponse && hasStringId(decodedStringIdResponse->id, "server-originated-id"),
                              "string response ID representation is preserved exactly");
    }

    void testRemoteErrors(tests::support::TestResult& testResult) {
        const Json errorWithData = {
            {"id", 201},
            {"error", {{"code", -32000}, {"message", "failure"}, {"data", {{"reason", "example"}}}, {"future", 1}}},
            {"responseExtension", true},
        };
        std::string errorMessage;
        const std::optional<ProtocolMessage> decodedWithData = decode(errorWithData, errorMessage);
        testResult.expectTrue(decodedWithData && decodedWithData->kind == ProtocolMessage::Kind::Response && decodedWithData->error,
                              "remote error with data is decoded as an error response");
        testResult.expectTrue(decodedWithData && decodedWithData->error && decodedWithData->error->code == -32000 &&
                                  decodedWithData->error->message == "failure",
                              "remote error code and message are preserved");
        testResult.expectTrue(decodedWithData && decodedWithData->error && decodedWithData->error->data &&
                                  *decodedWithData->error->data == Json{{"reason", "example"}},
                              "remote error object data is preserved");
        testResult.expectTrue(decodedWithData && decodedWithData->error && decodedWithData->error->raw == errorWithData["error"] &&
                                  decodedWithData->raw == errorWithData,
                              "remote error retains exact unknown error-object and envelope fields in raw");

        const Json errorWithNullData = {
            {"id", 202},
            {"error", {{"code", 7}, {"message", "null data"}, {"data", nullptr}}},
        };
        const std::optional<ProtocolMessage> decodedWithNullData = decode(errorWithNullData, errorMessage);
        testResult.expectTrue(decodedWithNullData && decodedWithNullData->error && decodedWithNullData->error->data.has_value() &&
                                  decodedWithNullData->error->data->is_null(),
                              "explicit null remote error data remains present and null");

        const Json errorWithoutData = {
            {"id", 203},
            {"error", {{"code", -1}, {"message", "no data"}}},
        };
        const std::optional<ProtocolMessage> decodedWithoutData = decode(errorWithoutData, errorMessage);
        testResult.expectTrue(decodedWithoutData && decodedWithoutData->error && !decodedWithoutData->error->data,
                              "absent remote error data remains absent");

        const Json errorWithScalarData = {
            {"id", "error-id"},
            {"error", {{"code", 0}, {"message", "scalar data"}, {"data", false}}},
        };
        const std::optional<ProtocolMessage> decodedWithScalarData = decode(errorWithScalarData, errorMessage);
        testResult.expectTrue(decodedWithScalarData && hasStringId(decodedWithScalarData->id, "error-id") && decodedWithScalarData->error &&
                                  decodedWithScalarData->error->data && *decodedWithScalarData->error->data == false,
                              "remote error accepts arbitrary scalar data with a string response ID");
    }

    void testMalformedAndAmbiguousMessages(tests::support::TestResult& testResult) {
        expectWireRejected(testResult, "{not-json}", "malformed JSON");
        expectRejected(testResult, Json::array({1, 2}), "non-object top-level message");
        expectRejected(testResult, Json{{"result", Json::object()}}, "response missing an ID");
        expectRejected(testResult,
                       Json{{"id", 1}, {"result", Json::object()}, {"error", {{"code", -1}, {"message", "both"}}}},
                       "response containing both result and error");
        expectRejected(testResult, Json{{"id", 1}}, "response containing neither result nor error");
        expectRejected(testResult, Json{{"method", 17}, {"params", Json::object()}}, "non-string method");
        expectRejected(testResult, Json{{"method", "future/request"}, {"id", false}}, "Boolean request ID");
        expectRejected(testResult, Json{{"id", Json::array({1})}, {"result", nullptr}}, "array response ID");
        expectRejected(testResult, Json{{"id", 1.5}, {"result", nullptr}}, "floating-point response ID");
        expectWireRejected(testResult,
                           R"({"method":"future/request","id":18446744073709551615,"params":{}})",
                           "unsigned request ID outside signed 64-bit range");
        expectRejected(testResult,
                       Json{{"method", "future/ambiguous"}, {"id", 5}, {"params", Json::object()}, {"result", nullptr}},
                       "method envelope also containing a result");
        expectRejected(testResult,
                       Json{{"method", "future/ambiguous"}, {"error", {{"code", -1}, {"message", "response-shaped notification"}}}},
                       "method envelope also containing an error");
        expectRejected(testResult, Json::object(), "empty protocol envelope");
        expectRejected(testResult, Json{{"id", 2}, {"error", "not an object"}}, "remote error whose payload is not an object");
        expectRejected(testResult,
                       Json{{"id", 3}, {"error", {{"code", "not an integer"}, {"message", "failure"}}}},
                       "remote error with a non-integer code");
        expectRejected(
            testResult, Json{{"id", 4}, {"error", {{"code", -1}, {"message", Json::object()}}}}, "remote error with a non-string message");
        std::string maximumErrorMessage;
        const auto decodedMaximumErrorCode = decode(
            Json{{"id", 5}, {"error", {{"code", std::numeric_limits<std::int64_t>::max()}, {"message", "maximum"}}}}, maximumErrorMessage);
        testResult.expectTrue(decodedMaximumErrorCode && decodedMaximumErrorCode->error &&
                                  decodedMaximumErrorCode->error->code == std::numeric_limits<std::int64_t>::max(),
                              "maximum signed 64-bit remote error code is preserved");
        testResult.expectTrue(maximumErrorMessage.empty(), "maximum signed 64-bit remote error code has no decode error");

        std::string minimumErrorMessage;
        const auto decodedMinimumErrorCode = decode(
            Json{{"id", 6}, {"error", {{"code", std::numeric_limits<std::int64_t>::min()}, {"message", "minimum"}}}}, minimumErrorMessage);
        testResult.expectTrue(decodedMinimumErrorCode && decodedMinimumErrorCode->error &&
                                  decodedMinimumErrorCode->error->code == std::numeric_limits<std::int64_t>::min(),
                              "minimum signed 64-bit remote error code is preserved");
        testResult.expectTrue(minimumErrorMessage.empty(), "minimum signed 64-bit remote error code has no decode error");
    }

    void testGenericEncoders(tests::support::TestResult& testResult) {
        std::string errorMessage = "stale";
        const Json requestParams = Json::array({Json{{"type", "text"}}, nullptr});
        const std::optional<std::string> encodedRequest = ProtocolCodec::encodeRequest(301, "turn/start", requestParams, errorMessage);
        const Json expectedRequest = {{"method", "turn/start"}, {"id", 301}, {"params", requestParams}};
        testResult.expectTrue(encodedRequest && Json::parse(*encodedRequest) == expectedRequest,
                              "generic request encoder preserves method, integer ID, and arbitrary params");
        testResult.expectTrue(encodedRequest && encodedRequest->find('\n') == std::string::npos &&
                                  encodedRequest->find('\r') == std::string::npos,
                              "generic request encoder emits an unframed JSON document");
        testResult.expectTrue(errorMessage.empty(), "successful request encoding clears a previous error");

        const Json notificationParams = "delta";
        const std::optional<std::string> encodedNotification =
            ProtocolCodec::encodeNotification("future/notification", notificationParams, errorMessage);
        const Json expectedNotification = {{"method", "future/notification"}, {"params", notificationParams}};
        testResult.expectTrue(encodedNotification && Json::parse(*encodedNotification) == expectedNotification,
                              "generic notification encoder preserves an unknown method and scalar params");
        testResult.expectTrue(encodedNotification && encodedNotification->find('\n') == std::string::npos &&
                                  encodedNotification->find('\r') == std::string::npos,
                              "generic notification encoder emits an unframed JSON document");

        const ProtocolId integerResponseId = std::int64_t{302};
        const Json successResult = Json{{"accepted", true}};
        const std::optional<std::string> encodedIntegerSuccess =
            ProtocolCodec::encodeSuccessResponse(integerResponseId, successResult, errorMessage);
        const Json expectedIntegerSuccess = {{"id", 302}, {"result", successResult}};
        testResult.expectTrue(encodedIntegerSuccess && Json::parse(*encodedIntegerSuccess) == expectedIntegerSuccess,
                              "generic success encoder preserves an integer server-request ID and result");

        const ProtocolId stringResponseId = std::string("approval-302");
        const Json arrayResult = Json::array({"accepted", 1, nullptr});
        const std::optional<std::string> encodedStringSuccess =
            ProtocolCodec::encodeSuccessResponse(stringResponseId, arrayResult, errorMessage);
        const Json expectedStringSuccess = {{"id", "approval-302"}, {"result", arrayResult}};
        testResult.expectTrue(encodedStringSuccess && Json::parse(*encodedStringSuccess) == expectedStringSuccess,
                              "generic success encoder preserves a string server-request ID exactly");

        const ProtocolError rejection = {
            .code = -32000,
            .message = "Request rejected",
            .data = std::optional<Json>{Json{{"reason", "policy"}}},
            .raw = Json{{"code", 17}, {"message", "must not override typed fields"}, {"future", "incoming-only"}},
        };
        const std::optional<std::string> encodedRejection = ProtocolCodec::encodeErrorResponse(stringResponseId, rejection, errorMessage);
        const Json expectedRejection = {
            {"id", "approval-302"},
            {"error", {{"code", -32000}, {"message", "Request rejected"}, {"data", {{"reason", "policy"}}}}},
        };
        testResult.expectTrue(encodedRejection && Json::parse(*encodedRejection) == expectedRejection,
                              "generic rejection encoder preserves typed fields and excludes incoming-only raw extensions");
        testResult.expectTrue(encodedRejection && encodedRejection->find('\n') == std::string::npos &&
                                  encodedRejection->find('\r') == std::string::npos,
                              "generic error encoder emits an unframed JSON document");

        const ProtocolError rejectionWithoutData = {
            .code = -1,
            .message = "without data",
            .data = std::nullopt,
        };
        const std::optional<std::string> encodedWithoutData =
            ProtocolCodec::encodeErrorResponse(integerResponseId, rejectionWithoutData, errorMessage);
        testResult.expectTrue(encodedWithoutData && !Json::parse(*encodedWithoutData)["error"].contains("data"),
                              "generic rejection encoder omits absent error data");

        const ProtocolError rejectionWithNullData = {
            .code = -2,
            .message = "null data",
            .data = std::optional<Json>{Json(nullptr)},
        };
        const std::optional<std::string> encodedWithNullData =
            ProtocolCodec::encodeErrorResponse(integerResponseId, rejectionWithNullData, errorMessage);
        testResult.expectTrue(encodedWithNullData && Json::parse(*encodedWithNullData)["error"].contains("data") &&
                                  Json::parse(*encodedWithNullData)["error"]["data"].is_null(),
                              "generic rejection encoder preserves explicitly null error data");

        const Json invalidUtf8 = std::string(1, static_cast<char>(0xff));
        errorMessage.clear();
        const std::optional<std::string> invalidEncoding =
            ProtocolCodec::encodeRequest(303, "future/invalidUtf8", invalidUtf8, errorMessage);
        testResult.expectTrue(!invalidEncoding && !errorMessage.empty(), "encoder reports invalid JSON string encoding safely");

        const Json discarded = Json::parse("{", nullptr, false);
        errorMessage.clear();
        const std::optional<std::string> discardedEncoding =
            ProtocolCodec::encodeNotification("future/discarded", Json{{"nested", discarded}}, errorMessage);
        testResult.expectTrue(!discardedEncoding && !errorMessage.empty(),
                              "encoder rejects discarded parser values instead of emitting invalid JSON");

        const Json nonFinite = std::numeric_limits<double>::quiet_NaN();
        errorMessage.clear();
        const std::optional<std::string> nonFiniteEncoding =
            ProtocolCodec::encodeSuccessResponse(integerResponseId, nonFinite, errorMessage);
        testResult.expectTrue(!nonFiniteEncoding && !errorMessage.empty(),
                              "encoder rejects non-finite numbers instead of silently changing them to null");
    }

    void testInitializationCodec(tests::support::TestResult& testResult) {
        ClientInfo clientInfo;
        clientInfo.name = "codec_test";
        clientInfo.title = "Codec Test";
        clientInfo.version = "9.8.7";

        const std::string initialize = ProtocolCodec::initializeRequest(401, clientInfo);
        testResult.expectTrue(!initialize.empty(), "initialize encoding produces a JSON document");
        testResult.expectTrue(initialize.find('\n') == std::string::npos && initialize.find('\r') == std::string::npos,
                              "initialize encoding contains no stdio line delimiter");

        std::string errorMessage;
        const std::optional<ProtocolMessage> initializeMessage = ProtocolCodec::decode(initialize, errorMessage);
        testResult.expectTrue(initializeMessage && initializeMessage->kind == ProtocolMessage::Kind::Request &&
                                  hasIntegerId(initializeMessage->id, 401) && initializeMessage->method == "initialize",
                              "initialize wrapper uses the generic correlated request encoder");
        testResult.expectTrue(initializeMessage && initializeMessage->params["clientInfo"]["name"] == "codec_test" &&
                                  initializeMessage->params["clientInfo"]["title"] == "Codec Test" &&
                                  initializeMessage->params["clientInfo"]["version"] == "9.8.7",
                              "legacy ClientInfo maps to the complete canonical client information");
        testResult.expectTrue(initializeMessage && !initializeMessage->params.contains("capabilities"),
                              "legacy ClientInfo preserves the existing omitted-capabilities behavior");

        const std::array<std::pair<OptionalNullable<std::string>, Json>, 3> titleCases{{
            {OptionalNullable<std::string>::omitted(), Json()},
            {OptionalNullable<std::string>::explicitNull(), Json(nullptr)},
            {OptionalNullable<std::string>::withValue("Canonical Title"), Json("Canonical Title")},
        }};
        std::int64_t canonicalId = 410;
        for (const auto& [title, expected] : titleCases) {
            InitializeParams params{
                InitializeClientInfo{"canonical-client", "2.0", title, Json{{"futureClientInfo", true}, {"title", "raw must not win"}}}};
            params.raw = Json{{"futureInitialize", Json{{"retained", true}}}, {"capabilities", Json{{"stale", true}}}};
            const Json encoded = Json::parse(ProtocolCodec::initializeRequest(canonicalId++, params));
            const Json& encodedInfo = encoded.at("params").at("clientInfo");
            testResult.expectTrue(encodedInfo.at("name") == "canonical-client" && encodedInfo.at("version") == "2.0" &&
                                      encodedInfo.at("futureClientInfo") == true,
                                  "canonical client information overlays known fields while preserving future fields");
            testResult.expectTrue(title.isOmitted() ? !encodedInfo.contains("title") : encodedInfo.at("title") == expected,
                                  "client title preserves omitted, explicit-null, and string states");
            testResult.expectTrue(encoded.at("params").at("futureInitialize").at("retained") == true &&
                                      !encoded.at("params").contains("capabilities"),
                                  "open InitializeParams raw fields survive while omitted capabilities override stale raw data");
        }

        InitializeParams nullCapabilities{
            InitializeClientInfo{"canonical-client", "2.0", OptionalNullable<std::string>::omitted(), Json::object()}};
        nullCapabilities.capabilities = OptionalNullable<InitializeCapabilities>::explicitNull();
        const Json nullCapabilitiesWire = Json::parse(ProtocolCodec::initializeRequest(420, nullCapabilities));
        testResult.expectTrue(nullCapabilitiesWire.at("params").contains("capabilities") &&
                                  nullCapabilitiesWire.at("params").at("capabilities").is_null(),
                              "root capabilities preserve explicit null separately from omission");

        const std::array<std::optional<bool>, 3> booleanStates{{std::nullopt, false, true}};
        for (const std::optional<bool> state : booleanStates) {
            InitializeCapabilities capabilities;
            capabilities.experimentalApi = state;
            capabilities.mcpServerOpenaiFormElicitation = state;
            capabilities.requestAttestation = state;
            capabilities.raw = Json{{"futureCapability", 7}, {"experimentalApi", "stale"}};
            InitializeParams params{
                InitializeClientInfo{"canonical-client", "2.0", OptionalNullable<std::string>::omitted(), Json::object()}};
            params.capabilities = OptionalNullable<InitializeCapabilities>::withValue(std::move(capabilities));
            const Json encodedCapabilities =
                Json::parse(ProtocolCodec::initializeRequest(canonicalId++, params)).at("params").at("capabilities");
            for (const char* field : {"experimentalApi", "mcpServerOpenaiFormElicitation", "requestAttestation"}) {
                testResult.expectTrue(state ? encodedCapabilities.at(field) == *state : !encodedCapabilities.contains(field),
                                      std::string(field) + " preserves omitted, false, and true states");
            }
            testResult.expectTrue(encodedCapabilities.at("futureCapability") == 7,
                                  "open InitializeCapabilities future fields are preserved");
        }

        const std::array<std::pair<OptionalNullable<std::vector<std::string>>, Json>, 4> optOutCases{{
            {OptionalNullable<std::vector<std::string>>::omitted(), Json()},
            {OptionalNullable<std::vector<std::string>>::explicitNull(), Json(nullptr)},
            {OptionalNullable<std::vector<std::string>>::withValue({}), Json::array()},
            {OptionalNullable<std::vector<std::string>>::withValue({"thread/started", "warning"}),
             Json::array({"thread/started", "warning"})},
        }};
        for (const auto& [methods, expected] : optOutCases) {
            InitializeCapabilities capabilities;
            capabilities.optOutNotificationMethods = methods;
            InitializeParams params{
                InitializeClientInfo{"canonical-client", "2.0", OptionalNullable<std::string>::omitted(), Json::object()}};
            params.capabilities = OptionalNullable<InitializeCapabilities>::withValue(std::move(capabilities));
            const Json encodedCapabilities =
                Json::parse(ProtocolCodec::initializeRequest(canonicalId++, params)).at("params").at("capabilities");
            testResult.expectTrue(methods.isOmitted() ? !encodedCapabilities.contains("optOutNotificationMethods")
                                                      : encodedCapabilities.at("optOutNotificationMethods") == expected,
                                  "notification opt-out methods preserve omitted, null, empty, and populated states");
        }

        const std::string initialized = ProtocolCodec::initializedNotification();
        testResult.expectTrue(!initialized.empty(), "initialized encoding produces a JSON document");
        testResult.expectTrue(initialized.find('\n') == std::string::npos && initialized.find('\r') == std::string::npos,
                              "initialized encoding contains no stdio line delimiter");
        const std::optional<ProtocolMessage> initializedMessage = ProtocolCodec::decode(initialized, errorMessage);
        testResult.expectTrue(initializedMessage && initializedMessage->kind == ProtocolMessage::Kind::Notification &&
                                  initializedMessage->method == "initialized" && initializedMessage->params.is_null(),
                              "initialized wrapper decodes as the parameterless stable notification");
        testResult.expectTrue(!Json::parse(initialized).contains("params"),
                              "initialized wire envelope contains the method and no params member");

        const Json rawInitializeResult = {
            {"codexHome", "/tmp/fake-codex"},
            {"platformFamily", "unix"},
            {"platformOs", "linux"},
            {"userAgent", "snodec-codec-test"},
            {"futureField", {{"nested", Json::array({1, 2, 3})}}},
        };
        errorMessage = "stale";
        const std::optional<InitializeResult> initializeResult =
            ai::openai::codex::detail::decodeInitializeResult(rawInitializeResult, errorMessage);
        const std::optional<InitializeResponse> initializeResponse =
            ai::openai::codex::detail::decodeInitializeResponse(rawInitializeResult, errorMessage);
        testResult.expectTrue(initializeResult && initializeResult->codexHome == "/tmp/fake-codex" &&
                                  initializeResult->platformFamily == "unix" && initializeResult->platformOs == "linux" &&
                                  initializeResult->userAgent == "snodec-codec-test",
                              "separate initialization decoder caches all typed fields");
        testResult.expectTrue(initializeResult && initializeResult->raw == rawInitializeResult,
                              "separate initialization decoder preserves complete raw result and future fields");
        testResult.expectTrue(initializeResponse && initializeResponse->codexHome.value == "/tmp/fake-codex" &&
                                  initializeResponse->platformFamily == "unix" && initializeResponse->platformOs == "linux" &&
                                  initializeResponse->userAgent == "snodec-codec-test" && initializeResponse->raw == rawInitializeResult,
                              "one decoder exposes the strong canonical initialize response and complete raw object");
        testResult.expectTrue(errorMessage.empty(), "successful initialization decoding clears a previous error");

        errorMessage.clear();
        const std::optional<InitializeResult> scalarInitializeResult =
            ai::openai::codex::detail::decodeInitializeResult(Json::array(), errorMessage);
        testResult.expectTrue(!scalarInitializeResult && !errorMessage.empty(),
                              "initialization decoder rejects a non-object result with a reason");

        for (const char* field : {"codexHome", "platformFamily", "platformOs", "userAgent"}) {
            Json missingFieldResult = rawInitializeResult;
            missingFieldResult.erase(field);
            errorMessage.clear();
            const std::optional<InitializeResponse> missingFieldInitializeResult =
                ai::openai::codex::detail::decodeInitializeResponse(missingFieldResult, errorMessage);
            testResult.expectTrue(!missingFieldInitializeResult && !errorMessage.empty(),
                                  std::string("initialization decoder rejects missing required field ") + field);

            Json invalidFieldResult = rawInitializeResult;
            invalidFieldResult[field] = Json::array();
            errorMessage.clear();
            const std::optional<InitializeResponse> invalidFieldInitializeResult =
                ai::openai::codex::detail::decodeInitializeResponse(invalidFieldResult, errorMessage);
            testResult.expectTrue(!invalidFieldInitializeResult && !errorMessage.empty(),
                                  std::string("initialization decoder rejects wrong-typed required field ") + field);
        }
    }
} // namespace

int main() {
    tests::support::TestResult testResult;

    testRequestsAndNotifications(testResult);
    testResultResponses(testResult);
    testRemoteErrors(testResult);
    testMalformedAndAmbiguousMessages(testResult);
    testGenericEncoders(testResult);
    testInitializationCodec(testResult);

    return testResult.processResult();
}
