/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Protocol.h"
#include "ai/openai/codex/frontend/Security.h"
#include "support/TestResult.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;

    using Alias = std::pair<generated::MethodId, std::string_view>;

    constexpr std::array<Alias, 15> ExistingProtocolAliases{{
        {generated::MethodId::ControllerAcquire, frontend::method::ControllerAcquire},
        {generated::MethodId::ControllerRelease, frontend::method::ControllerRelease},
        {generated::MethodId::SnapshotGet, frontend::method::SnapshotGet},
        {generated::MethodId::EventsReplay, frontend::method::EventsReplay},
        {generated::MethodId::ThreadStart, frontend::method::ThreadStart},
        {generated::MethodId::ThreadResume, frontend::method::ThreadResume},
        {generated::MethodId::ThreadList, frontend::method::ThreadList},
        {generated::MethodId::ThreadRead, frontend::method::ThreadRead},
        {generated::MethodId::TurnStart, frontend::method::TurnStart},
        {generated::MethodId::TurnInterrupt, frontend::method::TurnInterrupt},
        {generated::MethodId::ApprovalRespond, frontend::method::ApprovalRespond},
        {generated::MethodId::UserInputRespond, frontend::method::UserInputRespond},
        {generated::MethodId::AuthenticationRespond, frontend::method::AuthenticationRespond},
        {generated::MethodId::UnknownRequestRespond, frontend::method::UnknownRequestRespond},
        {generated::MethodId::UnknownRequestReject, frontend::method::UnknownRequestReject},
    }};

    static_assert(generated::MethodCount == 105);
    static_assert(generated::ImplementedMethodCount == 105);
    static_assert(generated::ExistingMethodCount == 15);
    static_assert(generated::AdditiveMethodCount == 90);
    static_assert(generated::DefaultAvailableMethodCount == 90);
    static_assert(generated::DefaultRemotePermittedMethodCount == 53);
    static_assert(generated::LocalTrustedPermittedMethodCount == 90);
    static_assert(generated::FrontendNativeMethodCount == 7);
    static_assert(generated::NonNativeMethodCount == 98);
    static_assert(generated::ProviderOperationMethodCount == 86);
    static_assert(generated::ReverseMethodCount == 12);
    static_assert(generated::ProviderLifecycleMethodCount == 3);
    static_assert(generated::ImplementedMechanismCapabilityCount == 13);
    static_assert(generated::AllPendingRequestProjections.size() == 10);
    static_assert(std::variant_size_v<generated::CompleteCommandParameters> == 105);
    static_assert(std::variant_size_v<generated::CompleteCommandResult> == 105);
    static_assert(frontend::LocalTrustedScopes.size() == 12);
    static_assert(frontend::DefaultRemoteScopes.size() == 2);

    void testCounts(tests::support::TestResult& result) {
        result.expectTrue(generated::MethodCount == 105 && generated::ImplementedMethodCount == 105 &&
                              generated::ExistingMethodCount == 15 && generated::AdditiveMethodCount == 90 &&
                              generated::DefaultAvailableMethodCount == 90 && generated::DefaultRemotePermittedMethodCount == 53 &&
                              generated::LocalTrustedPermittedMethodCount == 90 && generated::FrontendNativeMethodCount == 7 &&
                              generated::NonNativeMethodCount == 98 && generated::ProviderOperationMethodCount == 86 &&
                              generated::ReverseMethodCount == 12 && generated::ProviderLifecycleMethodCount == 3,
                          "generated frontend metadata preserves the exact 105 implemented, 15 legacy, 90 default-available, "
                          "53 default_remote, 90 local_trusted, and 7/98/86/12/3 category census");
        result.expectEqual(std::size_t{105},
                           std::variant_size_v<generated::CompleteCommandParameters>,
                           "complete command parameters have one alternative for each of the 105 defined methods");
        result.expectEqual(std::size_t{105},
                           std::variant_size_v<generated::CompleteCommandResult>,
                           "complete command results have one alternative for each of the 105 defined methods");
    }

    void testExactMethodLookup(tests::support::TestResult& result) {
        bool exactLookup = true;
        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            const std::optional<generated::MethodId> found = generated::definedMethodFromString(metadata.method);
            exactLookup = exactLookup && found == metadata.id && generated::methodString(metadata.id) == metadata.method;
        }
        result.expectTrue(exactLookup, "all 105 defined methods have an exact generated id/string lookup round-trip");

        constexpr std::array<Alias, 6> PrefixFamilyMethods{{
            {generated::MethodId::CommandExec, "command.exec"},
            {generated::MethodId::CommandExecResize, "command.exec.resize"},
            {generated::MethodId::CommandExecTerminate, "command.exec.terminate"},
            {generated::MethodId::CommandExecWrite, "command.exec.write"},
            {generated::MethodId::ExternalAgentConfigImport, "externalAgentConfig.import"},
            {generated::MethodId::ExternalAgentConfigImportHistoriesRead, "externalAgentConfig.import.readHistories"},
        }};
        bool prefixFamilyExact = true;
        for (const auto& [id, method] : PrefixFamilyMethods) {
            prefixFamilyExact = prefixFamilyExact && generated::definedMethodFromString(method) == id;
        }
        result.expectTrue(prefixFamilyExact,
                          "command.exec and externalAgentConfig.import prefix families retain distinct exact method identities");

        constexpr std::array<std::string_view, 9> NonMethods{
            "command",
            "command.",
            "command.exec.",
            "command.exec.resiz",
            "command.exec.resize.extra",
            "externalAgentConfig",
            "externalAgentConfig.import.",
            "externalAgentConfig.import.read",
            "externalAgentConfig.import.readHistories.extra",
        };
        result.expectTrue(std::all_of(NonMethods.begin(),
                                      NonMethods.end(),
                                      [](std::string_view value) {
                                          return !generated::definedMethodFromString(value).has_value() &&
                                                 !generated::runtimeMethodFromString(value).has_value();
                                      }),
                          "method lookup rejects prefixes, truncated names, trailing separators, and extended exact names");
    }

    void testParameterAndAvailabilityRoundTrips(tests::support::TestResult& result) {
        std::size_t runtimeMethods = 0;
        std::size_t conditionalMethods = 0;
        bool exactParameters = true;
        bool exactRuntimeAvailability = true;
        bool conditionalDefaultOff = true;

        for (std::size_t index = 0; index < generated::AllMethods.size(); ++index) {
            const generated::MethodMetadata& metadata = generated::AllMethods[index];
            const nlohmann::json marker{{"index", index}, {"method", metadata.method}};
            const generated::CompleteCommandParameters parameters = generated::makeParameters(metadata.id, marker);
            exactParameters = exactParameters && generated::commandMethod(parameters) == metadata.id &&
                              std::visit(
                                  [&marker](const auto& value) {
                                      return value.value == marker;
                                  },
                                  parameters);

            const std::optional<generated::MethodId> available = generated::runtimeMethodFromString(metadata.method);
            exactRuntimeAvailability =
                exactRuntimeAvailability && (metadata.currentlyImplemented ? available == metadata.id : !available.has_value());
            runtimeMethods += metadata.currentlyImplemented ? 1U : 0U;

            if (metadata.exposure == "ConditionallyExposedFrontendMethod") {
                ++conditionalMethods;
                conditionalDefaultOff = conditionalDefaultOff && !metadata.defaultEnabled;
            }
        }

        result.expectTrue(exactParameters,
                          "all 105 methods preserve their exact MethodId and JSON value through makeParameters/commandMethod");
        result.expectTrue(exactRuntimeAvailability && runtimeMethods == 105,
                          "runtime lookup correlates every one of the 105 implemented A1.7b method handlers");
        result.expectTrue(conditionalMethods == 15 && conditionalDefaultOff,
                          "all and only the 15 conditional filesystem or command-execution methods are disabled by default");
    }

    void testSecurityMetadata(tests::support::TestResult& result) {
        result.expectTrue(frontend::DefaultRemoteScopes == std::array{frontend::FrontendScope::Observe, frontend::FrontendScope::Control},
                          "default remote authorization contains exactly Observe and Control in stable order");
        result.expectTrue(frontend::DefaultRemoteScopeProfile.name == "default_remote" && frontend::DefaultRemoteScopeProfile.remote &&
                              frontend::DefaultRemoteScopeProfile.scopes.size() == frontend::DefaultRemoteScopes.size(),
                          "the default-remote profile exposes its exact name, remote classification, and scope count");
        result.expectTrue(frontend::LocalTrustedScopeProfile.name == "local_trusted" && !frontend::LocalTrustedScopeProfile.remote &&
                              frontend::LocalTrustedScopeProfile.scopes.size() == frontend::LocalTrustedScopes.size(),
                          "the local-trusted profile exposes all 12 local scopes without classifying itself as remote");

        bool scopeRoundTrip = true;
        for (const frontend::FrontendScope scope : frontend::LocalTrustedScopes) {
            scopeRoundTrip = scopeRoundTrip && !frontend::toString(scope).empty() &&
                             frontend::frontendScopeFromString(frontend::toString(scope)) == scope;
        }
        result.expectTrue(scopeRoundTrip && !frontend::frontendScopeFromString("observe.extra").has_value(),
                          "all 12 security scopes round-trip exactly without prefix matching");
    }

    template <std::size_t Size>
    bool projectionRegistryKeysAreUnique(const std::array<generated::ProjectionMetadata, Size>& mappings) {
        for (std::size_t first = 0; first < mappings.size(); ++first) {
            for (std::size_t second = first + 1; second < mappings.size(); ++second) {
                if (mappings[first].registryKey == mappings[second].registryKey) {
                    return false;
                }
            }
        }
        return true;
    }

    template <std::size_t Size>
    bool projectionKeysMatchReviewedContracts(const std::array<generated::ProjectionMetadata, Size>& mappings,
                                              std::string_view registryPrefix) {
        const auto reviewedCount = std::count_if(generated::AllReviewedContracts.begin(),
                                                 generated::AllReviewedContracts.end(),
                                                 [registryPrefix](const generated::ContractMetadata& contract) {
                                                     return contract.registryKey.starts_with(registryPrefix);
                                                 });
        if (reviewedCount != static_cast<std::ptrdiff_t>(mappings.size())) {
            return false;
        }
        return std::all_of(mappings.begin(), mappings.end(), [](const generated::ProjectionMetadata& mapping) {
            return std::count_if(generated::AllReviewedContracts.begin(),
                                 generated::AllReviewedContracts.end(),
                                 [&mapping](const generated::ContractMetadata& contract) {
                                     return contract.registryKey == mapping.registryKey && contract.exposure == mapping.exposure &&
                                            contract.securityDecision == mapping.securityDecision &&
                                            contract.compatibilityBehavior == mapping.legacyContract;
                                 }) == 1;
        });
    }

    template <std::size_t Size>
    bool projectionSelectionIsExclusive(const std::array<generated::ProjectionMetadata, Size>& mappings) {
        constexpr std::array CurrentRuntimeCapabilities{
            generated::Capability::MethodDiscovery,
            generated::Capability::SecurityScopes,
        };
        return std::all_of(mappings.begin(),
                           mappings.end(),
                           [](const generated::ProjectionMetadata& mapping) {
                               constexpr std::array UnrelatedCapabilities{
                                   generated::Capability::MethodDiscovery,
                                   generated::Capability::SecurityScopes,
                               };
                               const std::array RequiredCapabilities{
                                   generated::Capability::MethodDiscovery,
                                   mapping.expansionCapability,
                                   mapping.expansionCapability,
                               };
                               const auto legacy =
                                   generated::selectCompatibilityRepresentation(mapping, std::span<const generated::Capability>{});
                               const auto unrelated = generated::selectCompatibilityRepresentation(mapping, UnrelatedCapabilities);
                               const auto expanded = generated::selectCompatibilityRepresentation(mapping, RequiredCapabilities);
                               const auto exactlyOne = [](generated::CompatibilityRepresentation representation) {
                                   return generated::emitsLegacy(representation) != generated::emitsExpanded(representation);
                               };
                               return legacy == generated::CompatibilityRepresentation::Legacy &&
                                      unrelated == generated::CompatibilityRepresentation::Legacy &&
                                      expanded == generated::CompatibilityRepresentation::Expanded && exactlyOne(legacy) &&
                                      exactlyOne(unrelated) && exactlyOne(expanded);
                           }) &&
               std::all_of(mappings.begin(), mappings.end(), [&CurrentRuntimeCapabilities](const generated::ProjectionMetadata& mapping) {
                   return generated::selectCompatibilityRepresentation(mapping, CurrentRuntimeCapabilities) ==
                          generated::CompatibilityRepresentation::Legacy;
               });
    }

    void testCompatibilityProjection(tests::support::TestResult& result) {
        result.expectTrue(generated::AllNotificationProjections.size() == 68 && generated::AllThreadItemProjections.size() == 18 &&
                              projectionRegistryKeysAreUnique(generated::AllNotificationProjections) &&
                              projectionRegistryKeysAreUnique(generated::AllThreadItemProjections),
                          "generated compatibility projection covers 68 unique notifications and 18 unique ThreadItems");

        result.expectTrue(projectionKeysMatchReviewedContracts(generated::AllNotificationProjections, "server_notification:") &&
                              projectionKeysMatchReviewedContracts(generated::AllThreadItemProjections, "item_discriminator:ThreadItem:"),
                          "notification and ThreadItem projection keys and final decisions bijectively match reviewed registry contracts");

        const auto notificationLegacyNormalized = std::count_if(generated::AllNotificationProjections.begin(),
                                                                generated::AllNotificationProjections.end(),
                                                                [](const generated::ProjectionMetadata& mapping) {
                                                                    return mapping.legacyContract == "legacy_normalized";
                                                                });
        const auto notificationLegacyExtensions = std::count_if(generated::AllNotificationProjections.begin(),
                                                                generated::AllNotificationProjections.end(),
                                                                [](const generated::ProjectionMetadata& mapping) {
                                                                    return mapping.legacyContract == "legacy_redacted_extension";
                                                                });
        const auto itemLegacyNormalized = std::count_if(generated::AllThreadItemProjections.begin(),
                                                        generated::AllThreadItemProjections.end(),
                                                        [](const generated::ProjectionMetadata& mapping) {
                                                            return mapping.legacyContract == "legacy_normalized";
                                                        });
        const auto itemLegacyMetadata = std::count_if(generated::AllThreadItemProjections.begin(),
                                                      generated::AllThreadItemProjections.end(),
                                                      [](const generated::ProjectionMetadata& mapping) {
                                                          return mapping.legacyContract == "legacy_metadata_only";
                                                      });
        const auto completePolicy = [](const generated::ProjectionMetadata& mapping) {
            return mapping.securityDecision == "ScopeProjectedStateEventApproved" && !mapping.expandedMappings.empty() &&
                   mapping.requiredScopes.size() == 1 && mapping.requiredScopes.front() == frontend::FrontendScope::Observe;
        };
        const auto notificationExposureMatchesLegacy = [](const generated::ProjectionMetadata& mapping) {
            return (mapping.legacyContract == "legacy_normalized" && mapping.exposure == "ExistingEventContractApproved") ||
                   (mapping.legacyContract == "legacy_redacted_extension" &&
                    mapping.exposure == "DedicatedEventWithLegacyExtensionCompatibility");
        };
        const auto itemExposureMatchesLegacy = [](const generated::ProjectionMetadata& mapping) {
            return (mapping.legacyContract == "legacy_normalized" && mapping.exposure == "ExistingEventContractApproved") ||
                   (mapping.legacyContract == "legacy_metadata_only" && mapping.exposure == "DedicatedItemWithLegacyMetadataCompatibility");
        };
        result.expectTrue(
            notificationLegacyNormalized == 14 && notificationLegacyExtensions == 54 && itemLegacyNormalized == 8 &&
                itemLegacyMetadata == 10 &&
                std::all_of(generated::AllNotificationProjections.begin(), generated::AllNotificationProjections.end(), completePolicy) &&
                std::all_of(generated::AllThreadItemProjections.begin(), generated::AllThreadItemProjections.end(), completePolicy) &&
                std::all_of(generated::AllNotificationProjections.begin(),
                            generated::AllNotificationProjections.end(),
                            notificationExposureMatchesLegacy) &&
                std::all_of(
                    generated::AllThreadItemProjections.begin(), generated::AllThreadItemProjections.end(), itemExposureMatchesLegacy),
            "projection metadata preserves exact 14/54 notification and 8/10 ThreadItem compatibility buckets");

        result.expectTrue(projectionSelectionIsExclusive(generated::AllNotificationProjections) &&
                              projectionSelectionIsExclusive(generated::AllThreadItemProjections),
                          "all 86 contracts select exactly one legacy or expanded representation per capability profile");
    }

    void testPendingRequestProjection(tests::support::TestResult& result) {
        struct ExpectedPendingRequest {
            std::string_view providerMethod;
            std::string_view kind;
            std::string_view responseMethod;
            bool supportsKnownReject;
        };
        constexpr std::array<ExpectedPendingRequest, 10> Expected{{
            {"item/commandExecution/requestApproval", "command_execution_approval", "request.approval.respond", false},
            {"item/fileChange/requestApproval", "file_change_approval", "request.approval.respond", false},
            {"item/tool/requestUserInput", "user_input", "request.userInput.respond", true},
            {"account/chatgptAuthTokens/refresh", "authentication", "request.authentication.respond", false},
            {"applyPatchApproval", "apply_patch_approval", "request.applyPatchApproval.respond", false},
            {"execCommandApproval", "exec_command_approval", "request.execCommandApproval.respond", false},
            {"item/permissions/requestApproval", "permissions_approval", "request.permissionsApproval.respond", false},
            {"attestation/generate", "attestation", "request.attestation.respond", true},
            {"item/tool/call", "dynamic_tool_call", "request.dynamicTool.respond", true},
            {"mcpServer/elicitation/request", "mcp_elicitation", "request.mcpElicitation.respond", true},
        }};

        bool exact = generated::AllPendingRequestProjections.size() == Expected.size();
        for (std::size_t index = 0; index < Expected.size(); ++index) {
            const auto& expected = Expected[index];
            const generated::PendingRequestProjectionMetadata& metadata = generated::AllPendingRequestProjections[index];
            const std::size_t expectedResponseCount = expected.supportsKnownReject ? 2U : 1U;
            exact =
                exact && metadata.providerMethod == expected.providerMethod && metadata.kind == expected.kind &&
                metadata.registryKey == "server_request:ServerRequest:method:" + std::string(expected.providerMethod) &&
                metadata.exposure == "DedicatedPendingRequestContract" && metadata.securityDecision == "ScopeProjectedStateEventApproved" &&
                metadata.legacyContract == "legacy_generic_or_existing_dedicated" && metadata.expandedEvent == "pendingRequests.updated" &&
                metadata.responseMethods.size() == expectedResponseCount && metadata.responseMethods.front() == expected.responseMethod &&
                (!expected.supportsKnownReject || metadata.responseMethods.back() == "request.known.reject") &&
                metadata.presentationRequiredScopes.size() == 1 &&
                metadata.presentationRequiredScopes.front() == frontend::FrontendScope::Observe &&
                !metadata.controllerRequiredForPresentation && metadata.responseRequiredScopes.size() == 2 &&
                metadata.responseRequiredScopes[0] == frontend::FrontendScope::Control &&
                metadata.responseRequiredScopes[1] == frontend::FrontendScope::SensitiveResponse &&
                metadata.controllerRequiredForResponse && metadata.redactionClass == "safe_pending_request" &&
                metadata.duplicateSuppression == "exactly_one_compatibility_representation_per_connection" &&
                metadata.expansionCapability == generated::Capability::DedicatedPendingRequests &&
                generated::pendingRequestProjectionFromKind(expected.kind) == &metadata &&
                generated::pendingRequestProjectionFromProviderMethod(expected.providerMethod) == &metadata;
        }
        const auto reviewedServerRequests =
            std::count_if(generated::AllReviewedContracts.begin(),
                          generated::AllReviewedContracts.end(),
                          [](const generated::ContractMetadata& contract) {
                              return contract.registryKey.starts_with("server_request:ServerRequest:method:");
                          });
        // Ten stable server requests have dedicated projections. The same
        // reviewed registry bucket also contains one experimental request,
        // which remains explicitly not exposed and has no projection entry.
        result.expectTrue(exact && reviewedServerRequests == 11 && generated::pendingRequestProjectionFromKind("unknown") == nullptr &&
                              generated::pendingRequestProjectionFromProviderMethod("unknown") == nullptr,
                          "generated pending-request metadata bijectively freezes all ten kinds, exact typed response methods, "
                          "scope/controller policy, redaction, compatibility, and dedicated capability selection");
    }

    void testExistingProtocolAliases(tests::support::TestResult& result) {
        bool aliasesMatch = true;
        for (const auto& [id, alias] : ExistingProtocolAliases) {
            aliasesMatch = aliasesMatch && alias == generated::methodString(id) && generated::runtimeMethodFromString(alias) == id &&
                           generated::legacyMethodFromString(alias) == id;
        }
        result.expectTrue(aliasesMatch, "Protocol.h aliases all 15 existing methods to their generated spellings and runtime identities");

        const bool everyLegacyMethodAliased =
            std::all_of(generated::AllMethods.begin(), generated::AllMethods.end(), [](const generated::MethodMetadata& metadata) {
                return !metadata.legacyCompatibilityMethod ||
                       std::any_of(ExistingProtocolAliases.begin(), ExistingProtocolAliases.end(), [&](const Alias& alias) {
                           return alias.first == metadata.id && alias.second == metadata.method;
                       });
            });
        result.expectTrue(everyLegacyMethodAliased,
                          "the 15-name Protocol.h compatibility alias set covers every frozen legacy method exactly once");
    }
} // namespace

int main() {
    tests::support::TestResult result;

    testCounts(result);
    testExactMethodLookup(result);
    testParameterAndAvailabilityRoundTrips(result);
    testSecurityMetadata(result);
    testCompatibilityProjection(result);
    testPendingRequestProjection(result);
    testExistingProtocolAliases(result);

    return result.processResult();
}
