/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/McpReverseRequestCodec.h"

#include "ai/openai/codex/typed/Conversation.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "ai/openai/codex/typed/Types.h"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace ai::openai::codex::detail {

    namespace {
        bool requireObject(const Json& value, std::string_view surface, std::string& error) {
            if (value.is_object()) {
                return true;
            }
            error = std::string(surface) + " expects an object at '$'";
            return false;
        }

        bool requiredString(const Json& value, const char* field, std::string_view surface, std::string& result, std::string& error) {
            const auto member = value.find(field);
            if (member == value.end()) {
                error = std::string(surface) + " is missing required string at '$." + field + "'";
                return false;
            }
            if (!member->is_string()) {
                error = std::string(surface) + " expects a string at '$." + field + "'";
                return false;
            }
            result = member->get<std::string>();
            return true;
        }

        bool optionalNullableString(const Json& value,
                                    const char* field,
                                    std::string_view surface,
                                    typed::OptionalNullable<std::string>& result,
                                    std::string& error) {
            const auto member = value.find(field);
            if (member == value.end()) {
                result = typed::OptionalNullable<std::string>::omitted();
                return true;
            }
            if (member->is_null()) {
                result = typed::OptionalNullable<std::string>::explicitNull();
                return true;
            }
            if (!member->is_string()) {
                error = std::string(surface) + " expects a string or null at '$." + field + "'";
                return false;
            }
            result = typed::OptionalNullable<std::string>::withValue(member->get<std::string>());
            return true;
        }

        bool openObject(Json raw, std::string_view surface, Json& result, std::string& error) {
            if (!raw.is_object()) {
                error = std::string(surface) + " raw future fields must be an object";
                return false;
            }
            result = std::move(raw);
            return true;
        }

        std::optional<Json> encodeDynamicToolCallOutputContentItem(const typed::DynamicToolCallOutputContentItem& item,
                                                                   std::string& error) {
            return std::visit(
                [&error](const auto& value) -> std::optional<Json> {
                    using Value = std::decay_t<decltype(value)>;
                    if constexpr (std::is_same_v<Value, typed::InputTextDynamicToolCallOutputContentItem>) {
                        Json encoded;
                        if (!openObject(value.raw, "InputTextDynamicToolCallOutputContentItem", encoded, error)) {
                            return std::nullopt;
                        }
                        encoded["type"] = "inputText";
                        encoded["text"] = value.text;
                        return std::optional<Json>{std::move(encoded)};
                    } else if constexpr (std::is_same_v<Value, typed::InputImageDynamicToolCallOutputContentItem>) {
                        Json encoded;
                        if (!openObject(value.raw, "InputImageDynamicToolCallOutputContentItem", encoded, error)) {
                            return std::nullopt;
                        }
                        encoded["type"] = "inputImage";
                        encoded["imageUrl"] = value.imageUrl;
                        return std::optional<Json>{std::move(encoded)};
                    } else {
                        error = "DynamicToolCallResponse cannot encode an unknown future content-item alternative";
                        return std::nullopt;
                    }
                },
                item);
        }
    } // namespace

    std::optional<typed::AttestationGenerateParams> decodeAttestationGenerateParams(const Json& value, std::string& error) noexcept {
        constexpr std::string_view Surface = "AttestationGenerateParams";
        try {
            if (!requireObject(value, Surface, error)) {
                return std::nullopt;
            }
            error.clear();
            return typed::AttestationGenerateParams{value, {}};
        } catch (const std::exception&) {
            error = std::string(Surface) + " could not be decoded at '$'";
            return std::nullopt;
        } catch (...) {
            error = std::string(Surface) + " could not be decoded at '$'";
            return std::nullopt;
        }
    }

    std::optional<typed::DynamicToolCallParams> decodeDynamicToolCallParams(const Json& value, std::string& error) noexcept {
        constexpr std::string_view Surface = "DynamicToolCallParams";
        try {
            if (!requireObject(value, Surface, error)) {
                return std::nullopt;
            }

            const auto arguments = value.find("arguments");
            if (arguments == value.end()) {
                error = std::string(Surface) + " is missing required JSON value at '$.arguments'";
                return std::nullopt;
            }

            typed::DynamicToolCallParams result;
            std::string callId;
            std::string threadId;
            std::string turnId;
            if (!requiredString(value, "callId", Surface, callId, error) ||
                !optionalNullableString(value, "namespace", Surface, result.nameSpace, error) ||
                !requiredString(value, "threadId", Surface, threadId, error) ||
                !requiredString(value, "tool", Surface, result.tool, error) || !requiredString(value, "turnId", Surface, turnId, error)) {
                return std::nullopt;
            }

            result.arguments = *arguments;
            result.callId = typed::ResponseCallId{std::move(callId)};
            result.threadId = typed::ThreadId{std::move(threadId)};
            result.turnId = typed::TurnId{std::move(turnId)};
            result.raw = value;
            error.clear();
            return std::optional<typed::DynamicToolCallParams>{std::move(result)};
        } catch (const std::exception&) {
            error = std::string(Surface) + " could not be decoded at '$'";
            return std::nullopt;
        } catch (...) {
            error = std::string(Surface) + " could not be decoded at '$'";
            return std::nullopt;
        }
    }

    std::optional<Json> encodeAttestationGenerateResponse(const typed::AttestationGenerateResponse& value, std::string& error) noexcept {
        constexpr std::string_view Surface = "AttestationGenerateResponse";
        try {
            Json result;
            if (!openObject(value.raw, Surface, result, error)) {
                return std::nullopt;
            }
            result["token"] = value.token;
            error.clear();
            return std::optional<Json>{std::move(result)};
        } catch (const std::exception&) {
            error = std::string(Surface) + " could not be encoded";
            return std::nullopt;
        } catch (...) {
            error = std::string(Surface) + " could not be encoded";
            return std::nullopt;
        }
    }

    std::optional<Json> encodeDynamicToolCallResponse(const typed::DynamicToolCallResponse& value, std::string& error) noexcept {
        constexpr std::string_view Surface = "DynamicToolCallResponse";
        try {
            Json result;
            if (!openObject(value.raw, Surface, result, error)) {
                return std::nullopt;
            }
            Json contentItems = Json::array();
            for (const typed::DynamicToolCallOutputContentItem& item : value.contentItems) {
                std::optional<Json> encoded = encodeDynamicToolCallOutputContentItem(item, error);
                if (!encoded) {
                    return std::nullopt;
                }
                contentItems.push_back(std::move(*encoded));
            }
            result["contentItems"] = std::move(contentItems);
            result["success"] = value.success;
            error.clear();
            return std::optional<Json>{std::move(result)};
        } catch (const std::exception&) {
            error = std::string(Surface) + " could not be encoded";
            return std::nullopt;
        } catch (...) {
            error = std::string(Surface) + " could not be encoded";
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::detail
