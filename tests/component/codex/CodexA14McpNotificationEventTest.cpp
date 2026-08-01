/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/detail/EventDecoder.h"
#include "ai/openai/codex/detail/McpCodec.h"
#include "ai/openai/codex/typed/Events.h"
#include "ai/openai/codex/typed/Mcp.h"
#include "support/TestResult.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {
    namespace codex = ai::openai::codex;
    namespace detail = ai::openai::codex::detail;
    namespace typed = ai::openai::codex::typed;

    static_assert(std::variant_size_v<typed::CanonicalServerNotification> == 68);
    static_assert(std::is_same_v<std::variant_alternative_t<56, typed::CanonicalServerNotification>, typed::SkillsChangedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<57, typed::CanonicalServerNotification>,
                                 typed::McpServerOauthLoginCompletedNotification>);
    static_assert(
        std::is_same_v<std::variant_alternative_t<58, typed::CanonicalServerNotification>, typed::McpServerStatusUpdatedNotification>);

    static_assert(std::variant_size_v<typed::Event> == 69);
    static_assert(std::is_same_v<std::variant_alternative_t<58, typed::Event>, typed::SkillsChangedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<59, typed::Event>, typed::McpServerOauthLoginCompletedNotification>);
    static_assert(std::is_same_v<std::variant_alternative_t<60, typed::Event>, typed::McpServerStatusUpdatedNotification>);

    codex::Notification notification(std::string method, codex::Json params) {
        codex::Json raw{
            {"jsonrpc", "2.0"},
            {"method", method},
            {"params", params},
            {"futureEnvelopeField", true},
        };
        return {std::move(method), std::move(params), std::move(raw)};
    }

    bool malformedEventAt(const typed::Event& event, const codex::Notification& wire, std::string_view path) {
        const auto* unknown = std::get_if<typed::UnknownEvent>(&event);
        return unknown != nullptr && unknown->method == wire.method && unknown->params == wire.params && unknown->raw == wire.raw &&
               unknown->decodingError.has_value() && unknown->diagnostic.has_value() &&
               unknown->diagnostic->kind == typed::DecodeIssueKind::MalformedKnownPayload &&
               unknown->diagnostic->severity == typed::DecodeIssueSeverity::ProtocolWarning &&
               unknown->diagnostic->surface == wire.method && unknown->diagnostic->fieldPath == path;
    }

    void testOauthLoginCompleted(tests::support::TestResult& result) {
        const codex::Notification wire = notification("mcpServer/oauthLogin/completed",
                                                      {
                                                          {"error", nullptr},
                                                          {"name", "synthetic-server"},
                                                          {"success", true},
                                                          {"threadId", "synthetic-thread"},
                                                          {"futureParam", {{"safe", true}}},
                                                      });

        std::string error = "stale";
        const auto decoded = detail::decodeMcpServerOauthLoginCompletedNotification(wire, error);
        const typed::Event event = detail::decodeEvent(wire);
        const auto* alternative = std::get_if<typed::McpServerOauthLoginCompletedNotification>(&event);
        result.expectTrue(decoded && decoded->error.present && !decoded->error.value && decoded->name == "synthetic-server" &&
                              decoded->success && decoded->threadId.present && decoded->threadId.value &&
                              decoded->threadId.value->value == "synthetic-thread" && decoded->raw == wire.raw && error.empty() &&
                              alternative != nullptr && event.index() == 59 && alternative->raw == wire.raw,
                          "mcpServer/oauthLogin/completed decodes every stable field and retains the raw envelope at Event index 59");

        codex::Notification malformed = wire;
        malformed.params.erase("name");
        malformed.raw["params"].erase("name");
        result.expectTrue(malformedEventAt(detail::decodeEvent(malformed), malformed, "$.params.name"),
                          "malformed known OAuth completion degrades nonfatally with exact raw preservation and field path");
    }

    void testStartupStatusUpdated(tests::support::TestResult& result) {
        const codex::Notification wire = notification("mcpServer/startupStatus/updated",
                                                      {
                                                          {"failureReason", nullptr},
                                                          {"name", "synthetic-server"},
                                                          {"status", "ready"},
                                                          {"threadId", nullptr},
                                                          {"futureParam", codex::Json::array({"retained"})},
                                                      });

        std::string error = "stale";
        const auto decoded = detail::decodeMcpServerStatusUpdatedNotification(wire, error);
        const typed::Event event = detail::decodeEvent(wire);
        const auto* alternative = std::get_if<typed::McpServerStatusUpdatedNotification>(&event);
        result.expectTrue(decoded && !decoded->error.present && decoded->failureReason.present && !decoded->failureReason.value &&
                              decoded->name == "synthetic-server" && decoded->status == typed::McpServerStartupState::ready() &&
                              decoded->threadId.present && !decoded->threadId.value && decoded->raw == wire.raw && error.empty() &&
                              alternative != nullptr && event.index() == 60 && alternative->raw == wire.raw,
                          "mcpServer/startupStatus/updated preserves omission, explicit null, future fields, and Event index 60");

        const codex::Notification future = notification("mcpServer/startupStatus/updated",
                                                        {
                                                            {"failureReason", "future-reason"},
                                                            {"name", "synthetic-server"},
                                                            {"status", "future-state"},
                                                        });
        const auto futureDecoded = detail::decodeMcpServerStatusUpdatedNotification(future, error);
        result.expectTrue(futureDecoded && futureDecoded->failureReason.hasValue() &&
                              futureDecoded->failureReason->value == "future-reason" && futureDecoded->status.value == "future-state" &&
                              futureDecoded->diagnostics.size() == 2 &&
                              futureDecoded->diagnostics[0].severity == typed::DecodeIssueSeverity::ForwardCompatibility &&
                              futureDecoded->diagnostics[1].severity == typed::DecodeIssueSeverity::ForwardCompatibility && error.empty(),
                          "unknown startup state and failure reason values remain typed, nonfatal, and diagnostic");

        codex::Notification malformed = wire;
        malformed.params["status"] = false;
        malformed.raw["params"]["status"] = false;
        result.expectTrue(malformedEventAt(detail::decodeEvent(malformed), malformed, "$.params.status"),
                          "malformed known startup status degrades nonfatally with exact raw preservation and field path");
    }

} // namespace

int main() {
    tests::support::TestResult result;
    testOauthLoginCompleted(result);
    testStartupStatusUpdated(result);
    return result.processResult();
}
