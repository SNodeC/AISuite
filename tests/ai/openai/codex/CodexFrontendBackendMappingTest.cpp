/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/backend/BackendCommand.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "support/TestResult.h"

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>

namespace {
    namespace backend = ai::openai::codex::backend;
    namespace generated = ai::openai::codex::frontend::generated;

    const generated::MethodMetadata* metadataFor(generated::MethodId id) noexcept {
        const auto found = std::find_if(generated::AllMethods.begin(), generated::AllMethods.end(), [id](const auto& metadata) {
            return metadata.id == id;
        });
        return found == generated::AllMethods.end() ? nullptr : &*found;
    }

    const generated::MethodMetadata* metadataForRegistryKey(std::string_view registryKey, std::size_t& matches) noexcept {
        const generated::MethodMetadata* found = nullptr;
        matches = 0;
        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            if (std::find(metadata.registryKeys.begin(), metadata.registryKeys.end(), registryKey) != metadata.registryKeys.end()) {
                found = &metadata;
                ++matches;
            }
        }
        return found;
    }

    std::string frontendMethodForProviderMethod(std::string_view providerMethod) {
        if (providerMethod == "thread/inject_items") {
            return "thread.injectItems";
        }
        std::string result{providerMethod};
        std::replace(result.begin(), result.end(), '/', '.');
        return result;
    }

    std::string clientRequestRegistryKey(std::string_view providerMethod) {
        return "client_request:ClientRequest:method:" + std::string{providerMethod};
    }

    // This converter is deliberately confined to the contract test. It
    // constructs an actual BackendCommand alternative so generated string
    // metadata cannot claim a mapping to a C++ command type that does not
    // exist. It is not reachable from the production adapter dispatcher.
    std::optional<backend::BackendCommand> contractCommandTarget(const generated::MethodMetadata& metadata) {
#define CODEX_BACKEND_PROVIDER_OPERATION(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)                                   \
    if (metadata.backendCommand == #COMMAND) {                                                                                             \
        return backend::BackendCommand{backend::COMMAND{}};                                                                                \
    }
#define CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)                             \
    CODEX_BACKEND_PROVIDER_OPERATION(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)
#include "ai/openai/codex/backend/internal/ProviderOperations.inc"
#undef CODEX_BACKEND_PROVIDER_OPERATION_EMPTY
#undef CODEX_BACKEND_PROVIDER_OPERATION

        if (metadata.backendCommand == "ApprovalRespond") {
            return backend::BackendCommand{backend::ApprovalRespond{}};
        }
        if (metadata.backendCommand == "UserInputRespond") {
            return backend::BackendCommand{backend::UserInputRespond{}};
        }
        if (metadata.backendCommand == "AuthenticationRespond") {
            return backend::BackendCommand{backend::AuthenticationRespond{}};
        }
        if (metadata.backendCommand == "UnknownRequestRespondRaw") {
            return backend::BackendCommand{backend::UnknownRequestRespondRaw{}};
        }
        if (metadata.backendCommand == "UnknownRequestReject") {
            return backend::BackendCommand{backend::UnknownRequestReject{}};
        }
        if (metadata.backendCommand == "ApplyPatchApprovalRespond") {
            return backend::BackendCommand{backend::ApplyPatchApprovalRespond{}};
        }
        if (metadata.backendCommand == "AttestationGenerateRespond") {
            return backend::BackendCommand{backend::AttestationGenerateRespond{}};
        }
        if (metadata.backendCommand == "DynamicToolCallRespond") {
            return backend::BackendCommand{backend::DynamicToolCallRespond{}};
        }
        if (metadata.backendCommand == "ExecCommandApprovalRespond") {
            return backend::BackendCommand{backend::ExecCommandApprovalRespond{}};
        }
        if (metadata.backendCommand == "KnownRequestReject") {
            return backend::BackendCommand{backend::KnownRequestReject{}};
        }
        if (metadata.backendCommand == "McpServerElicitationRespond") {
            return backend::BackendCommand{backend::McpServerElicitationRespond{}};
        }
        if (metadata.backendCommand == "PermissionsApprovalRespond") {
            return backend::BackendCommand{backend::PermissionsApprovalRespond{}};
        }
        return std::nullopt;
    }

    bool exactRegistryKeys(const generated::MethodMetadata& metadata, std::initializer_list<std::string_view> expected) {
        if (metadata.registryKeys.size() != expected.size()) {
            return false;
        }
        return std::all_of(expected.begin(), expected.end(), [&metadata](std::string_view key) {
            return std::count(metadata.registryKeys.begin(), metadata.registryKeys.end(), key) == 1;
        });
    }

    void testProviderTargets(tests::support::TestResult& result) {
        std::size_t providerCount = 0;
        bool exactMappings = true;
        std::set<std::string> frontendMethods;
        std::set<std::string> registryKeys;
        std::set<std::string> backendCommands;

#define CODEX_BACKEND_PROVIDER_OPERATION(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)                                   \
    do {                                                                                                                                   \
        ++providerCount;                                                                                                                   \
        const std::string registryKey = clientRequestRegistryKey(WIRE_METHOD);                                                             \
        std::size_t matches = 0;                                                                                                           \
        const generated::MethodMetadata* metadata = metadataForRegistryKey(registryKey, matches);                                          \
        const std::optional<backend::BackendCommand> target = metadata ? contractCommandTarget(*metadata) : std::nullopt;                  \
        exactMappings = exactMappings && matches == 1 && metadata != nullptr &&                                                            \
                        metadata->category == generated::MethodCategory::ProviderOperation && !metadata->frontendNative &&                 \
                        metadata->registryKeys.size() == 1 && metadata->registryKeys.front() == registryKey &&                             \
                        metadata->method == frontendMethodForProviderMethod(WIRE_METHOD) && metadata->backendCommand == #COMMAND &&        \
                        target.has_value() && std::holds_alternative<backend::COMMAND>(*target);                                           \
        if (metadata != nullptr) {                                                                                                         \
            frontendMethods.emplace(metadata->method);                                                                                     \
            backendCommands.emplace(metadata->backendCommand);                                                                             \
        }                                                                                                                                  \
        registryKeys.emplace(std::move(registryKey));                                                                                      \
    } while (false);
#define CODEX_BACKEND_PROVIDER_OPERATION_EMPTY(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)                             \
    CODEX_BACKEND_PROVIDER_OPERATION(COMMAND, RESULT, DOMAIN, METHOD, ACCESS, STATEFUL, WIRE_METHOD)
#include "ai/openai/codex/backend/internal/ProviderOperations.inc"
#undef CODEX_BACKEND_PROVIDER_OPERATION_EMPTY
#undef CODEX_BACKEND_PROVIDER_OPERATION

        result.expectTrue(exactMappings && providerCount == 86 && frontendMethods.size() == 86 && registryKeys.size() == 86 &&
                              backendCommands.size() == 86,
                          "all 86 generated provider methods map bijectively to their actual compiled BackendCommand alternatives");
    }

    template <typename Command>
    bool exactReverseTarget(generated::MethodId id,
                            std::string_view frontendMethod,
                            std::string_view commandName,
                            std::initializer_list<std::string_view> registryKeys) {
        const generated::MethodMetadata* metadata = metadataFor(id);
        if (metadata == nullptr || metadata->category != generated::MethodCategory::ReverseResponse || metadata->frontendNative ||
            metadata->method != frontendMethod || metadata->backendCommand != commandName || !exactRegistryKeys(*metadata, registryKeys)) {
            return false;
        }
        const std::optional<backend::BackendCommand> target = contractCommandTarget(*metadata);
        return target.has_value() && std::holds_alternative<Command>(*target);
    }

    void testReverseTargets(tests::support::TestResult& result) {
        const bool exactMappings =
            exactReverseTarget<backend::ApprovalRespond>(generated::MethodId::ApprovalRespond,
                                                         "request.approval.respond",
                                                         "ApprovalRespond",
                                                         {"server_request:ServerRequest:method:item/commandExecution/requestApproval",
                                                          "server_request:ServerRequest:method:item/fileChange/requestApproval"}) &&
            exactReverseTarget<backend::UserInputRespond>(generated::MethodId::UserInputRespond,
                                                          "request.userInput.respond",
                                                          "UserInputRespond",
                                                          {"server_request:ServerRequest:method:item/tool/requestUserInput"}) &&
            exactReverseTarget<backend::AuthenticationRespond>(generated::MethodId::AuthenticationRespond,
                                                               "request.authentication.respond",
                                                               "AuthenticationRespond",
                                                               {"server_request:ServerRequest:method:account/chatgptAuthTokens/refresh"}) &&
            exactReverseTarget<backend::UnknownRequestRespondRaw>(
                generated::MethodId::UnknownRequestRespond, "request.unknown.respond", "UnknownRequestRespondRaw", {}) &&
            exactReverseTarget<backend::UnknownRequestReject>(
                generated::MethodId::UnknownRequestReject, "request.unknown.reject", "UnknownRequestReject", {}) &&
            exactReverseTarget<backend::ApplyPatchApprovalRespond>(generated::MethodId::ApplyPatchApprovalRespond,
                                                                   "request.applyPatchApproval.respond",
                                                                   "ApplyPatchApprovalRespond",
                                                                   {"server_request:ServerRequest:method:applyPatchApproval"}) &&
            exactReverseTarget<backend::AttestationGenerateRespond>(generated::MethodId::AttestationRespond,
                                                                    "request.attestation.respond",
                                                                    "AttestationGenerateRespond",
                                                                    {"server_request:ServerRequest:method:attestation/generate"}) &&
            exactReverseTarget<backend::DynamicToolCallRespond>(generated::MethodId::DynamicToolRespond,
                                                                "request.dynamicTool.respond",
                                                                "DynamicToolCallRespond",
                                                                {"server_request:ServerRequest:method:item/tool/call"}) &&
            exactReverseTarget<backend::ExecCommandApprovalRespond>(generated::MethodId::ExecCommandApprovalRespond,
                                                                    "request.execCommandApproval.respond",
                                                                    "ExecCommandApprovalRespond",
                                                                    {"server_request:ServerRequest:method:execCommandApproval"}) &&
            exactReverseTarget<backend::KnownRequestReject>(generated::MethodId::KnownRequestReject,
                                                            "request.known.reject",
                                                            "KnownRequestReject",
                                                            {"server_request:ServerRequest:method:item/tool/requestUserInput",
                                                             "server_request:ServerRequest:method:attestation/generate",
                                                             "server_request:ServerRequest:method:item/tool/call",
                                                             "server_request:ServerRequest:method:mcpServer/elicitation/request"}) &&
            exactReverseTarget<backend::McpServerElicitationRespond>(
                generated::MethodId::McpElicitationRespond,
                "request.mcpElicitation.respond",
                "McpServerElicitationRespond",
                {"server_request:ServerRequest:method:mcpServer/elicitation/request"}) &&
            exactReverseTarget<backend::PermissionsApprovalRespond>(
                generated::MethodId::PermissionsApprovalRespond,
                "request.permissionsApproval.respond",
                "PermissionsApprovalRespond",
                {"server_request:ServerRequest:method:item/permissions/requestApproval"});

        result.expectTrue(exactMappings,
                          "all 12 reverse frontend methods map to their actual compiled BackendCommand alternatives and exact request "
                          "cardinalities");
    }

    void testMappingBoundary(tests::support::TestResult& result) {
        std::size_t mapped = 0;
        bool exactBoundary = true;
        for (const generated::MethodMetadata& metadata : generated::AllMethods) {
            const bool expected = metadata.category == generated::MethodCategory::ProviderOperation ||
                                  metadata.category == generated::MethodCategory::ReverseResponse;
            const bool present = contractCommandTarget(metadata).has_value();
            mapped += present ? 1U : 0U;
            exactBoundary = exactBoundary && present == expected;
        }
        result.expectTrue(exactBoundary && mapped == 98,
                          "the pure target converter covers exactly 86 provider plus 12 reverse methods and excludes frontend-native "
                          "lifecycle/replay controls");
    }
} // namespace

int main() {
    tests::support::TestResult result;

    testProviderTargets(result);
    testReverseTargets(result);
    testMappingBoundary(result);

    return result.processResult();
}
