/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/ClientOperationCodec.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "ai/openai/codex/detail/WindowsSandboxCodec.h"
#include "ai/openai/codex/typed/Client.h"
#include "ai/openai/codex/typed/WindowsSandbox.h"
#include "support/TestResult.h"

#include <string>
#include <type_traits>
#include <variant>

namespace {
    namespace codex = ai::openai::codex;
    namespace detail = ai::openai::codex::detail;
    namespace typed = ai::openai::codex::typed;

    void testSetupEncoding(tests::support::TestResult& result) {
        std::string error = "stale";
        typed::WindowsSandboxSetupStartParams params{};
        params.cwd = typed::OptionalNullable<typed::AbsolutePathBuf>::explicitNull();
        params.mode = typed::WindowsSandboxSetupMode::elevated();
        params.raw = {{"cwd", "stale"}, {"mode", "stale"}, {"futureField", codex::Json{{"retained", true}}}};

        const auto encoded = detail::encodeWindowsSandboxSetupStartParams(params, error);
        result.expectTrue(encoded == codex::Json{{"cwd", nullptr}, {"futureField", {{"retained", true}}}, {"mode", "elevated"}} &&
                              error.empty(),
                          "windowsSandbox/setupStart encodes exact stable fields and retains open future fields");

        params.cwd = typed::OptionalNullable<typed::AbsolutePathBuf>::omitted();
        params.mode = typed::WindowsSandboxSetupMode::unelevated();
        const auto omitted = detail::encodeWindowsSandboxSetupStartParams(params, error);
        result.expectTrue(omitted && !omitted->contains("cwd") && omitted->at("mode") == "unelevated" &&
                              omitted->at("futureField") == codex::Json{{"retained", true}} && error.empty(),
                          "windowsSandbox/setupStart preserves omitted cwd separately from explicit null");

        params.raw = codex::Json::array();
        result.expectTrue(!detail::encodeWindowsSandboxSetupStartParams(params, error) && error.find("$.raw") != std::string::npos,
                          "Windows sandbox setup rejects non-object raw state locally");
    }

    void testResultDecoding(tests::support::TestResult& result) {
        std::string error = "stale";
        const codex::Json readinessWire{{"status", "ready"}, {"futureField", true}};
        const auto readiness = detail::decodeWindowsSandboxReadinessResponse(readinessWire, error);
        result.expectTrue(readiness && readiness->status == typed::WindowsSandboxReadiness::ready() && readiness->raw == readinessWire &&
                              readiness->diagnostics.empty() && error.empty(),
                          "Windows sandbox readiness decodes its concrete response and retains future fields");

        const codex::Json futureReadinessWire{{"status", "futureStatus"}};
        const auto futureReadiness = detail::decodeWindowsSandboxReadinessResponse(futureReadinessWire, error);
        result.expectTrue(futureReadiness && !futureReadiness->status.isKnown() && futureReadiness->diagnostics.size() == 1 &&
                              futureReadiness->diagnostics.front().kind == typed::DecodeIssueKind::UnknownEnumValue &&
                              futureReadiness->diagnostics.front().severity == typed::DecodeIssueSeverity::ForwardCompatibility &&
                              futureReadiness->diagnostics.front().fieldPath == "$.status" && error.empty(),
                          "Windows sandbox readiness retains unknown future enum values nonfatally");

        const codex::Json setupWire{{"started", true}, {"futureField", {"retained", true}}};
        const auto setup = detail::decodeWindowsSandboxSetupStartResponse(setupWire, error);
        result.expectTrue(setup && setup->started && setup->raw == setupWire && error.empty(),
                          "Windows sandbox setup decodes its concrete response and retains future fields");

        const auto readinessOperation =
            detail::decodeClientOperationResult(detail::ClientRequestTarget::WindowsSandboxReadiness, readinessWire);
        const auto setupOperation = detail::decodeClientOperationResult(detail::ClientRequestTarget::WindowsSandboxSetupStart, setupWire);
        result.expectTrue(readinessOperation && std::holds_alternative<typed::WindowsSandboxReadinessResponse>(*readinessOperation.value) &&
                              setupOperation && std::holds_alternative<typed::WindowsSandboxSetupStartResponse>(*setupOperation.value),
                          "Windows sandbox operation results retain exact target/result association");

        const codex::Json malformed{{"status", {"sensitive", "not echoed"}}};
        result.expectTrue(!detail::decodeWindowsSandboxReadinessResponse(malformed, error) && error.find("$.status") != std::string::npos &&
                              error.find("not echoed") == std::string::npos,
                          "malformed readiness results report only a safe structural path");
    }

    void testRegistryAndFacade(tests::support::TestResult& result) {
        using WindowsSandboxAccessor = typed::WindowsSandbox& (typed::Client::*) () noexcept;
        using ConstWindowsSandboxAccessor = const typed::WindowsSandbox& (typed::Client::*) () const noexcept;
        static_assert(
            std::is_same_v<decltype(static_cast<WindowsSandboxAccessor>(&typed::Client::windowsSandbox)), WindowsSandboxAccessor>);
        static_assert(std::is_same_v<decltype(static_cast<ConstWindowsSandboxAccessor>(&typed::Client::windowsSandbox)),
                                     ConstWindowsSandboxAccessor>);
        static_assert(std::is_same_v<decltype(&typed::WindowsSandbox::checkReadiness),
                                     typed::WindowsSandbox::Submission (typed::WindowsSandbox::*)(
                                         typed::WindowsSandbox::CheckReadinessResultHandler)>);
        static_assert(std::is_same_v<decltype(&typed::WindowsSandbox::startSetup),
                                     typed::WindowsSandbox::Submission (typed::WindowsSandbox::*)(
                                         typed::WindowsSandboxSetupStartParams, typed::WindowsSandbox::StartSetupResultHandler)>);

        const detail::ProtocolSurfaceEntry& readiness = detail::entryFor(detail::ClientRequestTarget::WindowsSandboxReadiness);
        const detail::ProtocolSurfaceEntry& setup = detail::entryFor(detail::ClientRequestTarget::WindowsSandboxSetupStart);
        result.expectTrue(readiness.key.name == "windowsSandbox/readiness" && setup.key.name == "windowsSandbox/setupStart" &&
                              readiness.operationContract.parameterTypeIdentity == "Unit" &&
                              readiness.operationContract.resultTypeIdentity == "WindowsSandboxReadinessResponse" &&
                              setup.operationContract.parameterTypeIdentity == "WindowsSandboxSetupStartParams" &&
                              setup.operationContract.resultTypeIdentity == "WindowsSandboxSetupStartResponse" &&
                              readiness.typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                              setup.typedSchemaStatus == detail::TypedSchemaStatus::Complete,
                          "WindowsSandbox facade methods resolve only through exact Complete registry targets");
    }

} // namespace

int main() {
    tests::support::TestResult result;
    testSetupEncoding(result);
    testResultDecoding(result);
    testRegistryAndFacade(result);
    return result.processResult();
}
