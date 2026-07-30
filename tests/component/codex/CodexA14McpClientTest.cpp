/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/ClientOperationCodec.h"
#include "ai/openai/codex/detail/McpCodec.h"
#include "ai/openai/codex/detail/ProtocolSurfaceRegistry.h"
#include "ai/openai/codex/typed/Client.h"
#include "ai/openai/codex/typed/Mcp.h"
#include "support/TestResult.h"

#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <variant>

namespace {
    namespace codex = ai::openai::codex;
    namespace detail = ai::openai::codex::detail;
    namespace typed = ai::openai::codex::typed;

    codex::Json statusValue(std::string authStatus = "oAuth") {
        return {
            {"authStatus", std::move(authStatus)},
            {"name", "synthetic-server"},
            {"resourceTemplates",
             codex::Json::array({{{"annotations", nullptr},
                                  {"description", "template"},
                                  {"mimeType", "text/plain"},
                                  {"name", "template-name"},
                                  {"title", "Template"},
                                  {"uriTemplate", "file:///{path}"},
                                  {"futureTemplateField", true}}})},
            {"resources",
             codex::Json::array({{{"_meta", {{"safe", true}}},
                                  {"annotations", nullptr},
                                  {"description", "resource"},
                                  {"icons", codex::Json::array({{{"src", "resource.svg"}}})},
                                  {"mimeType", "text/plain"},
                                  {"name", "resource-name"},
                                  {"size", std::numeric_limits<std::int64_t>::max()},
                                  {"title", "Resource"},
                                  {"uri", "file:///resource"},
                                  {"futureResourceField", true}}})},
            {"serverInfo",
             {{"description", "server"},
              {"icons", codex::Json::array({{{"src", "server.svg"}}})},
              {"name", "synthetic-server"},
              {"title", "Synthetic MCP"},
              {"version", "1.0"},
              {"websiteUrl", "https://example.invalid"},
              {"futureInfoField", true}}},
            {"tools",
             {{"tool-key",
               {{"_meta", nullptr},
                {"annotations", {{"audience", codex::Json::array({"assistant"})}}},
                {"description", "tool"},
                {"icons", codex::Json::array({{{"src", "tool.svg"}}})},
                {"inputSchema", nullptr},
                {"name", "tool-name"},
                {"outputSchema", {{"type", "object"}}},
                {"title", "Tool"},
                {"futureToolField", true}}}}},
            {"futureStatusField", {{"retained", true}}},
        };
    }

    void testRequestEncoders(tests::support::TestResult& result) {
        std::string error = "stale";

        typed::McpServerOauthLoginParams oauth{};
        oauth.name = "synthetic-server";
        oauth.scopes = typed::OptionalNullable<std::vector<std::string>>::withValue({"scope:a", "scope:b"});
        oauth.threadId = typed::OptionalNullable<typed::ThreadId>::explicitNull();
        oauth.timeoutSecs = typed::OptionalNullable<std::int64_t>::withValue(-1);
        oauth.raw = {{"futureOauthField", {{"retained", true}}}, {"name", "stale"}};
        const auto encodedOauth = detail::encodeMcpServerOauthLoginParams(oauth, error);
        result.expectTrue(encodedOauth ==
                                  codex::Json{
                                      {"futureOauthField", {{"retained", true}}},
                                      {"name", "synthetic-server"},
                                      {"scopes", codex::Json::array({"scope:a", "scope:b"})},
                                      {"threadId", nullptr},
                                      {"timeoutSecs", -1},
                                  } &&
                              error.empty(),
                          "mcpServer/oauth/login encodes exact stable fields while preserving open future fields");

        typed::McpResourceReadParams read{};
        read.server = "synthetic-server";
        read.threadId = typed::OptionalNullable<typed::ThreadId>::omitted();
        read.uri = "file:///resource";
        const auto encodedRead = detail::encodeMcpResourceReadParams(read, error);
        result.expectTrue(encodedRead == codex::Json{{"server", "synthetic-server"}, {"uri", "file:///resource"}} && error.empty(),
                          "mcpServer/resource/read preserves omission separately from explicit null");

        typed::McpServerToolCallParams call{};
        call.meta = typed::OptionalNullable<codex::Json>::explicitNull();
        call.arguments = typed::OptionalNullable<codex::Json>::withValue({{"opaque", codex::Json::array({1, true, nullptr})}});
        call.server = "synthetic-server";
        call.threadId = {"synthetic-thread"};
        call.tool = "tool-name";
        const auto encodedCall = detail::encodeMcpServerToolCallParams(call, error);
        result.expectTrue(encodedCall ==
                                  codex::Json{
                                      {"_meta", nullptr},
                                      {"arguments", {{"opaque", codex::Json::array({1, true, nullptr})}}},
                                      {"server", "synthetic-server"},
                                      {"threadId", "synthetic-thread"},
                                      {"tool", "tool-name"},
                                  } &&
                              error.empty(),
                          "mcpServer/tool/call preserves opaque arguments and explicit-null metadata exactly");

        typed::ListMcpServerStatusParams list{};
        list.cursor = typed::OptionalNullable<std::string>::explicitNull();
        list.detail = typed::OptionalNullable<typed::McpServerStatusDetail>::withValue(typed::McpServerStatusDetail::toolsAndAuthOnly());
        list.limit = typed::OptionalNullable<std::uint32_t>::withValue(std::numeric_limits<std::uint32_t>::max());
        list.threadId = typed::OptionalNullable<typed::ThreadId>::explicitNull();
        const auto encodedList = detail::encodeListMcpServerStatusParams(list, error);
        result.expectTrue(encodedList ==
                                  codex::Json{
                                      {"cursor", nullptr},
                                      {"detail", "toolsAndAuthOnly"},
                                      {"limit", std::numeric_limits<std::uint32_t>::max()},
                                      {"threadId", nullptr},
                                  } &&
                              error.empty(),
                          "mcpServerStatus/list preserves nulls, enum spelling, and the uint32 boundary");

        read.raw = codex::Json::array();
        result.expectTrue(!detail::encodeMcpResourceReadParams(read, error) && error.find("$.raw") != std::string::npos,
                          "MCP parameter encoders reject non-object raw state locally");
    }

    void testResultDecoders(tests::support::TestResult& result) {
        std::string error = "stale";

        const codex::Json oauthWire{
            {"authorizationUrl", "https://example.invalid/oauth"},
            {"futureOauthResponse", true},
        };
        const auto oauth = detail::decodeMcpServerOauthLoginResponse(oauthWire, error);
        result.expectTrue(oauth && oauth->authorizationUrl == "https://example.invalid/oauth" && oauth->raw == oauthWire && error.empty(),
                          "MCP OAuth result decodes its concrete response and retains future fields");

        const codex::Json resourceWire{
            {"contents",
             codex::Json::array({{{"_meta", nullptr},
                                  {"blob", "c3ludGhldGlj"},
                                  {"mimeType", "text/plain"},
                                  {"text", "synthetic"},
                                  {"uri", "file:///resource"},
                                  {"futureContentField", true}},
                                 {{"text", "text branch"}, {"blob", 42}, {"uri", "file:///text-branch"}},
                                 {{"blob", "YmxvYiBicmFuY2g="}, {"text", false}, {"uri", "file:///blob-branch"}}})},
            {"futureReadResponse", true},
        };
        const auto resource = detail::decodeMcpResourceReadResponse(resourceWire, error);
        result.expectTrue(resource && resource->contents.size() == 3 && resource->contents[0].blob == "c3ludGhldGlj" &&
                              resource->contents[0].text == "synthetic" && resource->contents[0].meta.present &&
                              !resource->contents[0].meta.value && resource->contents[0].raw["futureContentField"] == true &&
                              resource->contents[1].text == "text branch" && !resource->contents[1].blob &&
                              resource->contents[1].raw["blob"] == 42 && resource->contents[2].blob == "YmxvYiBicmFuY2g=" &&
                              !resource->contents[2].text && resource->contents[2].raw["text"] == false && resource->raw == resourceWire &&
                              error.empty(),
                          "MCP resource anyOf selects any valid branch while retaining alternate-name future properties");

        const codex::Json toolWire{
            {"_meta", {{"opaque", true}}},
            {"content", codex::Json::array({{{"type", "text"}, {"text", "opaque result"}}})},
            {"isError", false},
            {"structuredContent", nullptr},
            {"futureToolResponse", true},
        };
        const auto tool = detail::decodeMcpServerToolCallResponse(toolWire, error);
        result.expectTrue(tool && codex::Json(tool->content) == toolWire["content"] && tool->meta.present && tool->meta.value &&
                              *tool->meta.value == toolWire["_meta"] && tool->isError.present && tool->isError.value &&
                              !*tool->isError.value && tool->structuredContent.present && !tool->structuredContent.value &&
                              tool->raw == toolWire && error.empty(),
                          "MCP tool result preserves opaque content and all three presence states");

        const codex::Json listWire{
            {"data", codex::Json::array({statusValue("futureAuth")})},
            {"nextCursor", nullptr},
            {"futureListResponse", true},
        };
        const auto list = detail::decodeListMcpServerStatusResponse(listWire, error);
        result.expectTrue(list && list->data.size() == 1 && list->data[0].name == "synthetic-server" &&
                              list->data[0].resourceTemplates.size() == 1 && list->data[0].resources.size() == 1 &&
                              list->data[0].serverInfo.present && list->data[0].serverInfo.value &&
                              list->data[0].tools.contains("tool-key") && list->data[0].tools.at("tool-key").inputSchema.is_null() &&
                              list->nextCursor.present && !list->nextCursor.value && list->raw == listWire && !list->diagnostics.empty() &&
                              list->diagnostics.front().kind == typed::DecodeIssueKind::UnknownEnumValue &&
                              list->diagnostics.front().severity == typed::DecodeIssueSeverity::ForwardCompatibility && error.empty(),
                          "MCP status result maps every nested stable field and preserves an unknown auth enum nonfatally");

        const auto oauthOperation = detail::decodeClientOperationResult(detail::ClientRequestTarget::McpServerOauthLogin, oauthWire);
        const auto readOperation = detail::decodeClientOperationResult(detail::ClientRequestTarget::McpResourceRead, resourceWire);
        const auto toolOperation = detail::decodeClientOperationResult(detail::ClientRequestTarget::McpServerToolCall, toolWire);
        const auto listOperation = detail::decodeClientOperationResult(detail::ClientRequestTarget::McpServerStatusList, listWire);
        result.expectTrue(oauthOperation && std::holds_alternative<typed::McpServerOauthLoginResponse>(*oauthOperation.value) &&
                              readOperation && std::holds_alternative<typed::McpResourceReadResponse>(*readOperation.value) &&
                              toolOperation && std::holds_alternative<typed::McpServerToolCallResponse>(*toolOperation.value) &&
                              listOperation && std::holds_alternative<typed::ListMcpServerStatusResponse>(*listOperation.value),
                          "the four registered concrete result decoders retain exact target/result association");

        codex::Json malformedResource = resourceWire;
        malformedResource["contents"][0].erase("uri");
        result.expectTrue(!detail::decodeMcpResourceReadResponse(malformedResource, error) &&
                              error.find("$.contents[0].uri") != std::string::npos && error.find("synthetic") == std::string::npos,
                          "malformed MCP results fail with a safe structural path and no resource content");

        codex::Json overflowList = listWire;
        overflowList["data"][0]["resources"][0]["size"] = std::numeric_limits<std::uint64_t>::max();
        result.expectTrue(!detail::decodeListMcpServerStatusResponse(overflowList, error) &&
                              error.find("$.data[0].resources[0].size") != std::string::npos,
                          "MCP status decoding enforces signed int64 width without coercion");
    }

    void testRegistryAndFacade(tests::support::TestResult& result) {
        using McpAccessor = typed::Mcp& (typed::Client::*) () noexcept;
        using ConstMcpAccessor = const typed::Mcp& (typed::Client::*) () const noexcept;
        static_assert(std::is_same_v<decltype(static_cast<McpAccessor>(&typed::Client::mcp)), McpAccessor>);
        static_assert(std::is_same_v<decltype(static_cast<ConstMcpAccessor>(&typed::Client::mcp)), ConstMcpAccessor>);
        static_assert(std::is_same_v<decltype(&typed::Mcp::startOauthLogin),
                                     typed::Mcp::Submission (typed::Mcp::*)(typed::McpServerOauthLoginParams,
                                                                            typed::Mcp::StartOauthLoginResultHandler)>);
        static_assert(
            std::is_same_v<decltype(&typed::Mcp::readResource),
                           typed::Mcp::Submission (typed::Mcp::*)(typed::McpResourceReadParams, typed::Mcp::ReadResourceResultHandler)>);
        static_assert(
            std::is_same_v<decltype(&typed::Mcp::callTool),
                           typed::Mcp::Submission (typed::Mcp::*)(typed::McpServerToolCallParams, typed::Mcp::CallToolResultHandler)>);
        static_assert(
            std::is_same_v<decltype(&typed::Mcp::listServers),
                           typed::Mcp::Submission (typed::Mcp::*)(typed::ListMcpServerStatusParams, typed::Mcp::ListServersResultHandler)>);

        const detail::ProtocolSurfaceEntry& oauth = detail::entryFor(detail::ClientRequestTarget::McpServerOauthLogin);
        const detail::ProtocolSurfaceEntry& read = detail::entryFor(detail::ClientRequestTarget::McpResourceRead);
        const detail::ProtocolSurfaceEntry& tool = detail::entryFor(detail::ClientRequestTarget::McpServerToolCall);
        const detail::ProtocolSurfaceEntry& list = detail::entryFor(detail::ClientRequestTarget::McpServerStatusList);
        result.expectTrue(oauth.key.name == "mcpServer/oauth/login" && read.key.name == "mcpServer/resource/read" &&
                              tool.key.name == "mcpServer/tool/call" && list.key.name == "mcpServerStatus/list" &&
                              oauth.typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                              read.typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                              tool.typedSchemaStatus == detail::TypedSchemaStatus::Complete &&
                              list.typedSchemaStatus == detail::TypedSchemaStatus::Complete,
                          "the four MCP facade methods resolve only through exact Complete registry targets");
    }

} // namespace

int main() {
    tests::support::TestResult result;
    testRequestEncoders(result);
    testResultDecoders(result);
    testRegistryAndFacade(result);
    return result.processResult();
}
