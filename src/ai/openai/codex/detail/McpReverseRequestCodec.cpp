/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/McpReverseRequestCodec.h"

#include "ai/openai/codex/detail/DecodeDiagnostic.h"
#include "ai/openai/codex/typed/Conversation.h"
#include "ai/openai/codex/typed/ServerRequests.h"
#include "ai/openai/codex/typed/Types.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <initializer_list>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace ai::openai::codex::detail {

    namespace {
        bool requireObject(const Json& value, std::string_view surface, std::string& error) {
            if (value.is_object()) {
                return true;
            }
            error = std::string(surface) + " expects an object at '$'";
            return false;
        }

        bool requireObjectAt(const Json& value, std::string_view surface, std::string_view path, std::string& error) {
            if (value.is_object()) {
                return true;
            }
            error = std::string(surface) + " expects an object at '" + std::string(path) + "'";
            return false;
        }

        bool requireClosedObject(const Json& value,
                                 std::string_view surface,
                                 std::string_view path,
                                 std::initializer_list<std::string_view> allowed,
                                 std::string& error) {
            if (!requireObjectAt(value, surface, path, error)) {
                return false;
            }
            for (auto member = value.begin(); member != value.end(); ++member) {
                if (std::find(allowed.begin(), allowed.end(), std::string_view(member.key())) == allowed.end()) {
                    error = std::string(surface) + " contains an unsupported property at '" + std::string(path) + "'";
                    return false;
                }
            }
            return true;
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

        bool requiredStringAt(const Json& value,
                              const char* field,
                              std::string_view surface,
                              std::string_view path,
                              std::string& result,
                              std::string& error) {
            const auto member = value.find(field);
            const std::string fieldPath = std::string(path) + "." + field;
            if (member == value.end()) {
                error = std::string(surface) + " is missing required string at '" + fieldPath + "'";
                return false;
            }
            if (!member->is_string()) {
                error = std::string(surface) + " expects a string at '" + fieldPath + "'";
                return false;
            }
            result = member->get<std::string>();
            return true;
        }

        bool requiredArrayAt(const Json& value,
                             const char* field,
                             std::string_view surface,
                             std::string_view path,
                             const Json*& result,
                             std::string& error) {
            const auto member = value.find(field);
            const std::string fieldPath = std::string(path) + "." + field;
            if (member == value.end()) {
                error = std::string(surface) + " is missing required array at '" + fieldPath + "'";
                return false;
            }
            if (!member->is_array()) {
                error = std::string(surface) + " expects an array at '" + fieldPath + "'";
                return false;
            }
            result = &*member;
            return true;
        }

        bool requiredObjectAt(const Json& value,
                              const char* field,
                              std::string_view surface,
                              std::string_view path,
                              const Json*& result,
                              std::string& error) {
            const auto member = value.find(field);
            const std::string fieldPath = std::string(path) + "." + field;
            if (member == value.end()) {
                error = std::string(surface) + " is missing required object at '" + fieldPath + "'";
                return false;
            }
            if (!member->is_object()) {
                error = std::string(surface) + " expects an object at '" + fieldPath + "'";
                return false;
            }
            result = &*member;
            return true;
        }

        bool decodeStringArray(
            const Json& value, std::string_view surface, std::string_view path, std::vector<std::string>& result, std::string& error) {
            if (!value.is_array()) {
                error = std::string(surface) + " expects an array at '" + std::string(path) + "'";
                return false;
            }
            result.clear();
            result.reserve(value.size());
            for (std::size_t index = 0; index < value.size(); ++index) {
                if (!value[index].is_string()) {
                    error = std::string(surface) + " expects a string at '" + std::string(path) + "[*]'";
                    return false;
                }
                result.push_back(value[index].get<std::string>());
            }
            return true;
        }

        template <typename T, typename Decoder>
        bool optionalNullable(const Json& value,
                              const char* field,
                              [[maybe_unused]] std::string_view surface,
                              std::string_view path,
                              typed::OptionalNullable<T>& result,
                              std::string& error,
                              Decoder&& decoder) {
            const auto member = value.find(field);
            if (member == value.end()) {
                result = typed::OptionalNullable<T>::omitted();
                return true;
            }
            if (member->is_null()) {
                result = typed::OptionalNullable<T>::explicitNull();
                return true;
            }
            T decoded;
            const std::string fieldPath = std::string(path) + "." + field;
            if (!decoder(*member, decoded, fieldPath, error)) {
                return false;
            }
            result = typed::OptionalNullable<T>::withValue(std::move(decoded));
            return true;
        }

        bool optionalNullableStringAt(const Json& value,
                                      const char* field,
                                      std::string_view surface,
                                      std::string_view path,
                                      typed::OptionalNullable<std::string>& result,
                                      std::string& error) {
            return optionalNullable<std::string>(
                value,
                field,
                surface,
                path,
                result,
                error,
                [surface](const Json& input, std::string& output, const std::string& fieldPath, std::string& nestedError) {
                    if (!input.is_string()) {
                        nestedError = std::string(surface) + " expects a string or null at '" + fieldPath + "'";
                        return false;
                    }
                    output = input.get<std::string>();
                    return true;
                });
        }

        bool optionalBoolean(const Json& value,
                             const char* field,
                             std::string_view surface,
                             std::string_view path,
                             std::optional<bool>& result,
                             std::string& error) {
            const auto member = value.find(field);
            if (member == value.end()) {
                result.reset();
                return true;
            }
            if (!member->is_boolean()) {
                error = std::string(surface) + " expects a boolean at '" + std::string(path) + "." + field + "'";
                return false;
            }
            result = member->get<bool>();
            return true;
        }

        bool decodeOptionalNullableUint64(const Json& value,
                                          const char* field,
                                          std::string_view surface,
                                          std::string_view path,
                                          typed::OptionalNullable<std::uint64_t>& result,
                                          std::string& error) {
            return optionalNullable<std::uint64_t>(
                value,
                field,
                surface,
                path,
                result,
                error,
                [surface](const Json& input, std::uint64_t& output, const std::string& fieldPath, std::string& nestedError) {
                    if (input.is_number_unsigned()) {
                        output = input.get<std::uint64_t>();
                        return true;
                    }
                    if (input.is_number_integer()) {
                        const std::int64_t signedValue = input.get<std::int64_t>();
                        if (signedValue >= 0) {
                            output = static_cast<std::uint64_t>(signedValue);
                            return true;
                        }
                    }
                    nestedError = std::string(surface) + " expects a uint64 value or null at '" + fieldPath + "'";
                    return false;
                });
        }

        bool decodeOptionalNullableUint32(const Json& value,
                                          const char* field,
                                          std::string_view surface,
                                          std::string_view path,
                                          typed::OptionalNullable<std::uint32_t>& result,
                                          std::string& error) {
            return optionalNullable<std::uint32_t>(
                value,
                field,
                surface,
                path,
                result,
                error,
                [surface](const Json& input, std::uint32_t& output, const std::string& fieldPath, std::string& nestedError) {
                    std::uint64_t unsignedValue = 0;
                    if (input.is_number_unsigned()) {
                        unsignedValue = input.get<std::uint64_t>();
                    } else if (input.is_number_integer()) {
                        const std::int64_t signedValue = input.get<std::int64_t>();
                        if (signedValue < 0) {
                            nestedError = std::string(surface) + " expects a uint32 value or null at '" + fieldPath + "'";
                            return false;
                        }
                        unsignedValue = static_cast<std::uint64_t>(signedValue);
                    } else {
                        nestedError = std::string(surface) + " expects a uint32 value or null at '" + fieldPath + "'";
                        return false;
                    }
                    if (unsignedValue > std::numeric_limits<std::uint32_t>::max()) {
                        nestedError = std::string(surface) + " expects a uint32 value or null at '" + fieldPath + "'";
                        return false;
                    }
                    output = static_cast<std::uint32_t>(unsignedValue);
                    return true;
                });
        }

        bool decodeOptionalNullableDouble(const Json& value,
                                          const char* field,
                                          std::string_view surface,
                                          std::string_view path,
                                          typed::OptionalNullable<double>& result,
                                          std::string& error) {
            return optionalNullable<double>(
                value,
                field,
                surface,
                path,
                result,
                error,
                [surface](const Json& input, double& output, const std::string& fieldPath, std::string& nestedError) {
                    if (!input.is_number()) {
                        nestedError = std::string(surface) + " expects a number or null at '" + fieldPath + "'";
                        return false;
                    }
                    output = input.get<double>();
                    return true;
                });
        }

        bool decodeOptionalNullableBoolean(const Json& value,
                                           const char* field,
                                           std::string_view surface,
                                           std::string_view path,
                                           typed::OptionalNullable<bool>& result,
                                           std::string& error) {
            return optionalNullable<bool>(
                value,
                field,
                surface,
                path,
                result,
                error,
                [surface](const Json& input, bool& output, const std::string& fieldPath, std::string& nestedError) {
                    if (!input.is_boolean()) {
                        nestedError = std::string(surface) + " expects a boolean or null at '" + fieldPath + "'";
                        return false;
                    }
                    output = input.get<bool>();
                    return true;
                });
        }

        bool decodeOptionalNullableStringArray(const Json& value,
                                               const char* field,
                                               std::string_view surface,
                                               std::string_view path,
                                               typed::OptionalNullable<std::vector<std::string>>& result,
                                               std::string& error) {
            return optionalNullable<std::vector<std::string>>(
                value,
                field,
                surface,
                path,
                result,
                error,
                [surface](const Json& input, std::vector<std::string>& output, const std::string& fieldPath, std::string& nestedError) {
                    return decodeStringArray(input, surface, fieldPath, output, nestedError);
                });
        }

        bool decodeOptionalNullableJson(const Json& value, const char* field, typed::OptionalNullable<Json>& result) {
            const auto member = value.find(field);
            if (member == value.end()) {
                result = typed::OptionalNullable<Json>::omitted();
            } else if (member->is_null()) {
                result = typed::OptionalNullable<Json>::explicitNull();
            } else {
                result = typed::OptionalNullable<Json>::withValue(*member);
            }
            return true;
        }

        void appendDiagnostics(std::vector<typed::DecodeDiagnostic>& target, const std::vector<typed::DecodeDiagnostic>& source) {
            target.insert(target.end(), source.begin(), source.end());
        }

        bool decodeMcpElicitationConstOption(const Json& value,
                                             typed::McpElicitationConstOption& result,
                                             std::string_view path,
                                             std::string& error) {
            constexpr std::string_view Surface = "McpElicitationConstOption";
            if (!requireClosedObject(value, Surface, path, {"const", "title"}, error) ||
                !requiredStringAt(value, "const", Surface, path, result.constant, error) ||
                !requiredStringAt(value, "title", Surface, path, result.title, error)) {
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            return true;
        }

        bool decodeConstOptionArray(const Json& value,
                                    std::vector<typed::McpElicitationConstOption>& result,
                                    std::string_view path,
                                    std::string& error) {
            constexpr std::string_view Surface = "McpElicitationConstOption";
            if (!value.is_array()) {
                error = std::string(Surface) + " expects an array at '" + std::string(path) + "'";
                return false;
            }
            result.clear();
            result.reserve(value.size());
            for (std::size_t index = 0; index < value.size(); ++index) {
                typed::McpElicitationConstOption option;
                const std::string itemPath = std::string(path) + "[*]";
                if (!decodeMcpElicitationConstOption(value[index], option, itemPath, error)) {
                    return false;
                }
                result.push_back(std::move(option));
            }
            return true;
        }

        bool decodeMcpElicitationBooleanSchema(const Json& value,
                                               typed::McpElicitationBooleanSchema& result,
                                               std::string_view path,
                                               std::string& error) {
            constexpr std::string_view Surface = "McpElicitationBooleanSchema";
            if (!requireClosedObject(value, Surface, path, {"default", "description", "title", "type"}, error) ||
                !decodeOptionalNullableBoolean(value, "default", Surface, path, result.defaultValue, error) ||
                !optionalNullableStringAt(value, "description", Surface, path, result.description, error) ||
                !optionalNullableStringAt(value, "title", Surface, path, result.title, error)) {
                return false;
            }
            std::string type;
            if (!requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }
            result.type = typed::McpElicitationBooleanType{std::move(type)};
            if (!result.type.isKnown()) {
                error = std::string(Surface) + " expects the known boolean type at '" + std::string(path) + ".type'";
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            return true;
        }

        bool decodeMcpElicitationNumberSchema(const Json& value,
                                              typed::McpElicitationNumberSchema& result,
                                              std::string_view path,
                                              std::string& error) {
            constexpr std::string_view Surface = "McpElicitationNumberSchema";
            if (!requireClosedObject(value, Surface, path, {"default", "description", "maximum", "minimum", "title", "type"}, error) ||
                !decodeOptionalNullableDouble(value, "default", Surface, path, result.defaultValue, error) ||
                !optionalNullableStringAt(value, "description", Surface, path, result.description, error) ||
                !decodeOptionalNullableDouble(value, "maximum", Surface, path, result.maximum, error) ||
                !decodeOptionalNullableDouble(value, "minimum", Surface, path, result.minimum, error) ||
                !optionalNullableStringAt(value, "title", Surface, path, result.title, error)) {
                return false;
            }
            std::string type;
            if (!requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }
            result.type = typed::McpElicitationNumberType{std::move(type)};
            if (!result.type.isKnown()) {
                error = std::string(Surface) + " expects a known number type at '" + std::string(path) + ".type'";
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            return true;
        }

        bool decodeMcpElicitationStringSchema(const Json& value,
                                              typed::McpElicitationStringSchema& result,
                                              std::string_view path,
                                              std::string& error) {
            constexpr std::string_view Surface = "McpElicitationStringSchema";
            result.diagnostics.clear();
            if (!requireClosedObject(
                    value, Surface, path, {"default", "description", "format", "maxLength", "minLength", "title", "type"}, error) ||
                !optionalNullableStringAt(value, "default", Surface, path, result.defaultValue, error) ||
                !optionalNullableStringAt(value, "description", Surface, path, result.description, error) ||
                !decodeOptionalNullableUint32(value, "maxLength", Surface, path, result.maxLength, error) ||
                !decodeOptionalNullableUint32(value, "minLength", Surface, path, result.minLength, error) ||
                !optionalNullableStringAt(value, "title", Surface, path, result.title, error)) {
                return false;
            }

            const auto format = value.find("format");
            if (format == value.end()) {
                result.format = typed::OptionalNullable<typed::McpElicitationStringFormat>::omitted();
            } else if (format->is_null()) {
                result.format = typed::OptionalNullable<typed::McpElicitationStringFormat>::explicitNull();
            } else if (!format->is_string()) {
                error = std::string(Surface) + " expects a string or null at '" + std::string(path) + ".format'";
                return false;
            } else {
                typed::McpElicitationStringFormat decoded{format->get<std::string>()};
                if (!decoded.isKnown()) {
                    result.diagnostics.emplace_back(unknownEnumDiagnostic("McpElicitationStringFormat", std::string(path) + ".format"));
                }
                result.format = typed::OptionalNullable<typed::McpElicitationStringFormat>::withValue(std::move(decoded));
            }

            std::string type;
            if (!requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }
            result.type = typed::McpElicitationStringType{std::move(type)};
            if (!result.type.isKnown()) {
                error = std::string(Surface) + " expects the known string type at '" + std::string(path) + ".type'";
                return false;
            }
            result.raw = value;
            return true;
        }

        bool decodeMcpElicitationTitledEnumItems(const Json& value,
                                                 typed::McpElicitationTitledEnumItems& result,
                                                 std::string_view path,
                                                 std::string& error) {
            constexpr std::string_view Surface = "McpElicitationTitledEnumItems";
            const Json* anyOf = nullptr;
            if (!requireClosedObject(value, Surface, path, {"anyOf"}, error) ||
                !requiredArrayAt(value, "anyOf", Surface, path, anyOf, error) ||
                !decodeConstOptionArray(*anyOf, result.anyOf, std::string(path) + ".anyOf", error)) {
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            for (const auto& option : result.anyOf) {
                appendDiagnostics(result.diagnostics, option.diagnostics);
            }
            return true;
        }

        bool decodeMcpElicitationUntitledEnumItems(const Json& value,
                                                   typed::McpElicitationUntitledEnumItems& result,
                                                   std::string_view path,
                                                   std::string& error) {
            constexpr std::string_view Surface = "McpElicitationUntitledEnumItems";
            const Json* values = nullptr;
            std::string type;
            if (!requireClosedObject(value, Surface, path, {"enum", "type"}, error) ||
                !requiredArrayAt(value, "enum", Surface, path, values, error) ||
                !decodeStringArray(*values, Surface, std::string(path) + ".enum", result.values, error) ||
                !requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }
            result.type = typed::McpElicitationStringType{std::move(type)};
            if (!result.type.isKnown()) {
                error = std::string(Surface) + " expects the known string type at '" + std::string(path) + ".type'";
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            return true;
        }

        bool decodeMcpElicitationTitledMultiSelect(const Json& value,
                                                   typed::McpElicitationTitledMultiSelectEnumSchema& result,
                                                   std::string_view path,
                                                   std::string& error) {
            constexpr std::string_view Surface = "McpElicitationTitledMultiSelectEnumSchema";
            const Json* items = nullptr;
            std::string type;
            if (!requireClosedObject(
                    value, Surface, path, {"default", "description", "items", "maxItems", "minItems", "title", "type"}, error) ||
                !decodeOptionalNullableStringArray(value, "default", Surface, path, result.defaultValue, error) ||
                !optionalNullableStringAt(value, "description", Surface, path, result.description, error) ||
                !requiredObjectAt(value, "items", Surface, path, items, error) ||
                !decodeMcpElicitationTitledEnumItems(*items, result.items, std::string(path) + ".items", error) ||
                !decodeOptionalNullableUint64(value, "maxItems", Surface, path, result.maxItems, error) ||
                !decodeOptionalNullableUint64(value, "minItems", Surface, path, result.minItems, error) ||
                !optionalNullableStringAt(value, "title", Surface, path, result.title, error) ||
                !requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }
            result.type = typed::McpElicitationArrayType{std::move(type)};
            if (!result.type.isKnown()) {
                error = std::string(Surface) + " expects the known array type at '" + std::string(path) + ".type'";
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            appendDiagnostics(result.diagnostics, result.items.diagnostics);
            return true;
        }

        bool decodeMcpElicitationUntitledMultiSelect(const Json& value,
                                                     typed::McpElicitationUntitledMultiSelectEnumSchema& result,
                                                     std::string_view path,
                                                     std::string& error) {
            constexpr std::string_view Surface = "McpElicitationUntitledMultiSelectEnumSchema";
            const Json* items = nullptr;
            std::string type;
            if (!requireClosedObject(
                    value, Surface, path, {"default", "description", "items", "maxItems", "minItems", "title", "type"}, error) ||
                !decodeOptionalNullableStringArray(value, "default", Surface, path, result.defaultValue, error) ||
                !optionalNullableStringAt(value, "description", Surface, path, result.description, error) ||
                !requiredObjectAt(value, "items", Surface, path, items, error) ||
                !decodeMcpElicitationUntitledEnumItems(*items, result.items, std::string(path) + ".items", error) ||
                !decodeOptionalNullableUint64(value, "maxItems", Surface, path, result.maxItems, error) ||
                !decodeOptionalNullableUint64(value, "minItems", Surface, path, result.minItems, error) ||
                !optionalNullableStringAt(value, "title", Surface, path, result.title, error) ||
                !requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }
            result.type = typed::McpElicitationArrayType{std::move(type)};
            if (!result.type.isKnown()) {
                error = std::string(Surface) + " expects the known array type at '" + std::string(path) + ".type'";
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            appendDiagnostics(result.diagnostics, result.items.diagnostics);
            return true;
        }

        bool decodeMcpElicitationTitledSingleSelect(const Json& value,
                                                    typed::McpElicitationTitledSingleSelectEnumSchema& result,
                                                    std::string_view path,
                                                    std::string& error) {
            constexpr std::string_view Surface = "McpElicitationTitledSingleSelectEnumSchema";
            const Json* oneOf = nullptr;
            std::string type;
            if (!requireClosedObject(value, Surface, path, {"default", "description", "oneOf", "title", "type"}, error) ||
                !optionalNullableStringAt(value, "default", Surface, path, result.defaultValue, error) ||
                !optionalNullableStringAt(value, "description", Surface, path, result.description, error) ||
                !requiredArrayAt(value, "oneOf", Surface, path, oneOf, error) ||
                !decodeConstOptionArray(*oneOf, result.oneOf, std::string(path) + ".oneOf", error) ||
                !optionalNullableStringAt(value, "title", Surface, path, result.title, error) ||
                !requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }
            result.type = typed::McpElicitationStringType{std::move(type)};
            if (!result.type.isKnown()) {
                error = std::string(Surface) + " expects the known string type at '" + std::string(path) + ".type'";
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            for (const auto& option : result.oneOf) {
                appendDiagnostics(result.diagnostics, option.diagnostics);
            }
            return true;
        }

        bool decodeMcpElicitationUntitledSingleSelect(const Json& value,
                                                      typed::McpElicitationUntitledSingleSelectEnumSchema& result,
                                                      std::string_view path,
                                                      std::string& error) {
            constexpr std::string_view Surface = "McpElicitationUntitledSingleSelectEnumSchema";
            const Json* values = nullptr;
            std::string type;
            if (!requireClosedObject(value, Surface, path, {"default", "description", "enum", "title", "type"}, error) ||
                !optionalNullableStringAt(value, "default", Surface, path, result.defaultValue, error) ||
                !optionalNullableStringAt(value, "description", Surface, path, result.description, error) ||
                !requiredArrayAt(value, "enum", Surface, path, values, error) ||
                !decodeStringArray(*values, Surface, std::string(path) + ".enum", result.values, error) ||
                !optionalNullableStringAt(value, "title", Surface, path, result.title, error) ||
                !requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }
            result.type = typed::McpElicitationStringType{std::move(type)};
            if (!result.type.isKnown()) {
                error = std::string(Surface) + " expects the known string type at '" + std::string(path) + ".type'";
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            return true;
        }

        bool decodeMcpElicitationLegacyTitledEnum(const Json& value,
                                                  typed::McpElicitationLegacyTitledEnumSchema& result,
                                                  std::string_view path,
                                                  std::string& error) {
            constexpr std::string_view Surface = "McpElicitationLegacyTitledEnumSchema";
            const Json* values = nullptr;
            std::string type;
            if (!requireClosedObject(value, Surface, path, {"default", "description", "enum", "enumNames", "title", "type"}, error) ||
                !optionalNullableStringAt(value, "default", Surface, path, result.defaultValue, error) ||
                !optionalNullableStringAt(value, "description", Surface, path, result.description, error) ||
                !requiredArrayAt(value, "enum", Surface, path, values, error) ||
                !decodeStringArray(*values, Surface, std::string(path) + ".enum", result.values, error) ||
                !decodeOptionalNullableStringArray(value, "enumNames", Surface, path, result.enumNames, error) ||
                !optionalNullableStringAt(value, "title", Surface, path, result.title, error) ||
                !requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }
            result.type = typed::McpElicitationStringType{std::move(type)};
            if (!result.type.isKnown()) {
                error = std::string(Surface) + " expects the known string type at '" + std::string(path) + ".type'";
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            return true;
        }

        bool decodeMcpElicitationPrimitive(const Json& value,
                                           typed::McpElicitationPrimitiveSchema& result,
                                           std::string_view path,
                                           std::vector<typed::DecodeDiagnostic>& diagnostics,
                                           std::string& error) {
            constexpr std::string_view Surface = "McpElicitationPrimitiveSchema";
            if (!requireObjectAt(value, Surface, path, error)) {
                return false;
            }
            std::string type;
            if (!requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }

            if (type == "boolean") {
                typed::McpElicitationBooleanSchema decoded;
                if (!decodeMcpElicitationBooleanSchema(value, decoded, path, error)) {
                    return false;
                }
                appendDiagnostics(diagnostics, decoded.diagnostics);
                result = std::move(decoded);
                return true;
            }
            if (type == "number" || type == "integer") {
                typed::McpElicitationNumberSchema decoded;
                if (!decodeMcpElicitationNumberSchema(value, decoded, path, error)) {
                    return false;
                }
                appendDiagnostics(diagnostics, decoded.diagnostics);
                result = std::move(decoded);
                return true;
            }
            if (type == "array") {
                const auto items = value.find("items");
                if (items == value.end() || !items->is_object()) {
                    error = std::string(Surface) + " expects required object at '" + std::string(path) + ".items'";
                    return false;
                }
                typed::McpElicitationEnumSchema enumSchema;
                if (items->contains("anyOf")) {
                    typed::McpElicitationTitledMultiSelectEnumSchema decoded;
                    if (!decodeMcpElicitationTitledMultiSelect(value, decoded, path, error)) {
                        return false;
                    }
                    appendDiagnostics(diagnostics, decoded.diagnostics);
                    enumSchema = std::move(decoded);
                } else if (items->contains("enum")) {
                    typed::McpElicitationUntitledMultiSelectEnumSchema decoded;
                    if (!decodeMcpElicitationUntitledMultiSelect(value, decoded, path, error)) {
                        return false;
                    }
                    appendDiagnostics(diagnostics, decoded.diagnostics);
                    enumSchema = std::move(decoded);
                } else {
                    error = std::string(Surface) + " expects a known enum-items shape at '" + std::string(path) + ".items'";
                    return false;
                }
                result = std::move(enumSchema);
                return true;
            }
            if (type == "string") {
                typed::McpElicitationEnumSchema enumSchema;
                if (value.contains("oneOf")) {
                    typed::McpElicitationTitledSingleSelectEnumSchema decoded;
                    if (!decodeMcpElicitationTitledSingleSelect(value, decoded, path, error)) {
                        return false;
                    }
                    appendDiagnostics(diagnostics, decoded.diagnostics);
                    enumSchema = std::move(decoded);
                    result = std::move(enumSchema);
                    return true;
                }
                if (value.contains("enumNames")) {
                    typed::McpElicitationLegacyTitledEnumSchema decoded;
                    if (!decodeMcpElicitationLegacyTitledEnum(value, decoded, path, error)) {
                        return false;
                    }
                    appendDiagnostics(diagnostics, decoded.diagnostics);
                    enumSchema = std::move(decoded);
                    result = std::move(enumSchema);
                    return true;
                }
                if (value.contains("enum")) {
                    typed::McpElicitationUntitledSingleSelectEnumSchema decoded;
                    if (!decodeMcpElicitationUntitledSingleSelect(value, decoded, path, error)) {
                        return false;
                    }
                    appendDiagnostics(diagnostics, decoded.diagnostics);
                    enumSchema = std::move(decoded);
                    result = std::move(enumSchema);
                    return true;
                }
                typed::McpElicitationStringSchema decoded;
                if (!decodeMcpElicitationStringSchema(value, decoded, path, error)) {
                    return false;
                }
                appendDiagnostics(diagnostics, decoded.diagnostics);
                result = std::move(decoded);
                return true;
            }

            typed::DecodeDiagnostic diagnostic = unknownEnumDiagnostic("McpElicitationPrimitiveSchema", std::string(path) + ".type");
            diagnostics.push_back(diagnostic);
            result = typed::UnknownMcpElicitationPrimitiveSchema{std::move(type), value, std::move(diagnostic)};
            return true;
        }

        bool decodeMcpElicitationSchema(const Json& value, typed::McpElicitationSchema& result, std::string_view path, std::string& error) {
            constexpr std::string_view Surface = "McpElicitationSchema";
            const Json* properties = nullptr;
            std::string type;
            if (!requireClosedObject(value, Surface, path, {"$schema", "properties", "required", "type"}, error) ||
                !optionalNullableStringAt(value, "$schema", Surface, path, result.schema, error) ||
                !requiredObjectAt(value, "properties", Surface, path, properties, error) ||
                !decodeOptionalNullableStringArray(value, "required", Surface, path, result.required, error) ||
                !requiredStringAt(value, "type", Surface, path, type, error)) {
                return false;
            }

            result.type = typed::McpElicitationObjectType{std::move(type)};
            result.diagnostics.clear();
            if (!result.type.isKnown()) {
                result.diagnostics.emplace_back(unknownEnumDiagnostic("McpElicitationObjectType", std::string(path) + ".type"));
            }

            result.properties.clear();
            for (auto member = properties->begin(); member != properties->end(); ++member) {
                typed::McpElicitationPrimitiveSchema primitive;
                if (!decodeMcpElicitationPrimitive(
                        member.value(), primitive, std::string(path) + ".properties[*]", result.diagnostics, error)) {
                    return false;
                }
                result.properties.emplace(member.key(), std::move(primitive));
            }
            result.raw = value;
            return true;
        }

        bool decodeToolRequestUserInputOption(const Json& value,
                                              typed::ToolRequestUserInputOption& result,
                                              std::string_view path,
                                              std::string& error) {
            constexpr std::string_view Surface = "ToolRequestUserInputOption";
            if (!requireObjectAt(value, Surface, path, error) ||
                !requiredStringAt(value, "description", Surface, path, result.description, error) ||
                !requiredStringAt(value, "label", Surface, path, result.label, error)) {
                return false;
            }
            result.raw = value;
            return true;
        }

        bool decodeToolRequestUserInputQuestion(const Json& value,
                                                typed::ToolRequestUserInputQuestion& result,
                                                std::string_view path,
                                                std::string& error) {
            constexpr std::string_view Surface = "ToolRequestUserInputQuestion";
            if (!requireObjectAt(value, Surface, path, error) || !requiredStringAt(value, "header", Surface, path, result.header, error) ||
                !requiredStringAt(value, "id", Surface, path, result.id, error) ||
                !optionalBoolean(value, "isOther", Surface, path, result.isOther, error) ||
                !optionalBoolean(value, "isSecret", Surface, path, result.isSecret, error) ||
                !requiredStringAt(value, "question", Surface, path, result.question, error)) {
                return false;
            }

            const auto options = value.find("options");
            if (options == value.end()) {
                result.options = typed::OptionalNullable<std::vector<typed::ToolRequestUserInputOption>>::omitted();
            } else if (options->is_null()) {
                result.options = typed::OptionalNullable<std::vector<typed::ToolRequestUserInputOption>>::explicitNull();
            } else if (!options->is_array()) {
                error = std::string(Surface) + " expects an array or null at '" + std::string(path) + ".options'";
                return false;
            } else {
                std::vector<typed::ToolRequestUserInputOption> decoded;
                decoded.reserve(options->size());
                for (std::size_t index = 0; index < options->size(); ++index) {
                    typed::ToolRequestUserInputOption option;
                    if (!decodeToolRequestUserInputOption((*options)[index], option, std::string(path) + ".options[*]", error)) {
                        return false;
                    }
                    decoded.push_back(std::move(option));
                }
                result.options = typed::OptionalNullable<std::vector<typed::ToolRequestUserInputOption>>::withValue(std::move(decoded));
            }
            result.raw = value;
            return true;
        }

        bool decodeToolRequestUserInputParamsValue(const Json& value, typed::ToolRequestUserInputParams& result, std::string& error) {
            constexpr std::string_view Surface = "ToolRequestUserInputParams";
            constexpr std::string_view Path = "$";
            const Json* questions = nullptr;
            std::string itemId;
            std::string threadId;
            std::string turnId;
            if (!requireObjectAt(value, Surface, Path, error) ||
                !decodeOptionalNullableUint64(value, "autoResolutionMs", Surface, Path, result.autoResolutionMs, error) ||
                !requiredStringAt(value, "itemId", Surface, Path, itemId, error) ||
                !requiredArrayAt(value, "questions", Surface, Path, questions, error) ||
                !requiredStringAt(value, "threadId", Surface, Path, threadId, error) ||
                !requiredStringAt(value, "turnId", Surface, Path, turnId, error)) {
                return false;
            }

            result.questions.clear();
            result.questions.reserve(questions->size());
            for (std::size_t index = 0; index < questions->size(); ++index) {
                typed::ToolRequestUserInputQuestion question;
                if (!decodeToolRequestUserInputQuestion((*questions)[index], question, "$.questions[*]", error)) {
                    return false;
                }
                result.questions.push_back(std::move(question));
            }
            result.itemId = typed::ItemId{std::move(itemId)};
            result.threadId = typed::ThreadId{std::move(threadId)};
            result.turnId = typed::TurnId{std::move(turnId)};
            result.raw = value;
            result.diagnostics.clear();
            return true;
        }

        bool decodeMcpServerElicitationRequestParamsValue(const Json& value,
                                                          typed::McpServerElicitationRequestParams& result,
                                                          std::string& error) {
            constexpr std::string_view Surface = "McpServerElicitationRequestParams";
            constexpr std::string_view Path = "$";
            if (!requireObjectAt(value, Surface, Path, error) ||
                !requiredStringAt(value, "serverName", Surface, Path, result.serverName, error)) {
                return false;
            }

            std::string threadId;
            if (!requiredStringAt(value, "threadId", Surface, Path, threadId, error)) {
                return false;
            }
            result.threadId = typed::ThreadId{std::move(threadId)};

            if (!optionalNullable<typed::TurnId>(
                    value,
                    "turnId",
                    Surface,
                    Path,
                    result.turnId,
                    error,
                    [surface = Surface](const Json& input, typed::TurnId& output, const std::string& fieldPath, std::string& nestedError) {
                        if (!input.is_string()) {
                            nestedError = std::string(surface) + " expects a string or null at '" + fieldPath + "'";
                            return false;
                        }
                        output = typed::TurnId{input.get<std::string>()};
                        return true;
                    })) {
                return false;
            }

            std::string mode;
            if (!requiredStringAt(value, "mode", Surface, Path, mode, error)) {
                return false;
            }

            result.diagnostics.clear();
            if (mode == "form") {
                typed::McpElicitationForm form;
                const auto requestedSchema = value.find("requestedSchema");
                if (!requiredStringAt(value, "message", Surface, Path, form.message, error)) {
                    return false;
                }
                if (requestedSchema == value.end()) {
                    error = std::string(Surface) + " is missing required object at '$.requestedSchema'";
                    return false;
                }
                if (!decodeMcpElicitationSchema(*requestedSchema, form.requestedSchema, "$.requestedSchema", error)) {
                    return false;
                }
                decodeOptionalNullableJson(value, "_meta", form.meta);
                form.raw = value;
                form.diagnostics = form.requestedSchema.diagnostics;
                appendDiagnostics(result.diagnostics, form.diagnostics);
                result.elicitation = std::move(form);
            } else if (mode == "openai/form") {
                typed::McpElicitationOpenAiForm form;
                const auto requestedSchema = value.find("requestedSchema");
                if (!requiredStringAt(value, "message", Surface, Path, form.message, error)) {
                    return false;
                }
                if (requestedSchema == value.end()) {
                    error = std::string(Surface) + " is missing required JSON value at '$.requestedSchema'";
                    return false;
                }
                form.requestedSchema = *requestedSchema;
                decodeOptionalNullableJson(value, "_meta", form.meta);
                form.raw = value;
                form.diagnostics.clear();
                result.elicitation = std::move(form);
            } else if (mode == "url") {
                typed::McpElicitationUrl url;
                if (!requiredStringAt(value, "elicitationId", Surface, Path, url.elicitationId, error) ||
                    !requiredStringAt(value, "message", Surface, Path, url.message, error) ||
                    !requiredStringAt(value, "url", Surface, Path, url.url, error)) {
                    return false;
                }
                decodeOptionalNullableJson(value, "_meta", url.meta);
                url.raw = value;
                url.diagnostics.clear();
                result.elicitation = std::move(url);
            } else {
                typed::DecodeDiagnostic diagnostic = unknownDiscriminatorDiagnostic(std::string(Surface), "$.mode");
                result.diagnostics.push_back(diagnostic);
                result.elicitation = typed::UnknownMcpElicitation{std::move(mode), value, std::move(diagnostic)};
            }

            result.raw = value;
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

        std::optional<Json> encodeToolRequestUserInputAnswer(const typed::ToolRequestUserInputAnswer& value, std::string& error) {
            Json result;
            if (!openObject(value.raw, "ToolRequestUserInputAnswer", result, error)) {
                return std::nullopt;
            }
            result["answers"] = value.answers;
            return std::optional<Json>{std::move(result)};
        }

        void encodeOptionalNullableJson(Json& result, const char* field, const typed::OptionalNullable<Json>& value) {
            result.erase(field);
            if (!value.present) {
                return;
            }
            result[field] = value.value ? *value.value : Json(nullptr);
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

    std::optional<typed::ToolRequestUserInputParams> decodeToolRequestUserInputParams(const Json& value, std::string& error) noexcept {
        constexpr std::string_view Surface = "ToolRequestUserInputParams";
        try {
            typed::ToolRequestUserInputParams result;
            if (!decodeToolRequestUserInputParamsValue(value, result, error)) {
                return std::nullopt;
            }
            error.clear();
            return std::optional<typed::ToolRequestUserInputParams>{std::move(result)};
        } catch (const std::exception&) {
            error = std::string(Surface) + " could not be decoded at '$'";
            return std::nullopt;
        } catch (...) {
            error = std::string(Surface) + " could not be decoded at '$'";
            return std::nullopt;
        }
    }

    std::optional<typed::McpServerElicitationRequestParams> decodeMcpServerElicitationRequestParams(const Json& value,
                                                                                                    std::string& error) noexcept {
        constexpr std::string_view Surface = "McpServerElicitationRequestParams";
        try {
            typed::McpServerElicitationRequestParams result;
            if (!decodeMcpServerElicitationRequestParamsValue(value, result, error)) {
                return std::nullopt;
            }
            error.clear();
            return std::optional<typed::McpServerElicitationRequestParams>{std::move(result)};
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

    std::optional<Json> encodeToolRequestUserInputResponse(const typed::ToolRequestUserInputResponse& value, std::string& error) noexcept {
        constexpr std::string_view Surface = "ToolRequestUserInputResponse";
        try {
            Json result;
            if (!openObject(value.raw, Surface, result, error)) {
                return std::nullopt;
            }
            Json answers = Json::object();
            for (const auto& [questionId, answer] : value.answers) {
                std::optional<Json> encoded = encodeToolRequestUserInputAnswer(answer, error);
                if (!encoded) {
                    return std::nullopt;
                }
                answers[questionId] = std::move(*encoded);
            }
            result["answers"] = std::move(answers);
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

    std::optional<Json> encodeMcpServerElicitationRequestResponse(const typed::McpServerElicitationRequestResponse& value,
                                                                  std::string& error) noexcept {
        constexpr std::string_view Surface = "McpServerElicitationRequestResponse";
        try {
            if (!value.action.isKnown()) {
                error = std::string(Surface) + " action is not one of accept, decline, or cancel";
                return std::nullopt;
            }
            Json result;
            if (!openObject(value.raw, Surface, result, error)) {
                return std::nullopt;
            }
            result.erase("action");
            result["action"] = value.action.value;
            encodeOptionalNullableJson(result, "content", value.content);
            encodeOptionalNullableJson(result, "_meta", value.meta);
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
