/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/detail/GeneratedSchemaValidator.h"
#include "support/TestResult.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace validator = ai::openai::codex::frontend::detail;

    using Validation = validator::GeneratedSchemaValidation;
    using Limits = validator::SchemaValidationLimits;
    using Statistics = validator::SchemaValidationStatistics;

    struct CorpusStatistics {
        std::size_t validations = 0;
        std::size_t maximumVisits = 0;
        std::size_t maximumDepth = 0;
        std::size_t maximumReferences = 0;
        std::size_t maximumAlternatives = 0;
        std::size_t maximumDiscriminatorFastPaths = 0;
        std::size_t maximumUniqueComparisons = 0;
        std::size_t maximumRegularExpressions = 0;

        void observe(const Statistics& statistics) {
            ++validations;
            maximumVisits = std::max(maximumVisits, statistics.visits);
            maximumDepth = std::max(maximumDepth, statistics.maximumDepthObserved);
            maximumReferences = std::max(maximumReferences, statistics.referencesResolved);
            maximumAlternatives = std::max(maximumAlternatives, statistics.alternativesEvaluated);
            maximumDiscriminatorFastPaths = std::max(maximumDiscriminatorFastPaths, statistics.discriminatorFastPaths);
            maximumUniqueComparisons = std::max(maximumUniqueComparisons, statistics.uniqueItemComparisons);
            maximumRegularExpressions = std::max(maximumRegularExpressions, statistics.regularExpressionsEvaluated);
        }
    };

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

    Validation validateNode(const frontend::Json& root,
                            const frontend::Json& schema,
                            const frontend::Json& value,
                            Limits limits,
                            Statistics* statistics = nullptr) {
        return validator::validateGeneratedSchemaNodeForTest(root, schema, value, "value", limits, statistics);
    }

    Validation validateNode(const frontend::Json& schema, const frontend::Json& value, Statistics* statistics = nullptr) {
        static const frontend::Json EmptyRoot = frontend::Json::object();
        return validateNode(EmptyRoot, schema, value, Limits{}, statistics);
    }

    std::pair<frontend::Json, frontend::Json> nestedArray(std::size_t depth) {
        frontend::Json schema{{"type", "integer"}};
        frontend::Json value = 1;
        for (std::size_t index = 0; index < depth; ++index) {
            schema = frontend::Json{{"type", "array"}, {"items", std::move(schema)}};
            value = frontend::Json::array({std::move(value)});
        }
        return {std::move(schema), std::move(value)};
    }

    std::pair<frontend::Json, frontend::Json> recursiveArray(std::size_t depth) {
        frontend::Json root{
            {"anyOf",
             frontend::Json::array(
                 {frontend::Json{{"type", "integer"}},
                  frontend::Json{{"type", "array"}, {"minItems", 1}, {"maxItems", 1}, {"items", frontend::Json{{"$ref", "#"}}}}})},
        };
        frontend::Json value = 1;
        for (std::size_t index = 0; index < depth; ++index) {
            value = frontend::Json::array({std::move(value)});
        }
        return {std::move(root), std::move(value)};
    }

    frontend::Json discriminatedSchema(std::string_view property, std::size_t alternatives, bool reverse) {
        frontend::Json branches = frontend::Json::array();
        for (std::size_t index = 0; index < alternatives; ++index) {
            const std::size_t value = reverse ? alternatives - index - 1 : index;
            branches.push_back(frontend::Json{
                {"type", "object"},
                {"required", frontend::Json::array({std::string(property), "payload"})},
                {"properties",
                 {{std::string(property), {{"const", "alternative-" + std::to_string(value)}}}, {"payload", {{"type", "integer"}}}}},
                {"additionalProperties", false},
            });
        }
        return frontend::Json{{"oneOf", std::move(branches)}};
    }

    frontend::Json uniqueIntegers(std::size_t count) {
        frontend::Json values = frontend::Json::array();
        for (std::size_t index = 0; index < count; ++index) {
            values.push_back(index);
        }
        return values;
    }

    frontend::Json nestedArrayValue(std::size_t depth) {
        frontend::Json value = "safe";
        for (std::size_t index = 0; index < depth; ++index) {
            value = frontend::Json::array({std::move(value)});
        }
        return value;
    }

    std::size_t countGeneratedPatterns(const frontend::Json& schema) {
        if (schema.is_array()) {
            std::size_t count = 0;
            for (const frontend::Json& value : schema) {
                count += countGeneratedPatterns(value);
            }
            return count;
        }
        if (!schema.is_object()) {
            return 0;
        }
        std::size_t count = schema.contains("pattern") && schema.at("pattern").is_string() ? 1 : 0;
        for (const auto& [name, value] : schema.items()) {
            (void) name;
            count += countGeneratedPatterns(value);
        }
        return count;
    }

    void testDeterministicComplexityBoundaries(tests::support::TestResult& result) {
        result.expectTrue(Limits{}.maximumDepth == 128 && Limits{}.maximumVisits == 4'000'000,
                          "the public codec and private seam share fixed 128-depth/four-million-visit defaults");
        auto [depthSchema, depthValue] = recursiveArray(3);
        Statistics measuredDepthStats;
        const Validation measuredDepth = validateNode(depthSchema, depthSchema, depthValue, Limits{128, 1'000}, &measuredDepthStats);
        Statistics atDepthStats;
        const Validation atDepth =
            validateNode(depthSchema, depthSchema, depthValue, Limits{measuredDepthStats.maximumDepthObserved, 1'000}, &atDepthStats);
        Statistics belowDepthStats;
        const Validation belowDepth = validateNode(
            depthSchema, depthSchema, depthValue, Limits{measuredDepthStats.maximumDepthObserved - 1, 1'000}, &belowDepthStats);
        result.expectTrue(measuredDepth.valid && atDepth.valid &&
                              atDepthStats.maximumDepthObserved == measuredDepthStats.maximumDepthObserved &&
                              !atDepthStats.complexityRejected,
                          "a small recursive schema accepts a value at its exact configured depth boundary");
        result.expectTrue(!belowDepth.valid && !belowDepth.internalFailure && belowDepthStats.complexityRejected,
                          "the recursive schema rejects the same value one level beyond the configured depth boundary");

        const frontend::Json visitSchema{{"type", "array"}, {"items", {{"type", "integer"}}}};
        const frontend::Json visitValue = frontend::Json::array({1, 2, 3});
        Statistics atVisitStats;
        const Validation atVisits = validateNode(frontend::Json::object(), visitSchema, visitValue, Limits{32, 4}, &atVisitStats);
        Statistics belowVisitStats;
        const Validation belowVisits = validateNode(frontend::Json::object(), visitSchema, visitValue, Limits{32, 3}, &belowVisitStats);
        result.expectTrue(atVisits.valid && atVisitStats.visits == 4 && !atVisitStats.complexityRejected,
                          "schema validation accepts the exact four-visit boundary");
        result.expectTrue(!belowVisits.valid && !belowVisits.internalFailure && belowVisitStats.complexityRejected,
                          "schema validation rejects the first visit beyond its deterministic budget");

        auto [productionDepthSchema, productionDepthValue] = nestedArray(Limits{}.maximumDepth);
        Statistics productionDepthStats;
        const Validation productionDepth =
            validateNode(frontend::Json::object(), productionDepthSchema, productionDepthValue, Limits{}, &productionDepthStats);
        auto [excessiveDepthSchema, excessiveDepthValue] = nestedArray(Limits{}.maximumDepth + 1);
        Statistics excessiveDepthStats;
        const Validation excessiveDepth =
            validateNode(frontend::Json::object(), excessiveDepthSchema, excessiveDepthValue, Limits{}, &excessiveDepthStats);
        const auto depthRecoveredCommand = frontend::Codec::decodeDefinedCommand(frontend::Json{
            {"protocol", frontend::ProtocolIdentity},
            {"version", frontend::ProtocolVersion},
            {"kind", "command"},
            {"requestId", "depth-recovered"},
            {"method", "snapshot.get"},
            {"params", frontend::Json::object()},
        });
        result.expectTrue(productionDepth.valid && productionDepthStats.maximumDepthObserved == 128,
                          "the production default accepts a synthetic value at exact depth 128");
        result.expectTrue(!excessiveDepth.valid && excessiveDepthStats.complexityRejected &&
                              excessiveDepthStats.maximumDepthObserved == 129 && depthRecoveredCommand.hasValue(),
                          "the production default rejects exact depth 129 and permits immediate public-codec recovery");

        const std::string visitSecret = "A17A_VISIT_LIMIT_SECRET";
        const frontend::Json secretValue = frontend::Json::array({"one", "two", visitSecret});
        Statistics secretStats;
        const Validation secretLimited = validateNode(frontend::Json::object(),
                                                      frontend::Json{{"type", "array"}, {"items", {{"type", "string"}}}},
                                                      secretValue,
                                                      Limits{32, 3},
                                                      &secretStats);
        const auto recoveredCommand = frontend::Codec::decodeDefinedCommand(frontend::Json{
            {"protocol", frontend::ProtocolIdentity},
            {"version", frontend::ProtocolVersion},
            {"kind", "command"},
            {"requestId", "recovered"},
            {"method", "snapshot.get"},
            {"params", frontend::Json::object()},
        });
        result.expectTrue(!secretLimited.valid && secretStats.visits == 3 && secretStats.complexityRejected &&
                              secretLimited.message.find(visitSecret) == std::string::npos && recoveredCommand.hasValue(),
                          "visit exhaustion stops exactly at its limit, leaks no value, and permits immediate public-codec reuse");
    }

    void testDiscriminatorsAndBranchOrder(tests::support::TestResult& result) {
        for (const std::string_view discriminator : {std::string_view{"method"}, std::string_view{"type"}, std::string_view{"kind"}}) {
            const frontend::Json schema = discriminatedSchema(discriminator, 8, false);
            const frontend::Json value{{std::string(discriminator), "alternative-7"}, {"payload", 7}};
            Statistics statistics;
            const Validation validation = validateNode(schema, value, &statistics);
            frontend::Json unknown = value;
            unknown[std::string(discriminator)] = "unknown";
            Statistics unknownStatistics;
            const Validation rejected = validateNode(schema, unknown, &unknownStatistics);
            result.expectTrue(validation.valid && statistics.discriminatorFastPaths == 1 && statistics.alternativesEvaluated == 1,
                              std::string(discriminator) + " uses one exact discriminator branch");
            result.expectTrue(!rejected.valid && !rejected.missingRequired && !rejected.internalFailure &&
                                  unknownStatistics.discriminatorFastPaths == 1 && unknownStatistics.alternativesEvaluated == 0,
                              std::string(discriminator) + " rejects an unknown discriminator without branch ambiguity");
        }

        constexpr std::size_t AdversarialBranches = 128;
        const frontend::Json value{{"type", "alternative-127"}, {"payload", 127}};
        Statistics forwardStatistics;
        Statistics reverseStatistics;
        const Validation forward = validateNode(discriminatedSchema("type", AdversarialBranches, false), value, &forwardStatistics);
        const Validation reverse = validateNode(discriminatedSchema("type", AdversarialBranches, true), value, &reverseStatistics);
        result.expectTrue(forward.valid && reverse.valid && forwardStatistics.alternativesEvaluated == 1 &&
                              reverseStatistics.alternativesEvaluated == 1 && forwardStatistics.visits == reverseStatistics.visits,
                          "a 128-branch discriminator adversary is bounded and independent of branch order");

        const frontend::Json nondiscriminatedForward{
            {"oneOf", frontend::Json::array({frontend::Json{{"type", "string"}}, frontend::Json{{"type", "integer"}}})}};
        const frontend::Json nondiscriminatedReverse{
            {"oneOf", frontend::Json::array({frontend::Json{{"type", "integer"}}, frontend::Json{{"type", "string"}}})}};
        const Validation firstOrder = validateNode(nondiscriminatedForward, 17);
        const Validation secondOrder = validateNode(nondiscriminatedReverse, 17);
        result.expectTrue(firstOrder.valid && secondOrder.valid,
                          "non-discriminated exclusive alternatives retain order-independent semantics");

        const frontend::Json& root = validator::generatedProtocolSchema();
        Statistics commandStatistics;
        const Validation command = validator::validateGeneratedSchema(root,
                                                                      "#/$defs/Command",
                                                                      frontend::Json{{"protocol", frontend::ProtocolIdentity},
                                                                                     {"version", frontend::ProtocolVersion},
                                                                                     {"kind", "command"},
                                                                                     {"requestId", "request-1"},
                                                                                     {"method", "snapshot.get"},
                                                                                     {"params", frontend::Json::object()}},
                                                                      "command",
                                                                      Limits{},
                                                                      &commandStatistics);
        result.expectTrue(command.valid && commandStatistics.discriminatorFastPaths >= 1 && commandStatistics.alternativesEvaluated <= 1,
                          "the real 105-method command union uses its production discriminator fast path");

        Statistics envelopeStatistics;
        const Validation envelope = validator::validateGeneratedSchema(root,
                                                                       "#",
                                                                       frontend::Json{{"protocol", frontend::ProtocolIdentity},
                                                                                      {"version", frontend::ProtocolVersion},
                                                                                      {"kind", "command"},
                                                                                      {"requestId", "request-2"},
                                                                                      {"method", "snapshot.get"},
                                                                                      {"params", frontend::Json::object()}},
                                                                       "envelope",
                                                                       Limits{},
                                                                       &envelopeStatistics);
        const frontend::Json& fixtures = goldenFixtures();
        Statistics itemStatistics;
        const Validation item = validator::validateGeneratedSchema(root,
                                                                   "#/$defs/ExpandedThreadItem",
                                                                   fixtures.at("expandedSnapshot").at("state").at("items").front(),
                                                                   "item",
                                                                   Limits{},
                                                                   &itemStatistics);
        Statistics eventStatistics;
        const Validation event = validator::validateGeneratedSchema(
            root, "#/$defs/ExpandedFrontendEvent", fixtures.at("expandedEvents").front(), "event", Limits{}, &eventStatistics);
        result.expectTrue(envelope.valid && item.valid && event.valid && envelopeStatistics.discriminatorFastPaths >= 2 &&
                              itemStatistics.discriminatorFastPaths >= 1 && eventStatistics.discriminatorFastPaths >= 1,
                          "actual generated kind, method, ThreadItem type, and frontend-event type unions use discriminator fast paths");

        frontend::Json unknownMethod{{"protocol", frontend::ProtocolIdentity},
                                     {"version", frontend::ProtocolVersion},
                                     {"kind", "command"},
                                     {"requestId", "request-unknown-method"},
                                     {"method", "future.unknown"},
                                     {"params", frontend::Json::object()}};
        Statistics unknownMethodStatistics;
        const Validation unknownMethodValidation =
            validator::validateGeneratedSchema(root, "#/$defs/Command", unknownMethod, "command", Limits{}, &unknownMethodStatistics);
        frontend::Json unknownKind = unknownMethod;
        unknownKind["kind"] = "future-kind";
        Statistics unknownKindStatistics;
        const Validation unknownKindValidation =
            validator::validateGeneratedSchema(root, "#", unknownKind, "message", Limits{}, &unknownKindStatistics);
        frontend::Json unknownItem = fixtures.at("expandedSnapshot").at("state").at("items").front();
        unknownItem["type"] = "future-item";
        Statistics unknownTypeStatistics;
        const Validation unknownTypeValidation =
            validator::validateGeneratedSchema(root, "#/$defs/ExpandedThreadItem", unknownItem, "item", Limits{}, &unknownTypeStatistics);
        result.expectTrue(!unknownMethodValidation.valid && !unknownKindValidation.valid && !unknownTypeValidation.valid &&
                              unknownMethodStatistics.discriminatorFastPaths >= 1 && unknownKindStatistics.discriminatorFastPaths >= 1 &&
                              unknownTypeStatistics.discriminatorFastPaths >= 1 && unknownMethodStatistics.alternativesEvaluated == 0 &&
                              unknownKindStatistics.alternativesEvaluated == 0 && unknownTypeStatistics.alternativesEvaluated == 0,
                          "actual generated method, kind, and type unions reject unknown discriminators without payload traversal");

        frontend::Json invalidExpandedItemEvent = fixtures.at("expandedEvents").at(7);
        invalidExpandedItemEvent["data"]["item"]["type"] = "user_message";
        invalidExpandedItemEvent["data"]["item"]["credential"] = "NESTED_SECRET_MUST_NOT_APPEAR";
        const Validation exactItemDiagnostic = validator::validateGeneratedSchema(
            root, "#/$defs/ExpandedFrontendEvent", invalidExpandedItemEvent, "expanded event", Limits{}, nullptr);
        result.expectTrue(!exactItemDiagnostic.valid &&
                              exactItemDiagnostic.message ==
                                  "expanded event.data.item.type value 'user_message' is not a schema-defined discriminator" &&
                              exactItemDiagnostic.message.find("NESTED_SECRET_MUST_NOT_APPEAR") == std::string::npos,
                          "expanded item diagnostics include only the exact path and bounded rejected string discriminator");

        invalidExpandedItemEvent["data"]["item"]["type"] = "bad'\n\\type";
        const Validation escapedItemDiagnostic = validator::validateGeneratedSchema(
            root, "#/$defs/ExpandedFrontendEvent", invalidExpandedItemEvent, "expanded event", Limits{}, nullptr);
        result.expectTrue(escapedItemDiagnostic.message ==
                              "expanded event.data.item.type value 'bad\\'\\n\\\\type' is not a schema-defined discriminator",
                          "discriminator diagnostics deterministically escape quotes, controls, and backslashes");

        std::string longUtf8Discriminator;
        std::string boundedUtf8Prefix;
        for (std::size_t index = 0; index < 80; ++index) {
            longUtf8Discriminator += "\xC3\xA9";
            if (index < 62) {
                boundedUtf8Prefix += "\xC3\xA9";
            }
        }
        longUtf8Discriminator += "TRUNCATED_SECRET";
        invalidExpandedItemEvent["data"]["item"]["type"] = longUtf8Discriminator;
        const Validation boundedUtf8Diagnostic = validator::validateGeneratedSchema(
            root, "#/$defs/ExpandedFrontendEvent", invalidExpandedItemEvent, "expanded event", Limits{}, nullptr);
        const std::string expectedBoundedUtf8 =
            "expanded event.data.item.type value '" + boundedUtf8Prefix + "...' is not a schema-defined discriminator";
        result.expectTrue(boundedUtf8Diagnostic.message == expectedBoundedUtf8 &&
                              boundedUtf8Diagnostic.message.find("TRUNCATED_SECRET") == std::string::npos,
                          "long discriminator diagnostics retain a valid UTF-8 prefix within the 128-byte value bound");

        std::string invalidUtf8 = "ok";
        invalidUtf8.push_back(static_cast<char>(0xC3));
        invalidUtf8 += "INVALID_UTF8_SECRET";
        invalidExpandedItemEvent["data"]["item"]["type"] = invalidUtf8;
        const Validation invalidUtf8Diagnostic = validator::validateGeneratedSchema(
            root, "#/$defs/ExpandedFrontendEvent", invalidExpandedItemEvent, "expanded event", Limits{}, nullptr);
        result.expectTrue(invalidUtf8Diagnostic.message ==
                                  "expanded event.data.item.type value 'ok...' is not a schema-defined discriminator" &&
                              invalidUtf8Diagnostic.message.find("INVALID_UTF8_SECRET") == std::string::npos,
                          "invalid UTF-8 discriminator tails are contained without leaking subsequent bytes");

        invalidExpandedItemEvent["data"]["item"]["type"] = frontend::Json{{"secret", "OBJECT_SECRET_MUST_NOT_APPEAR"}};
        const Validation nonStringDiagnostic = validator::validateGeneratedSchema(
            root, "#/$defs/ExpandedFrontendEvent", invalidExpandedItemEvent, "expanded event", Limits{}, nullptr);
        result.expectTrue(nonStringDiagnostic.message ==
                                  "expanded event.data.item.type value has JSON type 'object' is not a schema-defined discriminator" &&
                              nonStringDiagnostic.message.find("OBJECT_SECRET_MUST_NOT_APPEAR") == std::string::npos,
                          "non-string discriminator diagnostics report only the JSON type and exact path");

        frontend::Json duplicateGeneratedRoot = root;
        frontend::Json& generatedCommandBranches = duplicateGeneratedRoot["$defs"]["Command"]["allOf"][1]["oneOf"];
        generatedCommandBranches[1]["properties"]["method"]["const"] = generatedCommandBranches[0]["properties"]["method"]["const"];
        frontend::Json duplicateGeneratedCommand{{"protocol", frontend::ProtocolIdentity},
                                                 {"version", frontend::ProtocolVersion},
                                                 {"kind", "command"},
                                                 {"requestId", "request-duplicate-method"},
                                                 {"method", generatedCommandBranches[0]["properties"]["method"]["const"]},
                                                 {"params", frontend::Json::object()}};
        const Validation duplicateGeneratedDiscriminator = validator::validateGeneratedSchema(
            duplicateGeneratedRoot, "#/$defs/Command", duplicateGeneratedCommand, "command", Limits{}, nullptr);
        result.expectTrue(!duplicateGeneratedDiscriminator.valid && duplicateGeneratedDiscriminator.internalFailure,
                          "a duplicate constant in the actual generated method union is an internal schema failure");

        frontend::Json duplicateBranches = discriminatedSchema("type", 2, false).at("oneOf");
        duplicateBranches[1]["properties"]["type"]["const"] = "alternative-0";
        const Validation duplicateDiscriminator =
            validateNode(frontend::Json{{"oneOf", duplicateBranches}}, frontend::Json{{"type", "alternative-0"}, {"payload", 0}});
        result.expectTrue(!duplicateDiscriminator.valid && duplicateDiscriminator.internalFailure,
                          "duplicate generated discriminator values are contained as an internal schema defect");

        frontend::Json forwardBranches = frontend::Json::array();
        frontend::Json reverseBranches = frontend::Json::array();
        for (std::size_t index = 0; index < 64; ++index) {
            forwardBranches.push_back(frontend::Json{{"const", index}});
            reverseBranches.insert(reverseBranches.begin(), frontend::Json{{"const", index}});
        }
        for (const std::string_view keyword : {std::string_view{"anyOf"}, std::string_view{"oneOf"}}) {
            const frontend::Json forwardSchema{{std::string(keyword), forwardBranches}};
            const frontend::Json reverseSchema{{std::string(keyword), reverseBranches}};
            Statistics acceptedForwardStats;
            Statistics acceptedReverseStats;
            Statistics rejectedForwardStats;
            Statistics rejectedReverseStats;
            const Validation acceptedForward =
                validateNode(frontend::Json::object(), forwardSchema, 63, Limits{32, 65}, &acceptedForwardStats);
            const Validation acceptedReverse =
                validateNode(frontend::Json::object(), reverseSchema, 63, Limits{32, 65}, &acceptedReverseStats);
            const Validation rejectedForward =
                validateNode(frontend::Json::object(), forwardSchema, 64, Limits{32, 65}, &rejectedForwardStats);
            const Validation rejectedReverse =
                validateNode(frontend::Json::object(), reverseSchema, 64, Limits{32, 65}, &rejectedReverseStats);
            Statistics limitedStats;
            const Validation limited = validateNode(frontend::Json::object(), forwardSchema, 64, Limits{32, 16}, &limitedStats);
            const Validation reused = validateNode(forwardSchema, 0);
            result.expectTrue(acceptedForward.valid && acceptedReverse.valid && !rejectedForward.valid && !rejectedReverse.valid &&
                                  !rejectedForwardStats.complexityRejected && !rejectedReverseStats.complexityRejected &&
                                  acceptedForwardStats.visits <= 65 && acceptedReverseStats.visits <= 65,
                              std::string(keyword) + " remains bounded and semantically order-independent across 64 branches");
            result.expectTrue(!limited.valid && limitedStats.complexityRejected && limitedStats.visits == 16 && reused.valid,
                              std::string(keyword) + " stops exactly at an injected adversarial branch limit and remains reusable");
        }
    }

    void testUniqueItems(tests::support::TestResult& result) {
        const frontend::Json& root = validator::generatedProtocolSchema();
        constexpr std::size_t Count105 = 105;
        constexpr std::size_t Comparisons105 = Count105 * (Count105 - 1) / 2;
        frontend::Json methods = frontend::Json::array();
        for (const generated::MethodMetadata& method : generated::AllMethods) {
            methods.push_back(method.method);
        }
        Statistics stats105;
        const Validation valid105 = validator::validateGeneratedSchema(
            root, "#/$defs/Welcome/allOf/1/properties/availableMethods", methods, "methods", Limits{}, &stats105);
        methods[104] = methods[103];
        Statistics duplicate105Stats;
        const Validation duplicate105 = validator::validateGeneratedSchema(
            root, "#/$defs/Welcome/allOf/1/properties/availableMethods", methods, "methods", Limits{}, &duplicate105Stats);
        result.expectTrue(valid105.valid && stats105.uniqueItemComparisons == Comparisons105,
                          "all 105 generated FrontendMethod strings pass the actual unique-method schema in 5,460 comparisons");
        result.expectTrue(!duplicate105.valid && duplicate105Stats.uniqueItemComparisons == Comparisons105,
                          "a duplicate at generated method indices 103/104 fails on the final 5,460th comparison");

        constexpr std::size_t Count64 = 64;
        constexpr std::size_t Comparisons64 = Count64 * (Count64 - 1) / 2;
        frontend::Json omittedFields = frontend::Json::array();
        for (std::size_t index = 0; index < Count64; ++index) {
            omittedFields.push_back("field-" + std::to_string(index));
        }
        Statistics stats64;
        const Validation valid64 = validator::validateGeneratedSchema(
            root, "#/$defs/TruncationMetadata/properties/omittedFields", omittedFields, "omittedFields", Limits{}, &stats64);
        omittedFields[63] = omittedFields[62];
        Statistics duplicate64Stats;
        const Validation duplicate64 = validator::validateGeneratedSchema(
            root, "#/$defs/TruncationMetadata/properties/omittedFields", omittedFields, "omittedFields", Limits{}, &duplicate64Stats);
        result.expectTrue(valid64.valid && stats64.uniqueItemComparisons == Comparisons64,
                          "the actual 64-entry omission-field schema performs exactly 2,016 comparisons");
        result.expectTrue(!duplicate64.valid && duplicate64Stats.uniqueItemComparisons == Comparisons64,
                          "an omission-field duplicate in the last pair fails on comparison 2,016");

        constexpr std::size_t LargeCount = 2'000;
        constexpr std::size_t LargeComparisons = LargeCount * (LargeCount - 1) / 2;
        const frontend::Json schema{{"type", "array"}, {"uniqueItems", true}};
        frontend::Json largeValues = uniqueIntegers(LargeCount);
        Statistics largeStats;
        const Validation large = validateNode(schema, largeValues, &largeStats);
        Statistics limitedStats;
        const Validation limited = validateNode(frontend::Json::object(), schema, largeValues, Limits{32, 10'000}, &limitedStats);
        largeValues[LargeCount - 1] = largeValues[LargeCount - 2];
        Statistics duplicateStats;
        const Validation duplicate = validateNode(schema, largeValues, &duplicateStats);
        const Validation recovered = validateNode(schema, uniqueIntegers(3));
        result.expectTrue(large.valid && largeStats.uniqueItemComparisons == LargeComparisons,
                          "the large unique-items case remains below the fixed four-million-visit production limit");
        result.expectTrue(!limited.valid && limitedStats.complexityRejected && limitedStats.visits == 10'000,
                          "a large unique array stops exactly at a small injected visit limit");
        result.expectTrue(!duplicate.valid && !duplicate.internalFailure && duplicateStats.uniqueItemComparisons == LargeComparisons &&
                              recovered.valid,
                          "a large duplicate in the last pair fails only after the exact final comparison and permits immediate reuse");
    }

    void testRegexUtf8AndReuse(tests::support::TestResult& result) {
        const frontend::Json schema{{"type", "string"}, {"pattern", "^[a-z]+$"}, {"maxLength", 16}};
        Statistics acceptedStats;
        Statistics rejectedStats;
        const Validation accepted = validateNode(schema, "codex", &acceptedStats);
        const Validation rejected = validateNode(schema, "codex-7", &rejectedStats);
        result.expectTrue(accepted.valid && !rejected.valid && acceptedStats.regularExpressionsEvaluated == 1 &&
                              rejectedStats.regularExpressionsEvaluated == 1,
                          "valid and invalid regular-expression matches are counted exactly once");

        const frontend::Json invalidPattern{{"type", "string"}, {"pattern", "["}};
        const std::string secret = "A17A_SCHEMA_VALIDATOR_SECRET";
        const Validation contained = validateNode(invalidPattern, secret);
        const Validation reused = validateNode(schema, "reusable");
        result.expectTrue(!contained.valid && contained.internalFailure && contained.message.find(secret) == std::string::npos &&
                              reused.valid,
                          "an invalid generated regex is contained, redacted, and does not poison later validation");

        std::string invalidUtf8(1, static_cast<char>(0xFF));
        const Validation invalidText = validateNode(frontend::Json{{"type", "string"}}, frontend::Json(invalidUtf8));
        result.expectTrue(!invalidText.valid && !invalidText.internalFailure,
                          "invalid UTF-8 is rejected as caller data before JSON serialization");

        const frontend::Json& root = validator::generatedProtocolSchema();
        result.expectTrue(countGeneratedPatterns(root) == 3,
                          "the runtime schema contains exactly the generated DecimalId, SafePropertyName, and base64 patterns");
        const auto decimalValid = [&root](std::string_view value) {
            Statistics statistics;
            const Validation validation =
                validator::validateGeneratedSchema(root, "#/$defs/DecimalId", frontend::Json(value), "decimalId", Limits{}, &statistics);
            return validation.valid && statistics.regularExpressionsEvaluated == 1;
        };
        const auto decimalInvalid = [&root](std::string_view value) {
            Statistics statistics;
            const Validation validation =
                validator::validateGeneratedSchema(root, "#/$defs/DecimalId", frontend::Json(value), "decimalId", Limits{}, &statistics);
            return !validation.valid && !validation.internalFailure && statistics.regularExpressionsEvaluated == 1;
        };
        result.expectTrue(decimalValid("1") && decimalValid("18446744073709551615"),
                          "the generated DecimalId pattern accepts one and uint64 max");
        result.expectTrue(decimalInvalid("0"), "the generated DecimalId pattern rejects zero");
        result.expectTrue(decimalInvalid("18446744073709551616"), "the generated DecimalId pattern rejects uint64 overflow");
        result.expectTrue(decimalInvalid("01"), "the generated DecimalId pattern rejects a leading zero");
        Statistics overlengthStatistics;
        const Validation overlength = validator::validateGeneratedSchema(
            root, "#/$defs/DecimalId", frontend::Json("184467440737095516150"), "decimalId", Limits{}, &overlengthStatistics);
        result.expectTrue(!overlength.valid && !overlength.internalFailure && overlengthStatistics.regularExpressionsEvaluated == 0,
                          "DecimalId rejects overlength text before regular-expression evaluation");
        result.expectTrue(decimalInvalid("12a"), "the generated DecimalId pattern rejects malformed text");

        const frontend::Json& safePropertyName = root.at("$defs").at("SafePropertyName");
        frontend::Json patternOnly = safePropertyName;
        patternOnly.erase("x-aisuite-forbiddenNormalizedPropertyNames");
        Statistics ordinaryPatternStats;
        Statistics credentialPatternStats;
        const Validation ordinaryPattern = validateNode(root, patternOnly, frontend::Json("future_field"), Limits{}, &ordinaryPatternStats);
        const Validation credentialPattern =
            validateNode(root, patternOnly, frontend::Json("Access-Token"), Limits{}, &credentialPatternStats);
        const Validation actualOrdinary = validator::validateGeneratedSchema(
            root, "#/$defs/SafePropertyName", frontend::Json("future_field"), "property", Limits{}, nullptr);
        const Validation actualCredential = validator::validateGeneratedSchema(
            root, "#/$defs/SafePropertyName", frontend::Json("s-e-s-s-i-o-n token"), "property", Limits{}, nullptr);
        result.expectTrue(
            ordinaryPattern.valid && !credentialPattern.valid && ordinaryPatternStats.regularExpressionsEvaluated == 1 &&
                credentialPatternStats.regularExpressionsEvaluated == 1 && actualOrdinary.valid && !actualCredential.valid,
            "the generated SafePropertyName pattern and optimized normalized-name path accept ordinary fields and reject credential names");
    }

    void testAdditionalPropertiesAndNumericFormats(tests::support::TestResult& result) {
        const frontend::Json unusedAssertionSchema{
            {"type", "object"},
            {"minProperties", 1},
            {"if", frontend::Json{{"required", frontend::Json::array({"mode"})}, {"properties", {{"mode", {{"const", "primary"}}}}}}},
            {"then", frontend::Json{{"required", frontend::Json::array({"primaryValue"})}}},
            {"else", frontend::Json{{"required", frontend::Json::array({"fallbackValue"})}}},
        };
        const Validation emptyObject = validateNode(unusedAssertionSchema, frontend::Json::object());
        const Validation primary = validateNode(unusedAssertionSchema, frontend::Json{{"mode", "primary"}, {"primaryValue", true}});
        const Validation fallback = validateNode(unusedAssertionSchema, frontend::Json{{"fallbackValue", true}});
        const Validation missingFallback = validateNode(unusedAssertionSchema, frontend::Json{{"mode", "other"}});
        result.expectTrue(!emptyObject.valid && primary.valid && fallback.valid && !missingFallback.valid,
                          "the supported but currently ungenerated minProperties and else assertions remain executable");

        const frontend::Json additiveSchema{
            {"type", "object"}, {"properties", {{"known", {{"type", "integer"}}}}}, {"additionalProperties", false}};
        const Validation additive = validateNode(additiveSchema, frontend::Json{{"known", 1}, {"future", "preserved"}});
        const Validation additiveKnownWrong = validateNode(additiveSchema, frontend::Json{{"known", "one"}, {"future", "preserved"}});
        const frontend::Json mapSchema{{"type", "object"}, {"additionalProperties", {{"type", "boolean"}}}};
        const Validation mapValid = validateNode(mapSchema, frontend::Json{{"alpha", true}, {"beta", false}});
        const Validation mapInvalid = validateNode(mapSchema, frontend::Json{{"alpha", "true"}});
        result.expectTrue(additive.valid && !additiveKnownWrong.valid && mapValid.valid && !mapInvalid.valid,
                          "closed provider objects remain extension-compatible while schema-valued maps validate every value");

        const std::optional<generated::MethodId> threadList = generated::definedMethodFromString("thread.list");
        bool publicCompatibility = false;
        if (threadList.has_value()) {
            const frontend::Json safeResult{{"threads", frontend::Json::array()}, {"futureSafe", "preserved"}};
            const auto decoded = frontend::Codec::decodeDefinedResult(*threadList, safeResult);
            const auto encoded =
                decoded ? frontend::Codec::encodeDefinedResult(decoded.value()) : frontend::CodecResult<frontend::Json>{decoded.error()};
            const std::string secret = "A17A_ADDITIVE_CREDENTIAL_SECRET";
            const auto credential = frontend::Codec::decodeDefinedResult(
                *threadList, frontend::Json{{"threads", frontend::Json::array()}, {"Access-Token", secret}});
            const auto wrongKnown = frontend::Codec::decodeDefinedResult(*threadList, frontend::Json{{"threads", "not-an-array"}});
            publicCompatibility = decoded.hasValue() && encoded.hasValue() && encoded.value() == safeResult && !credential &&
                                  credential.error().code == frontend::ErrorCode::InvalidField &&
                                  credential.error().message.find(secret) == std::string::npos && !wrongKnown &&
                                  wrongKnown.error().code == frontend::ErrorCode::InvalidField;
        }
        result.expectTrue(
            publicCompatibility,
            "the public result codec preserves safe additive fields, rejects credential-shaped names, and still validates known fields");

        const auto checkFormat =
            [&result](std::string_view format, std::vector<frontend::Json> validValues, std::vector<frontend::Json> invalidValues) {
                const frontend::Json schema{{"type", "integer"}, {"format", std::string(format)}};
                const bool valid = std::all_of(validValues.begin(), validValues.end(), [&schema](const frontend::Json& value) {
                    return validateNode(schema, value).valid;
                });
                const bool invalid = std::all_of(invalidValues.begin(), invalidValues.end(), [&schema](const frontend::Json& value) {
                    const Validation validation = validateNode(schema, value);
                    return !validation.valid && !validation.internalFailure;
                });
                result.expectTrue(valid && invalid, std::string(format) + " enforces its exact integral boundary");
            };

        checkFormat("int32",
                    {frontend::Json(std::numeric_limits<std::int32_t>::min()), frontend::Json(std::numeric_limits<std::int32_t>::max())},
                    {frontend::Json(static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) - 1),
                     frontend::Json(static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1),
                     frontend::Json(1.5)});
        checkFormat("int64",
                    {frontend::Json(std::numeric_limits<std::int64_t>::min()),
                     frontend::Json(std::numeric_limits<std::int64_t>::max()),
                     frontend::Json(-0x1p63),
                     frontend::Json(0x1p62),
                     frontend::Json(std::nextafter(0x1p63, 0.0))},
                    {frontend::Json(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U),
                     frontend::Json(0x1p63),
                     frontend::Json(std::nextafter(0x1p63, std::numeric_limits<double>::infinity())),
                     frontend::Json(std::nextafter(-0x1p63, -std::numeric_limits<double>::infinity())),
                     frontend::Json(1.5)});
        checkFormat("uint16",
                    {frontend::Json(0), frontend::Json(std::numeric_limits<std::uint16_t>::max())},
                    {frontend::Json(-1), frontend::Json(static_cast<std::uint32_t>(std::numeric_limits<std::uint16_t>::max()) + 1U)});
        checkFormat("uint32",
                    {frontend::Json(0), frontend::Json(std::numeric_limits<std::uint32_t>::max())},
                    {frontend::Json(-1), frontend::Json(static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1U)});
        checkFormat("uint64",
                    {frontend::Json(0),
                     frontend::Json(std::numeric_limits<std::uint64_t>::max()),
                     frontend::Json(0x1p63),
                     frontend::Json(std::nextafter(0x1p64, 0.0))},
                    {frontend::Json(-1),
                     frontend::Json(0x1p64),
                     frontend::Json(std::nextafter(0x1p64, std::numeric_limits<double>::infinity())),
                     frontend::Json(1.5)});
        checkFormat("uint",
                    {frontend::Json(0),
                     frontend::Json(std::numeric_limits<std::uint64_t>::max()),
                     frontend::Json(0x1p63),
                     frontend::Json(std::nextafter(0x1p64, 0.0))},
                    {frontend::Json(-1),
                     frontend::Json(0x1p64),
                     frontend::Json(std::nextafter(0x1p64, std::numeric_limits<double>::infinity())),
                     frontend::Json(1.5)});

        const Validation unknownFormat = validateNode(frontend::Json{{"type", "integer"}, {"format", "future-integer"}}, frontend::Json(1));
        result.expectTrue(!unknownFormat.valid && unknownFormat.internalFailure,
                          "an unknown generated integer format is contained as an internal schema defect");
    }

    void testGeneratedCorpusAndProductionLimits(tests::support::TestResult& result) {
        // These reviewed ceilings retain generous headroom over the measured
        // 3,596/23/1,908/66 maxima while still detecting an unexpected
        // multiplication of ordinary validation work far below the broad
        // four-million-visit emergency bound.
        constexpr std::size_t CorpusVisitCeiling = 10'000;
        constexpr std::size_t CorpusDepthCeiling = 32;
        constexpr std::size_t CorpusReferenceCeiling = 4'096;
        constexpr std::size_t CorpusAlternativeCeiling = 128;
        constexpr std::size_t CorpusDiscriminatorCeiling = 64;
        constexpr std::size_t CorpusRegexCeiling = 32;
        constexpr std::size_t SnapshotVisitCeiling = 300'000;
        const frontend::Json& root = validator::generatedProtocolSchema();
        const frontend::Json& fixtures = goldenFixtures();
        CorpusStatistics corpus;
        bool valid = true;

        for (const frontend::Json& fixture : fixtures.at("methods")) {
            const std::optional<generated::MethodId> id = generated::definedMethodFromString(fixture.at("method").get<std::string>());
            if (!id.has_value()) {
                valid = false;
                continue;
            }
            const generated::MethodMetadata& metadata = generated::AllMethods[static_cast<std::size_t>(*id)];
            for (const std::string_view form : {std::string_view{"minimalParams"}, std::string_view{"completeParams"}}) {
                Statistics statistics;
                const Validation validation =
                    validator::validateGeneratedSchema(root, metadata.parameterSchema, fixture.at(form), form, Limits{}, &statistics);
                valid = valid && validation.valid;
                corpus.observe(statistics);
            }
            for (const std::string_view form : {std::string_view{"minimalResult"}, std::string_view{"completeResult"}}) {
                Statistics statistics;
                const Validation validation =
                    validator::validateGeneratedSchema(root, metadata.resultSchema, fixture.at(form), form, Limits{}, &statistics);
                valid = valid && validation.valid;
                corpus.observe(statistics);
            }
            for (const frontend::Json& nullable : fixture.at("nullableParams")) {
                Statistics statistics;
                const Validation validation =
                    validator::validateGeneratedSchema(root, metadata.parameterSchema, nullable, "nullableParams", Limits{}, &statistics);
                valid = valid && validation.valid;
                corpus.observe(statistics);
            }
            for (const frontend::Json& nullable : fixture.at("nullableResults")) {
                Statistics statistics;
                const Validation validation =
                    validator::validateGeneratedSchema(root, metadata.resultSchema, nullable, "nullableResults", Limits{}, &statistics);
                valid = valid && validation.valid;
                corpus.observe(statistics);
            }
        }
        {
            Statistics statistics;
            const Validation validation = validator::validateGeneratedSchema(
                root, "#/$defs/ExpandedSnapshot", fixtures.at("expandedSnapshot"), "snapshot", Limits{}, &statistics);
            valid = valid && validation.valid;
            corpus.observe(statistics);
        }
        for (const frontend::Json& event : fixtures.at("expandedEvents")) {
            Statistics statistics;
            const Validation validation =
                validator::validateGeneratedSchema(root, "#/$defs/ExpandedFrontendEvent", event, "event", Limits{}, &statistics);
            valid = valid && validation.valid;
            corpus.observe(statistics);
        }
        result.expectTrue(
            valid && corpus.validations == 570 && corpus.maximumVisits <= CorpusVisitCeiling && corpus.maximumDepth <= CorpusDepthCeiling &&
                corpus.maximumReferences <= CorpusReferenceCeiling && corpus.maximumAlternatives <= CorpusAlternativeCeiling &&
                corpus.maximumDiscriminatorFastPaths <= CorpusDiscriminatorCeiling &&
                corpus.maximumRegularExpressions <= CorpusRegexCeiling && corpus.maximumUniqueComparisons == 0,
            "all 570 generated minimal, complete, nullable, snapshot, and event fixtures stay within fixed production ceilings");

        Statistics generatedDepthStatistics;
        const frontend::Json& generatedSnapshot = fixtures.at("expandedSnapshot");
        const Validation generatedDepth = validator::validateGeneratedSchema(
            root, "#/$defs/ExpandedSnapshot", generatedSnapshot, "snapshot", Limits{}, &generatedDepthStatistics);
        Statistics exactGeneratedDepthStatistics;
        const Validation exactGeneratedDepth =
            validator::validateGeneratedSchema(root,
                                               "#/$defs/ExpandedSnapshot",
                                               generatedSnapshot,
                                               "snapshot",
                                               Limits{generatedDepthStatistics.maximumDepthObserved, Limits{}.maximumVisits},
                                               &exactGeneratedDepthStatistics);
        Statistics belowGeneratedDepthStatistics;
        const Validation belowGeneratedDepth =
            validator::validateGeneratedSchema(root,
                                               "#/$defs/ExpandedSnapshot",
                                               generatedSnapshot,
                                               "snapshot",
                                               Limits{generatedDepthStatistics.maximumDepthObserved - 1, Limits{}.maximumVisits},
                                               &belowGeneratedDepthStatistics);
        const auto publicGeneratedSnapshot = frontend::Codec::decodeExpandedSnapshot(generatedSnapshot);
        result.expectTrue(
            generatedDepth.valid && exactGeneratedDepth.valid && !belowGeneratedDepth.valid &&
                belowGeneratedDepthStatistics.complexityRejected && publicGeneratedSnapshot.hasValue() &&
                exactGeneratedDepthStatistics.maximumDepthObserved == generatedDepthStatistics.maximumDepthObserved,
            "a real generated snapshot passes at its measured exact depth, fails one below, and passes public default wiring");

        const frontend::Json executionConfiguration{
            {"approvalPolicy", "on-request"},
            {"approvalsReviewer", "user"},
            {"collaborationMode",
             {{"mode", "plan"},
              {"settings",
               {{"developerInstructions", "coordinate the turn"}, {"model", "gpt-test"}, {"reasoningEffort", "high"}}}}},
            {"cwd", "/workspace"},
            {"effort", "high"},
            {"model", "gpt-test"},
            {"modelProvider", "openai"},
            {"sandboxPolicy",
             {{"type", "workspaceWrite"},
              {"networkAccess", false},
              {"writableRoots", frontend::Json::array({"/workspace"})}}},
        };
        const frontend::Json configuredThread{
            {"id", "thread-config"},
            {"ephemeral", true},
            {"archived", false},
            {"executionConfiguration", executionConfiguration},
        };
        const frontend::Json configuredTurn{
            {"id", "turn-config"},
            {"threadId", "thread-config"},
            {"status", "completed"},
            {"active", false},
            {"terminal", true},
            {"effectiveExecutionConfiguration", executionConfiguration},
            {"effectiveExecutionConfigurationProvenance", "thread_settings_updated"},
        };
        const Validation configuredThreadValidation = validator::validateGeneratedSchema(
            root, "#/$defs/ExpandedThreadState", configuredThread, "thread", Limits{}, nullptr);
        const Validation configuredTurnValidation = validator::validateGeneratedSchema(
            root, "#/$defs/ExpandedTurnState", configuredTurn, "turn", Limits{}, nullptr);
        result.expectTrue(configuredThreadValidation.valid && configuredTurnValidation.valid,
                          "canonical thread and turn schemas accept typed execution configuration and lifecycle fields");

        const std::optional<generated::MethodId> accountLoginCancel = generated::definedMethodFromString("account.login.cancel");
        bool publicDepthWiring = false;
        if (accountLoginCancel.has_value()) {
            const generated::MethodMetadata& metadata = generated::AllMethods[static_cast<std::size_t>(*accountLoginCancel)];
            const auto fixture =
                std::find_if(fixtures.at("methods").begin(), fixtures.at("methods").end(), [](const frontend::Json& value) {
                    return value.at("method") == "account.login.cancel";
                });
            if (fixture != fixtures.at("methods").end()) {
                frontend::Json ordinaryValue = fixture->at("minimalResult");
                frontend::Json excessiveValue = ordinaryValue;
                excessiveValue["futureSafe"] = nestedArrayValue(Limits{}.maximumDepth + 1);
                Statistics excessiveStatistics;
                const Validation excessive = validator::validateGeneratedSchema(
                    root, metadata.resultSchema, excessiveValue, "result", Limits{}, &excessiveStatistics);
                const auto excessivePublic = frontend::Codec::decodeDefinedResult(*accountLoginCancel, excessiveValue);
                const auto recoveredPublic = frontend::Codec::decodeDefinedResult(*accountLoginCancel, ordinaryValue);
                publicDepthWiring = !excessive.valid && excessiveStatistics.complexityRejected &&
                                    excessiveStatistics.maximumDepthObserved > Limits{}.maximumDepth && !excessivePublic &&
                                    excessivePublic.error().code == frontend::ErrorCode::InvalidField &&
                                    excessivePublic.error().message.find("complexity bound") != std::string::npos &&
                                    recoveredPublic.hasValue();
            }
        }
        result.expectTrue(publicDepthWiring,
                          "the generated sensitive-field guard enforces depth 128 through the public codec and remains reusable");

        frontend::Json nestedDetailEvent =
            *std::find_if(fixtures.at("expandedEvents").begin(), fixtures.at("expandedEvents").end(), [](const frontend::Json& candidate) {
                return candidate.at("type") == "diagnostics.updated";
            });
        nestedDetailEvent["data"]["diagnostic"] = {{"outer", {{"inner", "not-a-safe-leaf"}}}};
        const auto rejectedNestedDetail = frontend::Codec::decodeExpandedEvent(nestedDetailEvent);
        const auto recoveredNestedDetail = frontend::Codec::decodeExpandedEvent(
            *std::find_if(fixtures.at("expandedEvents").begin(), fixtures.at("expandedEvents").end(), [](const frontend::Json& candidate) {
                return candidate.at("type") == "diagnostics.updated";
            }));
        result.expectTrue(!rejectedNestedDetail && rejectedNestedDetail.error().code == frontend::ErrorCode::InvalidField &&
                              recoveredNestedDetail.hasValue(),
                          "the non-recursive SafeDetailValue public path rejects nested objects and recovers immediately");

        frontend::Json expandedSnapshot = fixtures.at("expandedSnapshot");
        const frontend::Json item = expandedSnapshot.at("state").at("items").front();
        frontend::Json items = frontend::Json::array();
        for (std::size_t index = 0; index < 2'000; ++index) {
            frontend::Json next = item;
            next["id"] = "item-" + std::to_string(index);
            items.push_back(std::move(next));
        }
        expandedSnapshot["state"]["items"] = std::move(items);
        Statistics snapshotStatistics;
        const Validation snapshotValidation = validator::validateGeneratedSchema(
            root, "#/$defs/ExpandedSnapshot", expandedSnapshot, "snapshot", Limits{}, &snapshotStatistics);
        const auto publicSnapshot = frontend::Codec::decodeExpandedSnapshot(expandedSnapshot);
        result.expectTrue(snapshotValidation.valid && publicSnapshot.hasValue() && !snapshotStatistics.complexityRejected &&
                              snapshotStatistics.visits <= SnapshotVisitCeiling && snapshotStatistics.discriminatorFastPaths >= 2'000,
                          "a schema-valid 2,000-item snapshot passes both the private seam and public production codec");

        frontend::Json detail = frontend::Json::object();
        for (std::size_t index = 0; index < 64; ++index) {
            detail["field_" + std::to_string(index)] = std::string(16'000, static_cast<char>('a' + index % 26));
        }
        frontend::Json event =
            *std::find_if(fixtures.at("expandedEvents").begin(), fixtures.at("expandedEvents").end(), [](const frontend::Json& candidate) {
                return candidate.at("type") == "diagnostics.updated";
            });
        event["data"]["diagnostic"] = detail;
        const std::size_t adversarialBytes = event.dump().size();
        Statistics adversarialStatistics;
        const Validation adversarial =
            validator::validateGeneratedSchema(root, "#/$defs/ExpandedFrontendEvent", event, "event", Limits{}, &adversarialStatistics);
        const auto publicEvent = frontend::Codec::decodeExpandedEvent(event);
        detail["field_0"] = std::string(16'385, 'z');
        const Validation oversized =
            validator::validateGeneratedSchema(root, "#/$defs/SafeDetailObject", detail, "detail", Limits{}, nullptr);
        const auto recoveredEvent = frontend::Codec::decodeExpandedEvent(fixtures.at("expandedEvents").front());
        result.expectTrue(adversarial.valid && publicEvent.hasValue() && adversarialBytes >= 1'000'000 && adversarialBytes < 1'048'576 &&
                              adversarialStatistics.visits <= Limits{}.maximumVisits &&
                              adversarialStatistics.maximumDepthObserved <= Limits{}.maximumDepth && !oversized.valid &&
                              recoveredEvent.hasValue(),
                          "a near-one-MiB bounded public event is accepted and a one-byte-over-limit detail is rejected");

        std::cout << "SCHEMA_VALIDATOR_CORPUS validations=" << corpus.validations << " maxVisits=" << corpus.maximumVisits
                  << " maxDepth=" << corpus.maximumDepth << " maxReferences=" << corpus.maximumReferences
                  << " maxAlternatives=" << corpus.maximumAlternatives
                  << " maxDiscriminatorFastPaths=" << corpus.maximumDiscriminatorFastPaths
                  << " maxUniqueComparisons=" << corpus.maximumUniqueComparisons << " maxRegex=" << corpus.maximumRegularExpressions
                  << '\n';
        std::cout << "SCHEMA_VALIDATOR_SNAPSHOT items=2000 bytes=" << expandedSnapshot.dump().size()
                  << " visits=" << snapshotStatistics.visits << " depth=" << snapshotStatistics.maximumDepthObserved
                  << " alternatives=" << snapshotStatistics.alternativesEvaluated
                  << " discriminatorFastPaths=" << snapshotStatistics.discriminatorFastPaths << '\n';
        std::cout << "SCHEMA_VALIDATOR_ADVERSARY bytes=" << adversarialBytes << " visits=" << adversarialStatistics.visits
                  << " depth=" << adversarialStatistics.maximumDepthObserved << '\n';
    }

    void testSensitiveFieldContainmentAndRecovery(tests::support::TestResult& result) {
        const frontend::Json& root = validator::generatedProtocolSchema();
        const frontend::Json& fixture = goldenFixtures().at("methods").front();
        const std::optional<generated::MethodId> id = generated::definedMethodFromString(fixture.at("method").get<std::string>());
        bool passed = false;
        if (id.has_value()) {
            const generated::MethodMetadata& metadata = generated::AllMethods[static_cast<std::size_t>(*id)];
            frontend::Json unsafe = fixture.at("minimalResult");
            const std::string secret = "A17A_NEVER_REPORT_THIS_SECRET";
            unsafe["nested"] = {{"Access-Token", secret}};
            const Validation rejected =
                validator::validateGeneratedSchema(root, metadata.resultSchema, unsafe, "result", Limits{}, nullptr);
            const Validation recovered =
                validator::validateGeneratedSchema(root, metadata.resultSchema, fixture.at("minimalResult"), "result", Limits{}, nullptr);
            passed = !rejected.valid && !rejected.internalFailure && rejected.message.find(secret) == std::string::npos && recovered.valid;
        }
        result.expectTrue(passed, "sensitive additive result fields are rejected without value leakage and the validator remains reusable");
    }
} // namespace

int main() {
    tests::support::TestResult result;

    testDeterministicComplexityBoundaries(result);
    testDiscriminatorsAndBranchOrder(result);
    testUniqueItems(result);
    testRegexUtf8AndReuse(result);
    testAdditionalPropertiesAndNumericFormats(result);
    testGeneratedCorpusAndProductionLimits(result);
    testSensitiveFieldContainmentAndRecovery(result);

    return result.processResult();
}
