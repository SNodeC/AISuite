/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/McpCodec.h"

#include "ai/openai/codex/detail/DecodeDiagnostic.h"
#include "ai/openai/codex/typed/Types.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::openai::codex::detail {

    namespace {

        const Json* member(const Json& object, std::string_view name) noexcept {
            if (!object.is_object()) {
                return nullptr;
            }
            const auto iterator = object.find(name);
            return iterator == object.end() ? nullptr : &*iterator;
        }

        std::string fieldPath(std::string_view base, std::string_view field) {
            std::string path(base);
            path.push_back('.');
            path.append(field);
            return path;
        }

        std::string indexPath(std::string_view base, std::size_t index) {
            return std::string(base) + "[" + std::to_string(index) + "]";
        }

        void expected(std::string& error, std::string_view surface, std::string_view path, std::string_view type) {
            error = std::string(surface) + " field '" + std::string(path) + "' must be " + std::string(type);
        }

        void missing(std::string& error, std::string_view surface, std::string_view path) {
            error = std::string(surface) + " is missing required field '" + std::string(path) + "'";
        }

        bool decodeObject(const Json& value, std::string& error, std::string_view surface, std::string_view path) {
            if (!value.is_object()) {
                expected(error, surface, path, "an object");
                return false;
            }
            return true;
        }

        bool decodeString(const Json& value, std::string& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!value.is_string()) {
                expected(error, surface, path, "a string");
                return false;
            }
            output = value.get_ref<const std::string&>();
            return true;
        }

        bool decodeBoolean(const Json& value, bool& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!value.is_boolean()) {
                expected(error, surface, path, "a boolean");
                return false;
            }
            output = value.get<bool>();
            return true;
        }

        bool decodeInt64(const Json& value, std::int64_t& output, std::string& error, std::string_view surface, std::string_view path) {
            if (value.is_number_unsigned()) {
                const std::uint64_t unsignedValue = value.get<std::uint64_t>();
                if (unsignedValue <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    output = static_cast<std::int64_t>(unsignedValue);
                    return true;
                }
            } else if (value.is_number_integer()) {
                output = value.get<std::int64_t>();
                return true;
            }
            expected(error, surface, path, "an int64 integer");
            return false;
        }

        bool decodeJson(const Json& value, Json& output, std::string&, std::string_view, std::string_view) {
            output = value;
            return true;
        }

        template <typename T, typename Decoder>
        bool decodeArray(const Json& value,
                         std::vector<T>& output,
                         std::string& error,
                         std::string_view surface,
                         std::string_view path,
                         Decoder&& decoder) {
            if (!value.is_array()) {
                expected(error, surface, path, "an array");
                return false;
            }
            output.clear();
            output.reserve(value.size());
            for (std::size_t index = 0; index < value.size(); ++index) {
                T decoded{};
                if (!decoder(value[index], decoded, error, surface, indexPath(path, index))) {
                    return false;
                }
                output.emplace_back(std::move(decoded));
            }
            return true;
        }

        bool
        decodeJsonArray(const Json& value, std::vector<Json>& output, std::string& error, std::string_view surface, std::string_view path) {
            return decodeArray<Json>(value, output, error, surface, path, decodeJson);
        }

        template <typename T, typename Decoder>
        bool decodeRequired(const Json& object,
                            std::string_view field,
                            T& output,
                            std::string& error,
                            std::string_view surface,
                            std::string_view path,
                            Decoder&& decoder) {
            const std::string childPath = fieldPath(path, field);
            const Json* value = member(object, field);
            if (value == nullptr) {
                missing(error, surface, childPath);
                return false;
            }
            return decoder(*value, output, error, surface, childPath);
        }

        template <typename T, typename Decoder>
        bool decodeOptional(const Json& object,
                            std::string_view field,
                            std::optional<T>& output,
                            std::string& error,
                            std::string_view surface,
                            std::string_view path,
                            Decoder&& decoder) {
            const Json* value = member(object, field);
            if (value == nullptr) {
                output.reset();
                return true;
            }
            T decoded{};
            if (!decoder(*value, decoded, error, surface, fieldPath(path, field))) {
                return false;
            }
            output = std::move(decoded);
            return true;
        }

        template <typename T, typename Decoder>
        bool decodeOptionalNullable(const Json& object,
                                    std::string_view field,
                                    typed::OptionalNullable<T>& output,
                                    std::string& error,
                                    std::string_view surface,
                                    std::string_view path,
                                    Decoder&& decoder) {
            const Json* value = member(object, field);
            if (value == nullptr) {
                output = typed::OptionalNullable<T>::omitted();
                return true;
            }
            if (value->is_null()) {
                output = typed::OptionalNullable<T>::explicitNull();
                return true;
            }
            T decoded{};
            if (!decoder(*value, decoded, error, surface, fieldPath(path, field))) {
                return false;
            }
            output = typed::OptionalNullable<T>::withValue(std::move(decoded));
            return true;
        }

        template <typename Parent, typename Child>
        void appendDiagnostics(Parent& parent, const Child& child) {
            parent.diagnostics.insert(parent.diagnostics.end(), child.diagnostics.begin(), child.diagnostics.end());
        }

        template <typename OpenEnum>
        bool decodeOpenEnum(const Json& value,
                            OpenEnum& output,
                            std::vector<typed::DecodeDiagnostic>& diagnostics,
                            std::string_view enumSurface,
                            std::string& error,
                            std::string_view surface,
                            std::string_view path) {
            if (!decodeString(value, output.value, error, surface, path)) {
                return false;
            }
            if (!output.isKnown()) {
                diagnostics.emplace_back(unknownEnumDiagnostic(std::string(enumSurface), std::string(path)));
            }
            return true;
        }

        bool decodeMcpResourceContentValue(
            const Json& value, typed::McpResourceContent& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            if (!decodeOptionalNullable(value, "_meta", output.meta, error, surface, path, decodeJson) ||
                !decodeOptionalNullable(value, "mimeType", output.mimeType, error, surface, path, decodeString) ||
                !decodeRequired(value, "uri", output.uri, error, surface, path, decodeString)) {
                return false;
            }

            // ResourceContent is an open anyOf. A valid text branch may carry
            // an unrelated future "blob" property of any shape (and vice
            // versa), so only a property that satisfies a branch is projected.
            // The complete object, including a non-matching alternate-name
            // property, remains available in raw.
            const Json* text = member(value, "text");
            const Json* blob = member(value, "blob");
            output.text =
                text != nullptr && text->is_string() ? std::optional<std::string>{text->get_ref<const std::string&>()} : std::nullopt;
            output.blob =
                blob != nullptr && blob->is_string() ? std::optional<std::string>{blob->get_ref<const std::string&>()} : std::nullopt;
            if (!output.text && !output.blob) {
                if (text != nullptr) {
                    expected(error, surface, fieldPath(path, "text"), "a string");
                } else if (blob != nullptr) {
                    expected(error, surface, fieldPath(path, "blob"), "a string");
                } else {
                    error = std::string(surface) + " field '" + std::string(path) + "' must contain text or blob";
                }
                return false;
            }
            return true;
        }

        bool decodeMcpServerInfoValue(
            const Json& value, typed::McpServerInfo& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptionalNullable(value, "description", output.description, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "icons", output.icons, error, surface, path, decodeJsonArray) &&
                   decodeRequired(value, "name", output.name, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "title", output.title, error, surface, path, decodeString) &&
                   decodeRequired(value, "version", output.version, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "websiteUrl", output.websiteUrl, error, surface, path, decodeString);
        }

        bool decodeMcpResourceValue(
            const Json& value, typed::McpResource& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptionalNullable(value, "_meta", output.meta, error, surface, path, decodeJson) &&
                   decodeOptionalNullable(value, "annotations", output.annotations, error, surface, path, decodeJson) &&
                   decodeOptionalNullable(value, "description", output.description, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "icons", output.icons, error, surface, path, decodeJsonArray) &&
                   decodeOptionalNullable(value, "mimeType", output.mimeType, error, surface, path, decodeString) &&
                   decodeRequired(value, "name", output.name, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "size", output.size, error, surface, path, decodeInt64) &&
                   decodeOptionalNullable(value, "title", output.title, error, surface, path, decodeString) &&
                   decodeRequired(value, "uri", output.uri, error, surface, path, decodeString);
        }

        bool decodeMcpResourceTemplateValue(
            const Json& value, typed::McpResourceTemplate& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptionalNullable(value, "annotations", output.annotations, error, surface, path, decodeJson) &&
                   decodeOptionalNullable(value, "description", output.description, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "mimeType", output.mimeType, error, surface, path, decodeString) &&
                   decodeRequired(value, "name", output.name, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "title", output.title, error, surface, path, decodeString) &&
                   decodeRequired(value, "uriTemplate", output.uriTemplate, error, surface, path, decodeString);
        }

        bool
        decodeMcpToolValue(const Json& value, typed::McpTool& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptionalNullable(value, "_meta", output.meta, error, surface, path, decodeJson) &&
                   decodeOptionalNullable(value, "annotations", output.annotations, error, surface, path, decodeJson) &&
                   decodeOptionalNullable(value, "description", output.description, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "icons", output.icons, error, surface, path, decodeJsonArray) &&
                   decodeRequired(value, "inputSchema", output.inputSchema, error, surface, path, decodeJson) &&
                   decodeRequired(value, "name", output.name, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "outputSchema", output.outputSchema, error, surface, path, decodeJson) &&
                   decodeOptionalNullable(value, "title", output.title, error, surface, path, decodeString);
        }

        bool decodeMcpToolMap(const Json& value,
                              std::map<std::string, typed::McpTool>& output,
                              std::string& error,
                              std::string_view surface,
                              std::string_view path) {
            if (!value.is_object()) {
                expected(error, surface, path, "an object");
                return false;
            }
            output.clear();
            for (const auto& [key, item] : value.items()) {
                typed::McpTool decoded;
                // Map keys can be application data; diagnostics intentionally
                // use a structural wildcard rather than echoing the key.
                if (!decodeMcpToolValue(item, decoded, error, surface, fieldPath(path, "*"))) {
                    return false;
                }
                output.emplace(key, std::move(decoded));
            }
            return true;
        }

        bool decodeMcpServerStatusValue(
            const Json& value, typed::McpServerStatus& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            if (!decodeRequired(value,
                                "authStatus",
                                output.authStatus,
                                error,
                                surface,
                                path,
                                [&](const Json& input,
                                    typed::McpAuthStatus& decoded,
                                    std::string& nestedError,
                                    std::string_view nestedSurface,
                                    std::string_view nestedPath) {
                                    return decodeOpenEnum(
                                        input, decoded, output.diagnostics, "McpAuthStatus", nestedError, nestedSurface, nestedPath);
                                }) ||
                !decodeRequired(value, "name", output.name, error, surface, path, decodeString) ||
                !decodeRequired(value,
                                "resourceTemplates",
                                output.resourceTemplates,
                                error,
                                surface,
                                path,
                                [](const Json& input,
                                   std::vector<typed::McpResourceTemplate>& decoded,
                                   std::string& nestedError,
                                   std::string_view nestedSurface,
                                   std::string_view nestedPath) {
                                    return decodeArray<typed::McpResourceTemplate>(
                                        input, decoded, nestedError, nestedSurface, nestedPath, decodeMcpResourceTemplateValue);
                                }) ||
                !decodeRequired(value,
                                "resources",
                                output.resources,
                                error,
                                surface,
                                path,
                                [](const Json& input,
                                   std::vector<typed::McpResource>& decoded,
                                   std::string& nestedError,
                                   std::string_view nestedSurface,
                                   std::string_view nestedPath) {
                                    return decodeArray<typed::McpResource>(
                                        input, decoded, nestedError, nestedSurface, nestedPath, decodeMcpResourceValue);
                                }) ||
                !decodeOptionalNullable(value, "serverInfo", output.serverInfo, error, surface, path, decodeMcpServerInfoValue) ||
                !decodeRequired(value, "tools", output.tools, error, surface, path, decodeMcpToolMap)) {
                return false;
            }
            for (const auto& resourceTemplate : output.resourceTemplates) {
                appendDiagnostics(output, resourceTemplate);
            }
            for (const auto& resource : output.resources) {
                appendDiagnostics(output, resource);
            }
            if (output.serverInfo.hasValue()) {
                appendDiagnostics(output, *output.serverInfo);
            }
            for (const auto& [key, tool] : output.tools) {
                static_cast<void>(key);
                appendDiagnostics(output, tool);
            }
            return true;
        }

        Json
        encoderObject(const Json& raw, std::string& error, std::string_view surface, std::initializer_list<std::string_view> knownFields) {
            if (!raw.is_object()) {
                expected(error, surface, "$.raw", "an object");
                return Json();
            }
            Json result = raw;
            for (const std::string_view field : knownFields) {
                result.erase(std::string(field));
            }
            return result;
        }

        template <typename T, typename Encoder>
        bool encodeOptionalNullable(Json& object, std::string_view field, const typed::OptionalNullable<T>& value, Encoder&& encoder) {
            if (value.isOmitted()) {
                return true;
            }
            if (value.isNull()) {
                object[std::string(field)] = nullptr;
                return true;
            }
            object[std::string(field)] = encoder(*value);
            return true;
        }

        Json encodeThreadId(const typed::ThreadId& value) {
            return value.value;
        }

    } // namespace

    std::optional<Json> encodeMcpServerOauthLoginParams(const typed::McpServerOauthLoginParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(value.raw, error, "McpServerOauthLoginParams", {"name", "scopes", "threadId", "timeoutSecs"});
            if (!error.empty()) {
                return std::nullopt;
            }
            result["name"] = value.name;
            encodeOptionalNullable(result, "scopes", value.scopes, [](const std::vector<std::string>& input) {
                return Json(input);
            });
            encodeOptionalNullable(result, "threadId", value.threadId, encodeThreadId);
            encodeOptionalNullable(result, "timeoutSecs", value.timeoutSecs, [](std::int64_t input) {
                return Json(input);
            });
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            error = "McpServerOauthLoginParams encoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<Json> encodeMcpResourceReadParams(const typed::McpResourceReadParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(value.raw, error, "McpResourceReadParams", {"server", "threadId", "uri"});
            if (!error.empty()) {
                return std::nullopt;
            }
            result["server"] = value.server;
            encodeOptionalNullable(result, "threadId", value.threadId, encodeThreadId);
            result["uri"] = value.uri;
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            error = "McpResourceReadParams encoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<Json> encodeMcpServerToolCallParams(const typed::McpServerToolCallParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(value.raw, error, "McpServerToolCallParams", {"_meta", "arguments", "server", "threadId", "tool"});
            if (!error.empty()) {
                return std::nullopt;
            }
            encodeOptionalNullable(result, "_meta", value.meta, [](const Json& input) {
                return input;
            });
            encodeOptionalNullable(result, "arguments", value.arguments, [](const Json& input) {
                return input;
            });
            result["server"] = value.server;
            result["threadId"] = value.threadId.value;
            result["tool"] = value.tool;
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            error = "McpServerToolCallParams encoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<Json> encodeListMcpServerStatusParams(const typed::ListMcpServerStatusParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(value.raw, error, "ListMcpServerStatusParams", {"cursor", "detail", "limit", "threadId"});
            if (!error.empty()) {
                return std::nullopt;
            }
            encodeOptionalNullable(result, "cursor", value.cursor, [](const std::string& input) {
                return Json(input);
            });
            encodeOptionalNullable(result, "detail", value.detail, [](const typed::McpServerStatusDetail& input) {
                return Json(input.value);
            });
            encodeOptionalNullable(result, "limit", value.limit, [](std::uint32_t input) {
                return Json(input);
            });
            encodeOptionalNullable(result, "threadId", value.threadId, encodeThreadId);
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            error = "ListMcpServerStatusParams encoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::McpServerOauthLoginResponse> decodeMcpServerOauthLoginResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            typed::McpServerOauthLoginResponse result;
            if (!decodeObject(value, error, "McpServerOauthLoginResponse", "$") ||
                !decodeRequired(
                    value, "authorizationUrl", result.authorizationUrl, error, "McpServerOauthLoginResponse", "$", decodeString)) {
                return std::nullopt;
            }
            result.raw = value;
            return result;
        } catch (...) {
            error = "McpServerOauthLoginResponse decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::McpResourceReadResponse> decodeMcpResourceReadResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            typed::McpResourceReadResponse result;
            if (!decodeObject(value, error, "McpResourceReadResponse", "$") ||
                !decodeRequired(value,
                                "contents",
                                result.contents,
                                error,
                                "McpResourceReadResponse",
                                "$",
                                [](const Json& input,
                                   std::vector<typed::McpResourceContent>& decoded,
                                   std::string& nestedError,
                                   std::string_view nestedSurface,
                                   std::string_view nestedPath) {
                                    return decodeArray<typed::McpResourceContent>(
                                        input, decoded, nestedError, nestedSurface, nestedPath, decodeMcpResourceContentValue);
                                })) {
                return std::nullopt;
            }
            result.raw = value;
            for (const auto& content : result.contents) {
                appendDiagnostics(result, content);
            }
            return result;
        } catch (...) {
            error = "McpResourceReadResponse decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::McpServerToolCallResponse> decodeMcpServerToolCallResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            typed::McpServerToolCallResponse result;
            if (!decodeObject(value, error, "McpServerToolCallResponse", "$") ||
                !decodeOptionalNullable(value, "_meta", result.meta, error, "McpServerToolCallResponse", "$", decodeJson) ||
                !decodeRequired(value, "content", result.content, error, "McpServerToolCallResponse", "$", decodeJsonArray) ||
                !decodeOptionalNullable(value, "isError", result.isError, error, "McpServerToolCallResponse", "$", decodeBoolean) ||
                !decodeOptionalNullable(
                    value, "structuredContent", result.structuredContent, error, "McpServerToolCallResponse", "$", decodeJson)) {
                return std::nullopt;
            }
            result.raw = value;
            return result;
        } catch (...) {
            error = "McpServerToolCallResponse decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::ListMcpServerStatusResponse> decodeListMcpServerStatusResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            typed::ListMcpServerStatusResponse result;
            if (!decodeObject(value, error, "ListMcpServerStatusResponse", "$") ||
                !decodeRequired(value,
                                "data",
                                result.data,
                                error,
                                "ListMcpServerStatusResponse",
                                "$",
                                [](const Json& input,
                                   std::vector<typed::McpServerStatus>& decoded,
                                   std::string& nestedError,
                                   std::string_view nestedSurface,
                                   std::string_view nestedPath) {
                                    return decodeArray<typed::McpServerStatus>(
                                        input, decoded, nestedError, nestedSurface, nestedPath, decodeMcpServerStatusValue);
                                }) ||
                !decodeOptionalNullable(value, "nextCursor", result.nextCursor, error, "ListMcpServerStatusResponse", "$", decodeString)) {
                return std::nullopt;
            }
            result.raw = value;
            for (const auto& status : result.data) {
                appendDiagnostics(result, status);
            }
            return result;
        } catch (...) {
            error = "ListMcpServerStatusResponse decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::McpServerOauthLoginCompletedNotification>
    decodeMcpServerOauthLoginCompletedNotification(const Notification& notification, std::string& error) noexcept {
        try {
            error.clear();
            typed::McpServerOauthLoginCompletedNotification result;
            if (!decodeObject(notification.params, error, "mcpServer/oauthLogin/completed", "$.params") ||
                !decodeOptionalNullable(
                    notification.params, "error", result.error, error, "mcpServer/oauthLogin/completed", "$.params", decodeString) ||
                !decodeRequired(
                    notification.params, "name", result.name, error, "mcpServer/oauthLogin/completed", "$.params", decodeString) ||
                !decodeRequired(
                    notification.params, "success", result.success, error, "mcpServer/oauthLogin/completed", "$.params", decodeBoolean) ||
                !decodeOptionalNullable(notification.params,
                                        "threadId",
                                        result.threadId,
                                        error,
                                        "mcpServer/oauthLogin/completed",
                                        "$.params",
                                        [](const Json& input,
                                           typed::ThreadId& decoded,
                                           std::string& nestedError,
                                           std::string_view nestedSurface,
                                           std::string_view nestedPath) {
                                            return decodeString(input, decoded.value, nestedError, nestedSurface, nestedPath);
                                        })) {
                return std::nullopt;
            }
            result.raw = notification.raw;
            return result;
        } catch (...) {
            error = "mcpServer/oauthLogin/completed decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::McpServerStatusUpdatedNotification> decodeMcpServerStatusUpdatedNotification(const Notification& notification,
                                                                                                      std::string& error) noexcept {
        try {
            error.clear();
            typed::McpServerStatusUpdatedNotification result;
            if (!decodeObject(notification.params, error, "mcpServer/startupStatus/updated", "$.params") ||
                !decodeOptionalNullable(
                    notification.params, "error", result.error, error, "mcpServer/startupStatus/updated", "$.params", decodeString) ||
                !decodeOptionalNullable(
                    notification.params,
                    "failureReason",
                    result.failureReason,
                    error,
                    "mcpServer/startupStatus/updated",
                    "$.params",
                    [&](const Json& input,
                        typed::McpServerStartupFailureReason& decoded,
                        std::string& nestedError,
                        std::string_view nestedSurface,
                        std::string_view nestedPath) {
                        return decodeOpenEnum(
                            input, decoded, result.diagnostics, "McpServerStartupFailureReason", nestedError, nestedSurface, nestedPath);
                    }) ||
                !decodeRequired(
                    notification.params, "name", result.name, error, "mcpServer/startupStatus/updated", "$.params", decodeString) ||
                !decodeRequired(
                    notification.params,
                    "status",
                    result.status,
                    error,
                    "mcpServer/startupStatus/updated",
                    "$.params",
                    [&](const Json& input,
                        typed::McpServerStartupState& decoded,
                        std::string& nestedError,
                        std::string_view nestedSurface,
                        std::string_view nestedPath) {
                        return decodeOpenEnum(
                            input, decoded, result.diagnostics, "McpServerStartupState", nestedError, nestedSurface, nestedPath);
                    }) ||
                !decodeOptionalNullable(notification.params,
                                        "threadId",
                                        result.threadId,
                                        error,
                                        "mcpServer/startupStatus/updated",
                                        "$.params",
                                        [](const Json& input,
                                           typed::ThreadId& decoded,
                                           std::string& nestedError,
                                           std::string_view nestedSurface,
                                           std::string_view nestedPath) {
                                            return decodeString(input, decoded.value, nestedError, nestedSurface, nestedPath);
                                        })) {
                return std::nullopt;
            }
            result.raw = notification.raw;
            return result;
        } catch (...) {
            error = "mcpServer/startupStatus/updated decoding failed safely";
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::detail
