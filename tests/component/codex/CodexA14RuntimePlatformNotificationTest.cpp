/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/detail/EventDecoder.h"
#include "ai/openai/codex/detail/RuntimePlatformCodec.h"
#include "ai/openai/codex/typed/Events.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "support/TestResult.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {
    namespace codex = ai::openai::codex;
    namespace detail = ai::openai::codex::detail;
    namespace typed = ai::openai::codex::typed;

    static_assert(std::variant_size_v<typed::CanonicalServerNotification> == 67);
    static_assert(
        std::is_same_v<std::variant_alternative_t<58, typed::CanonicalServerNotification>, typed::McpServerStatusUpdatedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<59, typed::CanonicalServerNotification>, typed::DeprecationNoticeNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<60, typed::CanonicalServerNotification>, typed::ProcessExitedNotification>);
    static_assert(
        std::is_same_v<std::variant_alternative_t<61, typed::CanonicalServerNotification>, typed::ProcessOutputDeltaNotification>);
    static_assert(
        std::is_same_v<std::variant_alternative_t<62, typed::CanonicalServerNotification>, typed::RemoteControlStatusChangedNotification>);
    static_assert(
        std::is_same_v<std::variant_alternative_t<63, typed::CanonicalServerNotification>, typed::ServerRequestResolvedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<64, typed::CanonicalServerNotification>, typed::WarningNotification>);
    static_assert(
        std::is_same_v<std::variant_alternative_t<65, typed::CanonicalServerNotification>, typed::WindowsWorldWritableWarningNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<66, typed::CanonicalServerNotification>,
                                 typed::WindowsSandboxSetupCompletedNotification>);

    static_assert(std::variant_size_v<typed::Event> == 69);
    static_assert(std::is_same_v<std::variant_alternative_t<60, typed::Event>, typed::McpServerStatusUpdatedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<61, typed::Event>, typed::DeprecationNoticeNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<62, typed::Event>, typed::ProcessExitedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<63, typed::Event>, typed::ProcessOutputDeltaNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<64, typed::Event>, typed::RemoteControlStatusChangedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<65, typed::Event>, typed::ServerRequestResolvedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<66, typed::Event>, typed::WarningNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<67, typed::Event>, typed::WindowsWorldWritableWarningNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<68, typed::Event>, typed::WindowsSandboxSetupCompletedNotification>);

    static_assert(std::variant_size_v<typed::TypedServerRequest> == 11);
    static_assert(std::is_same_v<std::variant_alternative_t<2, typed::TypedServerRequest>, typed::UserInputRequest>);
    static_assert(std::is_same_v<std::variant_alternative_t<4, typed::TypedServerRequest>, typed::UnknownServerRequest>);
    static_assert(std::is_same_v<std::variant_alternative_t<8, typed::TypedServerRequest>, typed::AttestationGenerateRequest>);
    static_assert(std::is_same_v<std::variant_alternative_t<9, typed::TypedServerRequest>, typed::DynamicToolCallRequest>);
    static_assert(std::is_same_v<std::variant_alternative_t<10, typed::TypedServerRequest>, typed::McpServerElicitationRequest>);

    codex::Notification notification(std::string method, codex::Json params) {
        codex::Json raw{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}, {"futureEnvelope", true}};
        return {std::move(method), std::move(params), std::move(raw)};
    }

    bool malformedAt(const typed::Event& event, const codex::Notification& wire, std::string_view path) {
        const auto* unknown = std::get_if<typed::UnknownEvent>(&event);
        return unknown != nullptr && unknown->method == wire.method && unknown->params == wire.params && unknown->raw == wire.raw &&
               unknown->diagnostic && unknown->diagnostic->kind == typed::DecodeIssueKind::MalformedKnownPayload &&
               unknown->diagnostic->severity == typed::DecodeIssueSeverity::ProtocolWarning && unknown->diagnostic->fieldPath == path;
    }

    struct NotificationCase {
        const char* method;
        codex::Json params;
        const char* requiredField;
        std::size_t eventIndex;
    };

    std::array<NotificationCase, 8> cases() {
        return {{{"deprecationNotice", {{"details", nullptr}, {"summary", "synthetic deprecation"}, {"future", true}}, "summary", 61},
                 {"process/exited",
                  {{"exitCode", std::numeric_limits<std::int32_t>::min()},
                   {"processHandle", "synthetic-handle"},
                   {"stderr", ""},
                   {"stderrCapReached", false},
                   {"stdout", ""},
                   {"stdoutCapReached", true},
                   {"future", true}},
                  "exitCode",
                  62},
                 {"process/outputDelta",
                  {{"capReached", false},
                   {"deltaBase64", "c3ludGhldGlj"},
                   {"processHandle", "synthetic-handle"},
                   {"stream", "stdout"},
                   {"future", true}},
                  "deltaBase64",
                  63},
                 {"remoteControl/status/changed",
                  {{"environmentId", nullptr},
                   {"installationId", "synthetic-installation"},
                   {"serverName", "synthetic-server"},
                   {"status", "connected"},
                   {"future", true}},
                  "installationId",
                  64},
                 {"serverRequest/resolved",
                  {{"requestId", std::numeric_limits<std::int64_t>::min()}, {"threadId", "synthetic-thread"}, {"future", true}},
                  "requestId",
                  65},
                 {"warning", {{"message", "synthetic warning"}, {"threadId", nullptr}, {"future", true}}, "message", 66},
                 {"windows/worldWritableWarning",
                  {{"extraCount", std::numeric_limits<std::uint64_t>::max()},
                   {"failedScan", false},
                   {"samplePaths", codex::Json::array({"C:/synthetic"})},
                   {"future", true}},
                  "samplePaths",
                  67},
                 {"windowsSandbox/setupCompleted",
                  {{"error", nullptr}, {"mode", "unelevated"}, {"success", true}, {"future", true}},
                  "mode",
                  68}}};
    }

    void testTable(tests::support::TestResult& result) {
        for (const auto& item : cases()) {
            const codex::Notification wire = notification(item.method, item.params);
            const typed::Event decoded = detail::decodeEvent(wire);
            result.expectTrue(decoded.index() == item.eventIndex && std::visit(
                                                                        [](const auto& value) {
                                                                            return value.raw.is_object();
                                                                        },
                                                                        decoded),
                              std::string(item.method) + " decodes to its exact append-only Event alternative");

            codex::Notification malformed = wire;
            malformed.params.erase(item.requiredField);
            malformed.raw["params"].erase(item.requiredField);
            result.expectTrue(malformedAt(detail::decodeEvent(malformed), malformed, std::string("$.params.") + item.requiredField),
                              std::string(item.method) + " rejects a missing required field with raw preservation");
        }
    }

    void testPresenceEnumsAndIds(tests::support::TestResult& result) {
        std::string error = "stale";
        const auto deprecation =
            detail::decodeDeprecationNoticeNotification(notification("deprecationNotice", {{"summary", "synthetic"}}), error);
        const auto remote = detail::decodeRemoteControlStatusChangedNotification(notification("remoteControl/status/changed",
                                                                                              {{"environmentId", "synthetic-env"},
                                                                                               {"installationId", "synthetic-installation"},
                                                                                               {"serverName", "synthetic-server"},
                                                                                               {"status", "future-status"}}),
                                                                                 error);
        const auto warning =
            detail::decodeWarningNotification(notification("warning", {{"message", "synthetic"}, {"threadId", "synthetic-thread"}}), error);
        const auto setup = detail::decodeWindowsSandboxSetupCompletedNotification(
            notification("windowsSandbox/setupCompleted", {{"mode", "future-mode"}, {"success", false}}), error);
        const auto stringId = detail::decodeServerRequestResolvedNotification(
            notification("serverRequest/resolved", {{"requestId", "synthetic-request"}, {"threadId", "synthetic-thread"}}), error);

        result.expectTrue(deprecation && !deprecation->details.present && remote && remote->environmentId.hasValue() &&
                              remote->status.value == "future-status" && !remote->diagnostics.empty() && warning &&
                              warning->threadId.hasValue() && setup && !setup->error.present && setup->mode.value == "future-mode" &&
                              !setup->diagnostics.empty() && stringId && std::holds_alternative<std::string>(stringId->requestId.value()) &&
                              error.empty(),
                          "optional omission/value states, open enums, and string request IDs remain exact and nonfatal");

        const auto futureStream = detail::decodeProcessOutputDeltaNotification(
            notification("process/outputDelta",
                         {{"capReached", true}, {"deltaBase64", ""}, {"processHandle", "synthetic-handle"}, {"stream", "future-stream"}}),
            error);
        result.expectTrue(futureStream && futureStream->stream.value == "future-stream" && !futureStream->diagnostics.empty() &&
                              futureStream->diagnostics.front().severity == typed::DecodeIssueSeverity::ForwardCompatibility,
                          "future process stream values are retained with a forward-compatibility diagnostic");

        const auto badId = detail::decodeServerRequestResolvedNotification(
            notification("serverRequest/resolved", {{"requestId", true}, {"threadId", "synthetic-thread"}}), error);
        result.expectTrue(!badId && error.find("$.params.requestId") != std::string::npos &&
                              error.find("synthetic-thread") == std::string::npos,
                          "resolved-request diagnostics expose only safe structural information");
    }

} // namespace

int main() {
    tests::support::TestResult result;
    testTable(result);
    testPresenceEnumsAndIds(result);
    return result.processResult();
}
