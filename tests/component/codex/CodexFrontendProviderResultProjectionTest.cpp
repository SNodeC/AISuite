/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/detail/ProviderResultProjection.h"
#include "support/TestResult.h"

#include <cstddef>
#include <fstream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace detail = ai::openai::codex::frontend::detail;
    namespace generated = ai::openai::codex::frontend::generated;
    namespace typed = ai::openai::codex::typed;
    using Json = ai::openai::codex::Json;

    const Json& goldenFixtures() {
        static const Json fixtures = [] {
            std::ifstream stream(CODEX_FRONTEND_GOLDEN_FIXTURE);
            if (!stream) {
                throw std::runtime_error("unable to open generated frontend protocol fixtures");
            }
            return Json::parse(stream);
        }();
        return fixtures;
    }

    template <std::size_t Index = 0>
    std::optional<backend::CommandValue> makeProviderResult(std::string_view resultType, const Json& raw) {
        if constexpr (Index == detail::ProviderResultAlternativeCount) {
            return std::nullopt;
        } else {
            if (detail::providerResultTypeNames()[Index] == resultType) {
                using Result = std::variant_alternative_t<Index, backend::ProviderOperationValue>;
                if constexpr (std::is_same_v<Result, typed::Unit>) {
                    return backend::CommandValue{std::in_place_type<Result>};
                } else if constexpr (std::is_same_v<Result, typed::LoginAccountResponse>) {
                    typed::ApiKeyLoginAccountResponse alternative;
                    alternative.raw = raw;
                    return backend::CommandValue{std::in_place_type<Result>, std::move(alternative)};
                } else {
                    Result result{};
                    result.raw = raw;
                    return backend::CommandValue{std::in_place_type<Result>, std::move(result)};
                }
            }
            return makeProviderResult<Index + 1>(resultType, raw);
        }
    }

    const Json* fixtureFor(std::string_view method, std::string_view form) {
        for (const Json& fixture : goldenFixtures().at("methods")) {
            if (fixture.at("method").get<std::string_view>() == method) {
                return &fixture.at(form);
            }
        }
        return nullptr;
    }

    void testExactResultInventoryAndCorpus(tests::support::TestResult& result) {
        const std::span names = detail::providerResultTypeNames();
        const std::set<std::string_view> uniqueNames(names.begin(), names.end());
        std::set<std::string_view> usedNames;
        std::size_t providerMethods = 0;
        std::size_t legacyMethods = 0;
        std::size_t projectedForms = 0;
        bool exact = names.size() == 65 && uniqueNames.size() == 65;

        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            if (metadata.category != generated::MethodCategory::ProviderOperation) {
                continue;
            }
            ++providerMethods;
            usedNames.insert(metadata.resultType);
            exact = exact && uniqueNames.contains(metadata.resultType);
            const bool legacy = detail::requiresLegacyProviderResultProjection(metadata.id);
            legacyMethods += legacy ? 1U : 0U;
            for (const std::string_view form : {"minimalResult", "completeResult"}) {
                const Json* fixture = fixtureFor(metadata.method, form);
                const std::optional<backend::CommandValue> value =
                    fixture == nullptr ? std::nullopt : makeProviderResult(metadata.resultType, *fixture);
                if (!value.has_value()) {
                    exact = false;
                    continue;
                }
                exact = exact && detail::providerResultTypeName(*value) == metadata.resultType;
                const detail::ProviderResultProjection projection = detail::projectProviderResult(metadata.id, *value, 8U * 1024U * 1024U);
                if (legacy) {
                    exact = exact && projection.status == detail::ProviderResultProjectionStatus::LegacyProjectionRequired;
                } else {
                    exact = exact && projection.hasValue() && projection.value == *fixture;
                    ++projectedForms;
                }
            }
        }

        result.expectTrue(exact && providerMethods == 86 && legacyMethods == 6 && projectedForms == 160 && usedNames == uniqueNames,
                          "all 86 provider methods correlate through the exact 65-alternative result inventory; both generated fixture "
                          "forms for all 80 additive methods preserve their exact schema-valid safe result JSON");
    }

    void testProjectionSafetyAndCorrelation(tests::support::TestResult& result) {
        const Json* fsFixture = fixtureFor("fs.readFile", "minimalResult");
        const auto fsMethod = generated::definedMethodFromString("fs.readFile");
        const auto modelMethod = generated::definedMethodFromString("model.list");
        if (fsFixture == nullptr || !fsMethod.has_value() || !modelMethod.has_value()) {
            result.expectTrue(false, "provider projection test fixtures and method metadata are present");
            return;
        }

        Json future = *fsFixture;
        future["futureProjectionField"] = "preserved";
        const std::optional<backend::CommandValue> futureValue = makeProviderResult("FsReadFileResponse", future);
        const detail::ProviderResultProjection futureProjection =
            futureValue ? detail::projectProviderResult(*fsMethod, *futureValue, 8U * 1024U * 1024U) : detail::ProviderResultProjection{};
        result.expectTrue(futureProjection.hasValue() && futureProjection.value == future,
                          "a bounded unknown non-conflicting result field remains byte-for-byte present after generated-schema validation");

        Json credential = *fsFixture;
        credential["accessToken"] = "SYNTHETIC_SECRET_SENTINEL";
        const std::optional<backend::CommandValue> credentialValue = makeProviderResult("FsReadFileResponse", credential);
        const detail::ProviderResultProjection credentialProjection =
            credentialValue ? detail::projectProviderResult(*fsMethod, *credentialValue, 8U * 1024U * 1024U)
                            : detail::ProviderResultProjection{};
        result.expectTrue(credentialProjection.status == detail::ProviderResultProjectionStatus::InvalidResult &&
                              credentialProjection.value.dump().find("SYNTHETIC_SECRET_SENTINEL") == std::string::npos,
                          "credential-shaped provider result fields are rejected without returning secret input in the projection");

        const detail::ProviderResultProjection mismatch =
            futureValue ? detail::projectProviderResult(*modelMethod, *futureValue, 8U * 1024U * 1024U)
                        : detail::ProviderResultProjection{};
        result.expectTrue(mismatch.status == detail::ProviderResultProjectionStatus::ResultTypeMismatch,
                          "a BackendCore result alternative cannot complete a differently correlated frontend method");

        const std::size_t exactSize = future.dump().size();
        const detail::ProviderResultProjection exactBound =
            futureValue ? detail::projectProviderResult(*fsMethod, *futureValue, exactSize) : detail::ProviderResultProjection{};
        const detail::ProviderResultProjection oneByteShort =
            futureValue ? detail::projectProviderResult(*fsMethod, *futureValue, exactSize - 1U) : detail::ProviderResultProjection{};
        result.expectTrue(exactBound.hasValue() && oneByteShort.status == detail::ProviderResultProjectionStatus::ResultTooLarge,
                          "provider result projection admits the exact serialized-byte bound and rejects the next byte");

        const backend::CommandValue controlValue{std::monostate{}};
        result.expectTrue(detail::projectProviderResult(*fsMethod, controlValue, 1024).status ==
                              detail::ProviderResultProjectionStatus::NotProviderResult,
                          "non-provider command completion alternatives cannot enter provider result projection");
    }

    void testLegacySixBoundary(tests::support::TestResult& result) {
        const std::set<std::string_view> expected{
            "thread.start", "thread.resume", "thread.list", "thread.read", "turn.start", "turn.interrupt"};
        std::set<std::string_view> actual;
        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            if (detail::requiresLegacyProviderResultProjection(metadata.id)) {
                actual.insert(metadata.method);
            }
        }
        result.expectTrue(actual == expected,
                          "only the original six provider operations remain on FrontendService's byte-compatible legacy projection path");
    }
} // namespace

int main() {
    tests::support::TestResult result;

    static_assert(std::variant_size_v<backend::ProviderOperationValue> == 65);
    static_assert(std::variant_size_v<backend::CommandValue> == 68);
    testExactResultInventoryAndCorpus(result);
    testProjectionSafetyAndCorrelation(result);
    testLegacySixBoundary(result);

    return result.processResult();
}
