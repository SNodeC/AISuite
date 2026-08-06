/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/detail/FrontendCapabilities.h"
#include "support/TestResult.h"

#include <array>
#include <cstddef>
#include <set>
#include <span>
#include <string_view>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;

    bool profileContains(std::span<const frontend::FrontendScope> profile, frontend::FrontendScope required) {
        for (const frontend::FrontendScope scope : profile) {
            if (scope == required) {
                return true;
            }
        }
        return false;
    }

    bool isPermittedForProfile(const generated::MethodMetadata& method, std::span<const frontend::FrontendScope> profile) {
        for (const frontend::FrontendScope required : method.requiredScopes) {
            if (!profileContains(profile, required)) {
                return false;
            }
        }
        return true;
    }

    void testCompleteAuthorizationMatrix(tests::support::TestResult& result) {
        std::size_t implemented = 0;
        std::size_t available = 0;
        std::size_t defaultRemotePermitted = 0;
        std::size_t localTrustedPermitted = 0;
        std::size_t privilegedProviderExcluded = 0;
        std::size_t reverseExcluded = 0;
        std::size_t lifecycleExcluded = 0;
        std::size_t providerReady = 0;
        std::size_t legacy = 0;
        bool complete = true;

        for (const generated::MethodMetadata& method : generated::AllMethods) {
            const bool providerPath = method.category == generated::MethodCategory::ProviderOperation ||
                                      method.category == generated::MethodCategory::ReverseResponse;
            complete = complete && method.currentlyImplemented && !method.parameterSchema.empty() && !method.resultSchema.empty() &&
                       !method.resultType.empty() && method.providerReadyRequired == providerPath;
            complete = complete && ((method.frontendNative && method.registryKeys.empty()) ||
                                    (!method.frontendNative && (!method.registryKeys.empty() || !method.genericContractKey.empty())));
            if (method.category == generated::MethodCategory::ProviderOperation ||
                method.category == generated::MethodCategory::ReverseResponse) {
                complete = complete && !method.backendCommand.empty() && method.serviceAction.empty();
            } else if (method.category == generated::MethodCategory::ProviderLifecycle) {
                complete = complete && method.backendCommand.empty() && !method.serviceAction.empty();
            } else {
                complete = complete && !method.backendCommand.empty() && !method.serviceAction.empty();
            }

            implemented += method.currentlyImplemented ? 1U : 0U;
            providerReady += method.providerReadyRequired ? 1U : 0U;
            legacy += method.legacyCompatibilityMethod ? 1U : 0U;
            if (!method.currentlyImplemented || !method.defaultEnabled) {
                continue;
            }
            ++available;
            const bool remote = isPermittedForProfile(method, frontend::DefaultRemoteScopes);
            const bool local = isPermittedForProfile(method, frontend::LocalTrustedScopes);
            defaultRemotePermitted += remote ? 1U : 0U;
            localTrustedPermitted += local ? 1U : 0U;
            if (remote) {
                continue;
            }
            if (method.category == generated::MethodCategory::ProviderOperation && method.securityDecision == "PrivilegedScopedApproved") {
                ++privilegedProviderExcluded;
            } else if (method.category == generated::MethodCategory::ReverseResponse) {
                ++reverseExcluded;
            } else if (method.category == generated::MethodCategory::ProviderLifecycle) {
                ++lifecycleExcluded;
            } else {
                complete = false;
            }
        }

        result.expectTrue(complete && generated::AllMethods.size() == 105 && implemented == 105 && available == 90 &&
                              defaultRemotePermitted == 53 && localTrustedPermitted == 90 && providerReady == 98 && legacy == 15,
                          "all 105 generated methods carry complete handler/result/readiness metadata; default availability is 90, "
                          "default_remote permits 53, local_trusted permits 90, provider readiness covers 98, and legacy remains 15");
        result.expectTrue(privilegedProviderExcluded == 22 && reverseExcluded == 12 && lifecycleExcluded == 3 &&
                              defaultRemotePermitted + privilegedProviderExcluded + reverseExcluded + lifecycleExcluded == available,
                          "default_remote exclusion is independently decomposed as 22 privileged providers + 12 reverse methods + "
                          "3 lifecycle methods, so 53 + 37 = 90");
    }

    void testIndependentOwnerCategoryDerivation(tests::support::TestResult& result) {
        std::size_t observer = 0;
        std::size_t controller = 0;
        std::size_t privileged = 0;
        std::size_t conditional = 0;
        std::size_t parameterSensitive = 0;
        std::size_t reachableNative = 0;
        for (const generated::MethodMetadata& method : generated::AllMethods) {
            if (method.category == generated::MethodCategory::ProviderOperation) {
                observer += method.securityDecision == "ObserverReadApproved" ? 1U : 0U;
                controller += method.securityDecision == "ControllerRequiredApproved" ? 1U : 0U;
                privileged += method.securityDecision == "PrivilegedScopedApproved" ? 1U : 0U;
                conditional += method.securityDecision == "ConditionalExplicitEnablementApproved" ? 1U : 0U;
                parameterSensitive += method.securityDecision == "ParameterSensitiveApproved" ? 1U : 0U;
            }
            reachableNative += method.frontendNative && method.category != generated::MethodCategory::ProviderLifecycle &&
                                       isPermittedForProfile(method, frontend::DefaultRemoteScopes)
                                   ? 1U
                                   : 0U;
        }
        const std::size_t registryReachable = observer + controller + parameterSensitive;
        result.expectTrue(observer == 26 && controller == 22 && privileged == 22 && conditional == 15 && parameterSensitive == 1 &&
                              registryReachable == 49 && reachableNative == 4 && registryReachable + reachableNative == 53,
                          "the independent owner-category derivation is 26 observer + 22 controller + 22 privileged + 15 conditional "
                          "+ 1 parameter-sensitive; 49 reachable operations + 4 native methods independently yields 53");
    }

    void testFrozenSetsAndCapabilities(tests::support::TestResult& result) {
        const std::set<std::string_view> expectedConditional{
            "command.exec",
            "command.exec.resize",
            "command.exec.terminate",
            "command.exec.write",
            "fs.copy",
            "fs.createDirectory",
            "fs.getMetadata",
            "fs.readDirectory",
            "fs.readFile",
            "fs.remove",
            "fs.unwatch",
            "fs.watch",
            "fs.writeFile",
            "fuzzyFileSearch",
            "thread.shellCommand",
        };
        const std::set<std::string_view> expectedLegacy{
            "controller.acquire",
            "controller.release",
            "events.replay",
            "request.approval.respond",
            "request.authentication.respond",
            "request.unknown.reject",
            "request.unknown.respond",
            "request.userInput.respond",
            "snapshot.get",
            "thread.list",
            "thread.read",
            "thread.resume",
            "thread.start",
            "turn.interrupt",
            "turn.start",
        };
        std::set<std::string_view> conditional;
        std::set<std::string_view> legacy;
        for (const generated::MethodMetadata& method : generated::AllMethods) {
            if (!method.defaultEnabled) {
                conditional.insert(method.method);
            }
            if (method.legacyCompatibilityMethod) {
                legacy.insert(method.method);
            }
        }

        const std::set<std::string_view> expectedMechanisms{
            "authenticated_frontend",
            "complete_backend_domains",
            "complete_provider_operations",
            "complete_reverse_requests",
            "complete_thread_items",
            "conditional_command_execution",
            "conditional_filesystem",
            "dedicated_notification_events",
            "dedicated_pending_requests",
            "method_discovery",
            "provider_lifecycle",
            "security_scopes",
            "scope_projected_state",
        };
        const std::set<std::string_view> expectedProducts{"browser_ui", "cpp_client_sdk", "qt_ui", "typescript_client_sdk"};
        std::set<std::string_view> mechanisms;
        std::set<std::string_view> products;
        std::set<std::string_view> topology;
        for (const generated::CapabilityMetadata& capability : generated::AllCapabilities) {
            switch (capability.category) {
                case generated::CapabilityCategory::StaticMechanism:
                    mechanisms.insert(capability.key);
                    break;
                case generated::CapabilityCategory::ConditionalTopology:
                    topology.insert(capability.key);
                    break;
                case generated::CapabilityCategory::Product:
                    products.insert(capability.key);
                    break;
            }
        }

        result.expectTrue(conditional == expectedConditional && legacy == expectedLegacy,
                          "the exact 15 default-disabled and exact 15 legacy-compatible method sets remain frozen independently");
        result.expectTrue(mechanisms == expectedMechanisms && products == expectedProducts &&
                              topology == std::set<std::string_view>{"multi_transport"},
                          "capability taxonomy independently classifies thirteen static mechanisms, multi_transport topology, and four "
                          "product capabilities regardless of current implementation truth");

        const auto sdkSingle = ai::openai::codex::frontend::detail::computeCapabilities(true, 1);
        const auto sdkMultiple = ai::openai::codex::frontend::detail::computeCapabilities(true, 2);
        const auto noSdkSingle = ai::openai::codex::frontend::detail::computeCapabilities(false, 1);
        const auto noSdkMultiple = ai::openai::codex::frontend::detail::computeCapabilities(false, 2);
        result.expectTrue(sdkSingle.staticMechanisms == 13 && sdkSingle.conditionalTopology == 0 && sdkSingle.implementedProducts == 1 &&
                              sdkSingle.advertisement.implemented.size() == 14 && sdkMultiple.staticMechanisms == 13 &&
                              sdkMultiple.conditionalTopology == 1 && sdkMultiple.implementedProducts == 1 &&
                              sdkMultiple.advertisement.implemented.size() == 15 && noSdkSingle.staticMechanisms == 13 &&
                              noSdkSingle.conditionalTopology == 0 && noSdkSingle.implementedProducts == 0 &&
                              noSdkSingle.advertisement.implemented.size() == 13 && noSdkMultiple.staticMechanisms == 13 &&
                              noSdkMultiple.conditionalTopology == 1 && noSdkMultiple.implementedProducts == 0 &&
                              noSdkMultiple.advertisement.implemented.size() == 14,
                          "build-derived SDK truth and topology-derived multi_transport produce the exact four capability totals");

        const auto accountRead = generated::definedMethodFromString("account.read");
        const bool accountPolicy =
            accountRead.has_value() && generated::AllMethods[static_cast<std::size_t>(*accountRead)].parameterPolicy.find(
                                           "refreshToken true") != std::string_view::npos;
        result.expectTrue(accountPolicy,
                          "account.read explicitly retains its observer form plus the validated refreshToken=true privileged branch");
    }
} // namespace

int main() {
    tests::support::TestResult result;

    static_assert(generated::MethodCount == 105);
    static_assert(generated::ImplementedMethodCount == 105);
    static_assert(generated::DefaultAvailableMethodCount == 90);
    static_assert(generated::DefaultRemotePermittedMethodCount == 53);
    static_assert(generated::LocalTrustedPermittedMethodCount == 90);
    static_assert(generated::ProviderReadyRequiredMethodCount == 98);
    static_assert(generated::ImplementedMechanismCapabilityCount == 13);
    testCompleteAuthorizationMatrix(result);
    testIndependentOwnerCategoryDerivation(result);
    testFrozenSetsAndCapabilities(result);

    return result.processResult();
}
