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

#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

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
        return {std::move(id), std::move(method), std::move(params), std::move(raw), codex::ServerRequestToken{31}};
    }

    codex::Json userInputParams() {
        return {
            {"autoResolutionMs", 2500},
            {"futureParams", {{"retained", true}}},
            {"itemId", "item-sensitive"},
            {"mode", "form"},
            {"questions",
             codex::Json::array({
                 {
                     {"futureQuestion", 1},
                     {"header", "Short header"},
                     {"id", "omitted"},
                     {"question", "private omitted question"},
                 },
                 {
                     {"header", "Nullable options"},
                     {"id", "null"},
                     {"isOther", false},
                     {"isSecret", true},
                     {"options", nullptr},
                     {"question", "private null question"},
                 },
                 {
                     {"header", "Empty options"},
                     {"id", "empty"},
                     {"isOther", true},
                     {"isSecret", false},
                     {"options", codex::Json::array()},
                     {"question", "private empty question"},
                 },
                 {
                     {"header", "Populated options"},
                     {"id", "values"},
                     {"options",
                      codex::Json::array({
                          {
                              {"description", "private option description"},
                              {"futureOption", true},
                              {"label", "Choice"},
                          },
                      })},
                     {"question", "private populated question"},
                 },
             })},
            {"threadId", "thread-sensitive"},
            {"turnId", "turn-sensitive"},
        };
    }

    codex::Json formParams() {
        return {
            {"_meta", {{"opaque", codex::Json::array({"private", 2})}}},
            {"futureOuter", {{"retained", true}}},
            {"message", "private elicitation message"},
            {"mode", "form"},
            {"requestedSchema",
             {
                 {"$schema", "https://json-schema.org/draft/2020-12/schema"},
                 {"properties",
                  {
                      {"boolean",
                       {
                           {"default", nullptr},
                           {"description", "boolean description"},
                           {"title", nullptr},
                           {"type", "boolean"},
                       }},
                      {"legacy",
                       {
                           {"default", "one"},
                           {"description", nullptr},
                           {"enum", codex::Json::array({"one", "two"})},
                           {"enumNames", codex::Json::array({"One", "Two"})},
                           {"title", "Legacy"},
                           {"type", "string"},
                       }},
                      {"number",
                       {
                           {"default", 1.5},
                           {"description", "number description"},
                           {"maximum", 10.5},
                           {"minimum", -2.5},
                           {"title", "Number"},
                           {"type", "integer"},
                       }},
                      {"string",
                       {
                           {"default", "default"},
                           {"description", "string description"},
                           {"format", "date-time"},
                           {"maxLength", 72},
                           {"minLength", 1},
                           {"title", "String"},
                           {"type", "string"},
                       }},
                      {"titledMulti",
                       {
                           {"default", codex::Json::array({"alpha"})},
                           {"description", "titled multi"},
                           {"items",
                            {{"anyOf",
                              codex::Json::array({
                                  {{"const", "alpha"}, {"title", "Alpha"}},
                                  {{"const", "beta"}, {"title", "Beta"}},
                              })}}},
                           {"maxItems", 4},
                           {"minItems", 1},
                           {"title", "Titled multi"},
                           {"type", "array"},
                       }},
                      {"titledSingle",
                       {
                           {"default", nullptr},
                           {"description", "titled single"},
                           {"oneOf",
                            codex::Json::array({
                                {{"const", "left"}, {"title", "Left"}},
                                {{"const", "right"}, {"title", "Right"}},
                            })},
                           {"title", "Titled single"},
                           {"type", "string"},
                       }},
                      {"untitledMulti",
                       {
                           {"default", nullptr},
                           {"description", nullptr},
                           {"items", {{"enum", codex::Json::array({"x", "y"})}, {"type", "string"}}},
                           {"maxItems", nullptr},
                           {"minItems", 0},
                           {"title", nullptr},
                           {"type", "array"},
                       }},
                      {"untitledSingle",
                       {
                           {"default", "red"},
                           {"description", "untitled single"},
                           {"enum", codex::Json::array({"red", "blue"})},
                           {"title", nullptr},
                           {"type", "string"},
                       }},
                  }},
                 {"required", codex::Json::array({"boolean", "string"})},
                 {"type", "object"},
             }},
            {"serverName", "server-sensitive"},
            {"threadId", "thread-sensitive"},
            {"turnId", nullptr},
        };
    }

    void testUserInputCanonicalModel(tests::support::TestResult& result) {
        static_assert(std::variant_size_v<typed::TypedServerRequest> == 11);
        static_assert(std::is_same_v<std::variant_alternative_t<2, typed::TypedServerRequest>, typed::UserInputRequest>);
        static_assert(std::is_same_v<std::variant_alternative_t<8, typed::TypedServerRequest>, typed::AttestationGenerateRequest>);
        static_assert(std::is_same_v<std::variant_alternative_t<9, typed::TypedServerRequest>, typed::DynamicToolCallRequest>);
        static_assert(std::is_same_v<std::variant_alternative_t<10, typed::TypedServerRequest>, typed::McpServerElicitationRequest>);
        static_assert(std::variant_size_v<typed::McpElicitation> == 4);
        static_assert(std::is_same_v<std::variant_alternative_t<0, typed::McpElicitation>, typed::McpElicitationForm>);
        static_assert(std::is_same_v<std::variant_alternative_t<1, typed::McpElicitation>, typed::McpElicitationOpenAiForm>);
        static_assert(std::is_same_v<std::variant_alternative_t<2, typed::McpElicitation>, typed::McpElicitationUrl>);
        static_assert(std::is_same_v<std::variant_alternative_t<3, typed::McpElicitation>, typed::UnknownMcpElicitation>);

        std::string error = "stale";
        const codex::Json params = userInputParams();
        const auto decoded = detail::decodeToolRequestUserInputParams(params, error);
        result.expectTrue(decoded && error.empty() && decoded->autoResolutionMs.hasValue() && *decoded->autoResolutionMs == 2500 &&
                              decoded->questions.size() == 4 && decoded->raw == params,
                          "tool user-input decodes all required fields, numeric auto resolution, and open params");
        if (!decoded || decoded->questions.size() != 4) {
            return;
        }

        const auto& omitted = decoded->questions[0];
        const auto& explicitNull = decoded->questions[1];
        const auto& empty = decoded->questions[2];
        const auto& populated = decoded->questions[3];
        result.expectTrue(!omitted.isOther && !omitted.isSecret && omitted.options.isOmitted() && omitted.raw.at("futureQuestion") == 1,
                          "default-bearing booleans and options preserve omission");
        result.expectTrue(explicitNull.isOther == false && explicitNull.isSecret == true && explicitNull.options.isNull(),
                          "tool user-input preserves explicit false, explicit true, and null options");
        result.expectTrue(empty.isOther == true && empty.isSecret == false && empty.options.hasValue() && empty.options->empty(),
                          "tool user-input distinguishes an empty option array from null and omission");
        result.expectTrue(populated.options.hasValue() && populated.options->size() == 1 &&
                              populated.options->front().description == "private option description" &&
                              populated.options->front().label == "Choice" && populated.options->front().raw.at("futureOption") == true,
                          "tool user-input represents every stable option field and retains open option fields");

        codex::Json omittedAuto = params;
        omittedAuto.erase("autoResolutionMs");
        const auto decodedOmittedAuto = detail::decodeToolRequestUserInputParams(omittedAuto, error);
        codex::Json nullAuto = params;
        nullAuto["autoResolutionMs"] = nullptr;
        const auto decodedNullAuto = detail::decodeToolRequestUserInputParams(nullAuto, error);
        result.expectTrue(decodedOmittedAuto && decodedOmittedAuto->autoResolutionMs.isOmitted() && decodedNullAuto &&
                              decodedNullAuto->autoResolutionMs.isNull(),
                          "autoResolutionMs preserves omitted, explicit-null, and numeric states");

        const typed::TypedServerRequest typedRequest =
            detail::decodeServerRequest(request(codex::ServerRequestId{42}, "item/tool/requestUserInput", params));
        const auto* user = std::get_if<typed::UserInputRequest>(&typedRequest);
        result.expectTrue(user && typedRequest.index() == 2 && user->requestToken.value() == 31 && user->canonicalParams.raw == params &&
                              user->questions.size() == 4 && user->questions[0].allowsFreeText == false && user->questions[1].secret &&
                              user->questions[2].allowsFreeText,
                          "the established UserInputRequest stays at index 2 and projects the complete canonical view");

        codex::Json malformed = params;
        malformed["questions"][0]["options"] = "private wrong value";
        const typed::TypedServerRequest invalid = detail::decodeServerRequest(
            request(codex::ServerRequestId{std::string{"malformed-user"}}, "item/tool/requestUserInput", malformed));
        const auto* unknown = std::get_if<typed::UnknownServerRequest>(&invalid);
        result.expectTrue(unknown && invalid.index() == 4 && unknown->diagnostic &&
                              unknown->diagnostic->kind == typed::DecodeIssueKind::MalformedKnownPayload &&
                              unknown->diagnostic->fieldPath == "$.params" && unknown->params == malformed &&
                              unknown->diagnostic->message.find("private wrong value") == std::string::npos,
                          "malformed user-input remains a malformed-known raw request with a content-safe structural diagnostic");

        codex::Json negativeAuto = params;
        negativeAuto["autoResolutionMs"] = -1;
        result.expectTrue(!detail::decodeToolRequestUserInputParams(negativeAuto, error) &&
                              error.find("$.autoResolutionMs") != std::string::npos,
                          "autoResolutionMs enforces unsigned width and its minimum");
    }

    void testCompleteFormSchema(tests::support::TestResult& result) {
        std::string error = "stale";
        const codex::Json params = formParams();
        const auto decoded = detail::decodeMcpServerElicitationRequestParams(params, error);
        result.expectTrue(decoded && error.empty() && decoded->turnId.isNull() && decoded->elicitation.index() == 0 &&
                              decoded->raw == params,
                          "form elicitation decodes at union index 0 and preserves nullable turn and open outer fields");
        if (!decoded) {
            return;
        }
        const auto* form = std::get_if<typed::McpElicitationForm>(&decoded->elicitation);
        result.expectTrue(form && form->message == "private elicitation message" && form->meta.hasValue() &&
                              form->raw.at("futureOuter").at("retained") == true && form->requestedSchema.schema.hasValue() &&
                              form->requestedSchema.required.hasValue() && form->requestedSchema.required->size() == 2 &&
                              form->requestedSchema.type == typed::McpElicitationObjectType::object() &&
                              form->requestedSchema.properties.size() == 8,
                          "form mode represents every root field, opaque metadata, map value, and open branch field");
        if (!form) {
            return;
        }

        const auto& properties = form->requestedSchema.properties;
        const auto* booleanSchema = std::get_if<typed::McpElicitationBooleanSchema>(&properties.at("boolean"));
        const auto* numberSchema = std::get_if<typed::McpElicitationNumberSchema>(&properties.at("number"));
        const auto* stringSchema = std::get_if<typed::McpElicitationStringSchema>(&properties.at("string"));
        result.expectTrue(booleanSchema && booleanSchema->defaultValue.isNull() && booleanSchema->description.hasValue() &&
                              booleanSchema->title.isNull() && booleanSchema->type == typed::McpElicitationBooleanType::boolean(),
                          "boolean form schemas preserve nullable default, description, title, and exact type");
        result.expectTrue(numberSchema && numberSchema->defaultValue.hasValue() && std::abs(*numberSchema->defaultValue - 1.5) < 0.001 &&
                              numberSchema->minimum.hasValue() && std::abs(*numberSchema->minimum + 2.5) < 0.001 &&
                              numberSchema->maximum.hasValue() && std::abs(*numberSchema->maximum - 10.5) < 0.001 &&
                              numberSchema->type == typed::McpElicitationNumberType::integer(),
                          "number form schemas preserve defaults, bounds, metadata, and number/integer enum values");
        result.expectTrue(stringSchema && stringSchema->defaultValue.hasValue() && stringSchema->format.hasValue() &&
                              stringSchema->format->value == "date-time" && stringSchema->minLength.hasValue() &&
                              *stringSchema->minLength == 1 && stringSchema->maxLength.hasValue() && *stringSchema->maxLength == 72,
                          "string form schemas preserve every stable nullable field, format, and uint32 constraint");

        const auto* titledSingleEnum = std::get_if<typed::McpElicitationEnumSchema>(&properties.at("titledSingle"));
        const auto* legacyEnum = std::get_if<typed::McpElicitationEnumSchema>(&properties.at("legacy"));
        const auto* untitledSingleEnum = std::get_if<typed::McpElicitationEnumSchema>(&properties.at("untitledSingle"));
        const auto* titledMultiEnum = std::get_if<typed::McpElicitationEnumSchema>(&properties.at("titledMulti"));
        const auto* untitledMultiEnum = std::get_if<typed::McpElicitationEnumSchema>(&properties.at("untitledMulti"));
        result.expectTrue(
            titledSingleEnum && std::holds_alternative<typed::McpElicitationTitledSingleSelectEnumSchema>(*titledSingleEnum) &&
                legacyEnum && std::holds_alternative<typed::McpElicitationLegacyTitledEnumSchema>(*legacyEnum) && untitledSingleEnum &&
                std::holds_alternative<typed::McpElicitationUntitledSingleSelectEnumSchema>(*untitledSingleEnum) && titledMultiEnum &&
                std::holds_alternative<typed::McpElicitationTitledMultiSelectEnumSchema>(*titledMultiEnum) && untitledMultiEnum &&
                std::holds_alternative<typed::McpElicitationUntitledMultiSelectEnumSchema>(*untitledMultiEnum),
            "all five enum-schema alternatives decode to their distinct canonical types");
        if (titledMultiEnum && untitledMultiEnum) {
            const auto* titledMulti = std::get_if<typed::McpElicitationTitledMultiSelectEnumSchema>(titledMultiEnum);
            const auto* untitledMulti = std::get_if<typed::McpElicitationUntitledMultiSelectEnumSchema>(untitledMultiEnum);
            result.expectTrue(titledMulti && titledMulti->items.anyOf.size() == 2 && titledMulti->minItems.hasValue() &&
                                  *titledMulti->minItems == 1 && titledMulti->maxItems.hasValue() && *titledMulti->maxItems == 4 &&
                                  untitledMulti && untitledMulti->items.values == std::vector<std::string>({"x", "y"}) &&
                                  untitledMulti->minItems.hasValue() && *untitledMulti->minItems == 0 && untitledMulti->maxItems.isNull(),
                              "titled and untitled multi-select schemas preserve nested items, ordering, defaults, and bounds");
        }

        codex::Json unknownFormat = params;
        unknownFormat["requestedSchema"]["properties"]["string"]["format"] = "future-format";
        const auto decodedUnknownFormat = detail::decodeMcpServerElicitationRequestParams(unknownFormat, error);
        const auto* unknownFormatForm =
            decodedUnknownFormat ? std::get_if<typed::McpElicitationForm>(&decodedUnknownFormat->elicitation) : nullptr;
        const auto* unknownFormatString =
            unknownFormatForm ? std::get_if<typed::McpElicitationStringSchema>(&unknownFormatForm->requestedSchema.properties.at("string"))
                              : nullptr;
        result.expectTrue(unknownFormatString && unknownFormatString->format.hasValue() &&
                              unknownFormatString->format->value == "future-format" && !unknownFormatString->diagnostics.empty() &&
                              unknownFormatString->diagnostics.front().severity == typed::DecodeIssueSeverity::ForwardCompatibility &&
                              unknownFormatString->raw.at("format") == "future-format",
                          "unknown open-enum values remain nonfatal, raw-preserving forward-compatibility diagnostics");

        const std::array<const char*, 12> closedObjectPointers{{
            "/requestedSchema",
            "/requestedSchema/properties/boolean",
            "/requestedSchema/properties/number",
            "/requestedSchema/properties/string",
            "/requestedSchema/properties/legacy",
            "/requestedSchema/properties/titledSingle",
            "/requestedSchema/properties/titledSingle/oneOf/0",
            "/requestedSchema/properties/titledMulti",
            "/requestedSchema/properties/titledMulti/items",
            "/requestedSchema/properties/untitledMulti",
            "/requestedSchema/properties/untitledMulti/items",
            "/requestedSchema/properties/untitledSingle",
        }};
        std::size_t rejectedClosedObjects = 0;
        for (const char* pointer : closedObjectPointers) {
            codex::Json malformed = params;
            malformed[codex::Json::json_pointer(pointer)]["unsupported"] = "private closed value";
            if (!detail::decodeMcpServerElicitationRequestParams(malformed, error) &&
                error.find("unsupported property") != std::string::npos && error.find("private closed value") == std::string::npos) {
                ++rejectedClosedObjects;
            }
        }
        result.expectEqual(closedObjectPointers.size(),
                           rejectedClosedObjects,
                           "all 12 closed form-schema object definitions reject additional properties safely");
    }

    void testElicitationAlternativesAndMalformedKnown(tests::support::TestResult& result) {
        std::string error;
        const codex::Json openAiParams = {
            {"_meta", nullptr},
            {"futureBranch", true},
            {"message", "private OpenAI form message"},
            {"mode", "openai/form"},
            {"requestedSchema", nullptr},
            {"serverName", "server-sensitive"},
            {"threadId", "thread-sensitive"},
        };
        const auto openAi = detail::decodeMcpServerElicitationRequestParams(openAiParams, error);
        const auto* openAiForm = openAi ? std::get_if<typed::McpElicitationOpenAiForm>(&openAi->elicitation) : nullptr;
        result.expectTrue(openAiForm && openAi->elicitation.index() == 1 && openAi->turnId.isOmitted() &&
                              openAiForm->requestedSchema.is_null() && openAiForm->meta.isNull() &&
                              openAiForm->raw.at("futureBranch") == true,
                          "openai/form decodes at index 1 with required nullable opaque schema and optional-null metadata");

        const codex::Json urlParams = {
            {"elicitationId", "elicitation-sensitive"},
            {"futureBranch", {{"retained", true}}},
            {"message", "private URL message"},
            {"mode", "url"},
            {"serverName", "server-sensitive"},
            {"threadId", "thread-sensitive"},
            {"turnId", "turn-sensitive"},
            {"url", "https://example.invalid/private"},
        };
        const auto url = detail::decodeMcpServerElicitationRequestParams(urlParams, error);
        const auto* decodedUrl = url ? std::get_if<typed::McpElicitationUrl>(&url->elicitation) : nullptr;
        result.expectTrue(decodedUrl && url->elicitation.index() == 2 && url->turnId.hasValue() && url->turnId->value == "turn-sensitive" &&
                              decodedUrl->elicitationId == "elicitation-sensitive" &&
                              decodedUrl->url == "https://example.invalid/private" && decodedUrl->meta.isOmitted() &&
                              decodedUrl->raw == urlParams,
                          "url elicitation decodes every stable field at index 2 and preserves open future fields");

        codex::Json future = urlParams;
        future["mode"] = "future/mode";
        const auto unknown = detail::decodeMcpServerElicitationRequestParams(future, error);
        const auto* unknownMode = unknown ? std::get_if<typed::UnknownMcpElicitation>(&unknown->elicitation) : nullptr;
        result.expectTrue(unknownMode && unknown->elicitation.index() == 3 && unknownMode->mode == "future/mode" &&
                              unknownMode->raw == future && unknownMode->diagnostic.kind == typed::DecodeIssueKind::UnknownDiscriminator &&
                              unknownMode->diagnostic.severity == typed::DecodeIssueSeverity::ForwardCompatibility,
                          "a genuinely future mode decodes nonfatally at index 3 with raw preservation");

        const typed::TypedServerRequest futureRequest =
            detail::decodeServerRequest(request(codex::ServerRequestId{std::string{"future-id"}}, "mcpServer/elicitation/request", future));
        result.expectTrue(std::holds_alternative<typed::McpServerElicitationRequest>(futureRequest) && futureRequest.index() == 10,
                          "a future elicitation mode remains a typed request rather than becoming malformed-known");

        codex::Json malformed = formParams();
        malformed.erase("requestedSchema");
        const typed::TypedServerRequest malformedRequest =
            detail::decodeServerRequest(request(codex::ServerRequestId{73}, "mcpServer/elicitation/request", malformed));
        const auto* malformedKnown = std::get_if<typed::UnknownServerRequest>(&malformedRequest);
        result.expectTrue(malformedKnown && malformedRequest.index() == 4 && malformedKnown->diagnostic &&
                              malformedKnown->diagnostic->kind == typed::DecodeIssueKind::MalformedKnownPayload &&
                              malformedKnown->diagnostic->fieldPath == "$.params" && malformedKnown->params == malformed &&
                              malformedKnown->diagnostic->message.find("private elicitation message") == std::string::npos,
                          "a known mode with a missing required field is malformed-known, raw-preserving, and content-safe");

        malformed = formParams();
        malformed["message"] = 17;
        result.expectTrue(!detail::decodeMcpServerElicitationRequestParams(malformed, error) &&
                              error.find("$.message") != std::string::npos &&
                              error.find("private elicitation message") == std::string::npos,
                          "a known mode with a wrong-typed field is rejected without becoming future-unknown");
    }

    void testResponseEncoding(tests::support::TestResult& result) {
        std::string error = "stale";
        typed::ToolRequestUserInputResponse userResponse;
        userResponse.answers.emplace(
            "question-sensitive", typed::ToolRequestUserInputAnswer{{"first", "second"}, {{"futureAnswer", true}, {"answers", "stale"}}});
        userResponse.raw = {{"answers", "stale"}, {"futureResponse", 7}};
        const auto encodedUser = detail::encodeToolRequestUserInputResponse(userResponse, error);
        result.expectTrue(
            encodedUser ==
                    codex::Json{
                        {"answers",
                         {{"question-sensitive", {{"answers", codex::Json::array({"first", "second"})}, {"futureAnswer", true}}}}},
                        {"futureResponse", 7},
                    } &&
                error.empty(),
            "canonical user-input responses preserve answer-map keys, answer ordering, and open response fields");

        typed::McpServerElicitationRequestResponse accept{
            typed::McpServerElicitationAction::accept(),
            typed::OptionalNullable<codex::Json>::withValue({{"field", "private accepted content"}}),
            typed::OptionalNullable<codex::Json>::explicitNull(),
            {{"action", "stale"}, {"futureResponse", true}},
        };
        const auto encodedAccept = detail::encodeMcpServerElicitationRequestResponse(accept, error);
        result.expectTrue(encodedAccept ==
                                  codex::Json{
                                      {"_meta", nullptr},
                                      {"action", "accept"},
                                      {"content", {{"field", "private accepted content"}}},
                                      {"futureResponse", true},
                                  } &&
                              error.empty(),
                          "accept factory encodes exact opaque content and metadata while preserving open response fields");

        typed::McpServerElicitationRequestResponse decline{
            typed::McpServerElicitationAction::decline(),
            typed::OptionalNullable<codex::Json>::omitted(),
            typed::OptionalNullable<codex::Json>::omitted(),
            codex::Json::object(),
        };
        typed::McpServerElicitationRequestResponse cancel{
            typed::McpServerElicitationAction::cancel(),
            typed::OptionalNullable<codex::Json>::omitted(),
            typed::OptionalNullable<codex::Json>::omitted(),
            codex::Json::object(),
        };
        const auto encodedDecline = detail::encodeMcpServerElicitationRequestResponse(decline, error);
        const auto encodedCancel = detail::encodeMcpServerElicitationRequestResponse(cancel, error);
        result.expectTrue(encodedDecline == codex::Json{{"action", "decline"}} && encodedCancel == codex::Json{{"action", "cancel"}} &&
                              !encodedDecline->contains("content") && !encodedCancel->contains("content"),
                          "decline and cancel factories do not fabricate content or metadata");

        accept.action = typed::McpServerElicitationAction{"private future action"};
        result.expectTrue(!detail::encodeMcpServerElicitationRequestResponse(accept, error) &&
                              error.find("accept, decline, or cancel") != std::string::npos &&
                              error.find("private future action") == std::string::npos &&
                              error.find("private accepted content") == std::string::npos,
                          "invalid local elicitation actions fail validation without logging action or content");

        userResponse.raw = nullptr;
        result.expectTrue(!detail::encodeToolRequestUserInputResponse(userResponse, error) &&
                              error.find("raw future fields must be an object") != std::string::npos &&
                              error.find("question-sensitive") == std::string::npos,
                          "malformed local user-input responses fail without logging question IDs or answers");
    }

    void testRegistryOwnership(tests::support::TestResult& result) {
        const auto& userInput = detail::entryFor(detail::ServerRequestTarget::ToolRequestUserInput);
        const auto& elicitation = detail::entryFor(detail::ServerRequestTarget::McpServerElicitation);
        result.expectTrue(userInput.key.name == "item/tool/requestUserInput" && elicitation.key.name == "mcpServer/elicitation/request" &&
                              userInput.typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                              elicitation.typedSchemaStatus == detail::TypedSchemaStatus::Complete,
                          "the two user-input/MCP-elicitation server requests are Complete and bound to their exact lifecycle targets");

        constexpr std::array<detail::IntegrationsAndLongTailUnionTarget, 3> ElicitationTargets{{
            detail::IntegrationsAndLongTailUnionTarget::McpServerElicitationForm,
            detail::IntegrationsAndLongTailUnionTarget::McpServerElicitationOpenAiForm,
            detail::IntegrationsAndLongTailUnionTarget::McpServerElicitationUrl,
        }};
        constexpr std::array<const char*, 3> ElicitationNames{{"form", "openai/form", "url"}};
        const auto descriptors = detail::integrationsAndLongTailUnionCodecDescriptors();
        std::size_t exactRows = 0;
        for (std::size_t index = 0; index < ElicitationTargets.size(); ++index) {
            const auto& entry = detail::entryFor(ElicitationTargets[index]);
            if (entry.key.domain == "McpServerElicitationRequestParams" && entry.key.field == "mode" &&
                entry.key.name == ElicitationNames[index] && entry.typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                descriptors[4 + index].target == ElicitationTargets[index]) {
                ++exactRows;
            }
        }
        result.expectTrue(descriptors.size() == 7 && exactRows == 3,
                          "form/openai-form/url belong only to McpServerElicitationRequestParams and append in exact order");
    }
} // namespace

int main() {
    tests::support::TestResult result;
    testUserInputCanonicalModel(result);
    testCompleteFormSchema(result);
    testElicitationAlternativesAndMalformedKnown(result);
    testResponseEncoding(result);
    testRegistryOwnership(result);
    return result.processResult();
}
