/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/McpReverseRequestCodec.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "ai/openai/codex/detail/ServerRequestDecoder.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "support/TestResult.h"

#include <cstdint>
#include <string>
#include <type_traits>
#include <variant>

namespace {
    namespace codex = ai::openai::codex;
    namespace detail = ai::openai::codex::detail;
    namespace typed = ai::openai::codex::typed;

    codex::ServerRequest request(codex::ServerRequestId id, std::string method, codex::Json params) {
        codex::Json encodedId = std::visit(
            [](const auto& value) -> codex::Json {
                return value;
            },
            id.value());
        codex::Json raw = {
            {"id", std::move(encodedId)},
            {"method", method},
            {"params", params},
            {"futureEnvelope", true},
        };
        return {std::move(id), std::move(method), std::move(params), std::move(raw), codex::ServerRequestToken{7}};
    }

    void testRequestDecoding(tests::support::TestResult& result) {
        static_assert(std::variant_size_v<typed::TypedServerRequest> == 11);
        static_assert(std::is_same_v<std::variant_alternative_t<2, typed::TypedServerRequest>, typed::UserInputRequest>);
        static_assert(std::is_same_v<std::variant_alternative_t<4, typed::TypedServerRequest>, typed::UnknownServerRequest>);
        static_assert(std::is_same_v<std::variant_alternative_t<8, typed::TypedServerRequest>, typed::AttestationGenerateRequest>);
        static_assert(std::is_same_v<std::variant_alternative_t<9, typed::TypedServerRequest>, typed::DynamicToolCallRequest>);
        static_assert(std::is_same_v<std::variant_alternative_t<10, typed::TypedServerRequest>, typed::McpServerElicitationRequest>);

        const codex::Json attestationParams = {{"futureAttestationField", {{"kept", true}}}};
        const typed::TypedServerRequest attestation = detail::decodeServerRequest(
            request(codex::ServerRequestId{std::string{"attestation-id"}}, "attestation/generate", attestationParams));
        const auto* decodedAttestation = std::get_if<typed::AttestationGenerateRequest>(&attestation);
        result.expectTrue(decodedAttestation && attestation.index() == 8 && decodedAttestation->requestToken.value() == 7 &&
                              decodedAttestation->params.raw == attestationParams && decodedAttestation->raw.at("futureEnvelope") == true,
                          "attestation/generate decodes at append index 8 and preserves its open params and raw envelope");

        const codex::Json dynamicParams = {
            {"arguments", nullptr},
            {"callId", "call-sensitive"},
            {"namespace", nullptr},
            {"threadId", "thread-sensitive"},
            {"tool", "tool-sensitive"},
            {"turnId", "turn-sensitive"},
            {"futureDynamicField", codex::Json::array({1, true})},
        };
        const typed::TypedServerRequest dynamic =
            detail::decodeServerRequest(request(codex::ServerRequestId{91}, "item/tool/call", dynamicParams));
        const auto* decodedDynamic = std::get_if<typed::DynamicToolCallRequest>(&dynamic);
        result.expectTrue(decodedDynamic && dynamic.index() == 9 && decodedDynamic->params.arguments.is_null() &&
                              decodedDynamic->params.callId.value == "call-sensitive" && decodedDynamic->params.nameSpace.isNull() &&
                              decodedDynamic->params.threadId.value == "thread-sensitive" &&
                              decodedDynamic->params.tool == "tool-sensitive" && decodedDynamic->params.turnId.value == "turn-sensitive" &&
                              decodedDynamic->params.raw == dynamicParams,
                          "item/tool/call decodes every stable field, including required null arguments and explicit-null namespace");

        codex::Json omittedNamespace = dynamicParams;
        omittedNamespace.erase("namespace");
        omittedNamespace["arguments"] = {{"opaque", codex::Json::array({1, 2})}};
        const typed::TypedServerRequest omitted =
            detail::decodeServerRequest(request(codex::ServerRequestId{92}, "item/tool/call", omittedNamespace));
        const auto* decodedOmitted = std::get_if<typed::DynamicToolCallRequest>(&omitted);
        result.expectTrue(decodedOmitted && decodedOmitted->params.nameSpace.isOmitted() &&
                              decodedOmitted->params.arguments == omittedNamespace.at("arguments"),
                          "dynamic-tool decoding distinguishes omitted namespace and preserves arbitrary argument JSON");

        codex::Json presentNamespace = omittedNamespace;
        presentNamespace["namespace"] = "synthetic-namespace";
        const typed::TypedServerRequest present =
            detail::decodeServerRequest(request(codex::ServerRequestId{93}, "item/tool/call", presentNamespace));
        const auto* decodedPresent = std::get_if<typed::DynamicToolCallRequest>(&present);
        result.expectTrue(decodedPresent && decodedPresent->params.nameSpace.hasValue() &&
                              *decodedPresent->params.nameSpace == "synthetic-namespace",
                          "dynamic-tool decoding preserves a present namespace value");

        codex::Json malformed = dynamicParams;
        malformed["arguments"] = {{"secret", "must-not-appear-in-diagnostics"}};
        malformed["tool"] = 3;
        const typed::TypedServerRequest invalid =
            detail::decodeServerRequest(request(codex::ServerRequestId{94}, "item/tool/call", malformed));
        const auto* unknown = std::get_if<typed::UnknownServerRequest>(&invalid);
        result.expectTrue(unknown && unknown->decodingError && unknown->decodingError->find("$.tool") != std::string::npos &&
                              unknown->decodingError->find("must-not-appear") == std::string::npos && unknown->params == malformed,
                          "malformed dynamic-tool fields produce a safe structural diagnostic and preserve the raw params");
    }

    void testResponseEncoding(tests::support::TestResult& result) {
        std::string error = "stale";
        typed::AttestationGenerateResponse attestation;
        attestation.token = "opaque-sensitive-token";
        attestation.raw = {{"futureResponseField", true}, {"token", "stale"}};
        const auto encodedAttestation = detail::encodeAttestationGenerateResponse(attestation, error);
        result.expectTrue(encodedAttestation == codex::Json{{"futureResponseField", true}, {"token", "opaque-sensitive-token"}} &&
                              error.empty(),
                          "attestation response encoding preserves future fields and writes the required opaque token");

        typed::DynamicToolCallResponse dynamic;
        dynamic.contentItems = {
            typed::InputTextDynamicToolCallOutputContentItem{"text-sensitive", {{"futureTextField", 1}, {"type", "stale"}}, {}},
            typed::InputImageDynamicToolCallOutputContentItem{"https://example.invalid/image", {{"futureImageField", 2}}, {}},
        };
        dynamic.success = true;
        dynamic.raw = {{"futureResponseField", true}, {"success", false}};
        const auto encodedDynamic = detail::encodeDynamicToolCallResponse(dynamic, error);
        result.expectTrue(encodedDynamic ==
                                  codex::Json{
                                      {"contentItems",
                                       codex::Json::array({
                                           {{"futureTextField", 1}, {"text", "text-sensitive"}, {"type", "inputText"}},
                                           {{"futureImageField", 2}, {"imageUrl", "https://example.invalid/image"}, {"type", "inputImage"}},
                                       })},
                                      {"futureResponseField", true},
                                      {"success", true},
                                  } &&
                              error.empty(),
                          "dynamic-tool response encoding preserves item order, both known alternatives, and all open fields");

        dynamic.contentItems = {
            typed::UnknownDynamicToolCallOutputContentItem{"future", {{"type", "future"}, {"content", "sensitive"}}, std::nullopt}};
        result.expectTrue(!detail::encodeDynamicToolCallResponse(dynamic, error) &&
                              error.find("unknown future content-item") != std::string::npos &&
                              error.find("sensitive") == std::string::npos,
                          "outgoing dynamic-tool responses reject future-unknown alternatives without exposing content");

        attestation.raw = nullptr;
        result.expectTrue(!detail::encodeAttestationGenerateResponse(attestation, error) &&
                              error.find("raw future fields must be an object") != std::string::npos &&
                              error.find("opaque-sensitive-token") == std::string::npos,
                          "malformed local attestation responses are rejected without exposing attestation material");
    }

    void testRegistry(tests::support::TestResult& result) {
        const detail::ProtocolSurfaceEntry& attestation = detail::entryFor(detail::ServerRequestTarget::AttestationGenerate);
        const detail::ProtocolSurfaceEntry& dynamic = detail::entryFor(detail::ServerRequestTarget::DynamicToolCall);
        result.expectTrue(attestation.key.name == "attestation/generate" && dynamic.key.name == "item/tool/call" &&
                              attestation.typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                              dynamic.typedSchemaStatus == detail::TypedSchemaStatus::Complete,
                          "only the two attestation/dynamic-tool server-request registry targets are Complete");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testRequestDecoding(result);
    testResponseEncoding(result);
    testRegistry(result);
    return result.processResult();
}
