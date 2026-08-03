/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include <ai/openai/codex/backend/BackendCore.h>
#include <ai/openai/codex/frontend/Codec.h>
#include <ai/openai/codex/frontend/GeneratedProtocol.h>
#include <ai/openai/codex/frontend/Messages.h>
#include <ai/openai/codex/frontend/Protocol.h>
#include <ai/openai/codex/frontend/Security.h>
#include <ai/openai/codex/stdio/Client.h>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

namespace {
    namespace frontend = ai::openai::codex::frontend;
    namespace generated = ai::openai::codex::frontend::generated;

    bool exerciseInstalledFrontendContract() {
        const generated::DefinedCommand command{
            "installed-contract",
            generated::makeParameters(generated::MethodId::SnapshotGet, frontend::Json::object()),
        };
        const auto encodedCommand = frontend::Codec::encodeDefinedCommand(command);
        if (!encodedCommand) {
            return false;
        }
        const auto decodedCommand = frontend::Codec::decodeDefinedCommand(encodedCommand.value());
        if (!decodedCommand || generated::commandMethod(decodedCommand.value().parameters) != generated::MethodId::SnapshotGet) {
            return false;
        }

        const generated::CompleteCommandResult commandResult =
            generated::makeResult(generated::MethodId::SnapshotGet, {{"sequence", 0}, {"installed", true}});
        const auto encodedResult = frontend::Codec::encodeDefinedResult(commandResult);
        if (!encodedResult) {
            return false;
        }
        const auto decodedResult = frontend::Codec::decodeDefinedResult(generated::MethodId::SnapshotGet, encodedResult.value());

        frontend::ExpandedBackendSnapshotState state;
        state.provider = {
            {"lifecycle", "stopped"},
            {"generation", 0},
            {"desiredRunning", false},
            {"recovery", {{"status", "idle"}, {"attempts", 0}}},
        };
        state.controller = {{"present", false}};
        state.truncation = {{"truncated", false}};
        const frontend::ExpandedSnapshot snapshot{frontend::SequenceNumber{0}, std::move(state)};
        const auto encodedSnapshot = frontend::Codec::encodeExpandedSnapshot(snapshot);
        if (!encodedSnapshot) {
            return false;
        }
        const auto decodedSnapshot = frontend::Codec::decodeExpandedSnapshot(encodedSnapshot.value());

        const frontend::CapabilityAdvertisement capabilities{
            {frontend::FrontendCapability::MethodDiscovery, frontend::FrontendCapability::SecurityScopes},
            {},
            {},
        };
        return decodedResult.hasValue() && generated::commandMethod(decodedResult.value()) == generated::MethodId::SnapshotGet &&
               decodedSnapshot.hasValue() && decodedSnapshot.value() == snapshot && capabilities.defined.size() == 2 &&
               frontend::frontendScopeFromString("observe") == frontend::FrontendScope::Observe;
    }
} // namespace

int main() {
    using ai::openai::codex::backend::BackendCore;
    using namespace ai::openai::codex::frontend;

    static_assert(ProtocolVersion == std::uint32_t{1});
    static_assert(ProtocolIdentity == std::string_view{"snodec.codex-frontend"});
    static_assert(generated::MethodCount == 105);
    static_assert(generated::ExistingMethodCount == 15);
    static_assert(std::variant_size_v<generated::CompleteCommandParameters> == std::size_t{105});
    static_assert(std::variant_size_v<generated::CompleteCommandResult> == std::size_t{105});
    static_assert(generated::AllNotificationProjections.size() == std::size_t{68});
    static_assert(generated::AllThreadItemProjections.size() == std::size_t{18});
    static_assert(DefaultRemoteScopes.size() == 2);
    static_assert(LocalTrustedScopes.size() == 12);
    static_assert(method::ThreadStart == std::string_view{"thread.start"});

    BackendCore<ai::openai::codex::stdio::Client> backend;
    const auto decoded = Codec::decodeClient(std::string_view{"{}"});
    return backend.isReady() || decoded.hasValue() || !exerciseInstalledFrontendContract() ? 1 : 0;
}
