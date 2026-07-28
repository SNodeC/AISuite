/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/ExternalAgentCodec.h"

#include "ai/openai/codex/detail/DecodeDiagnostic.h"
#include "ai/openai/codex/typed/Types.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ai::openai::codex::detail {

    namespace {
        void expected(std::string& error, std::string_view surface, std::string_view path, std::string_view type) {
            error = std::string(surface) + " field '" + std::string(path) + "' must be " + std::string(type);
        }

        void missing(std::string& error, std::string_view surface, std::string_view path) {
            error = std::string(surface) + " is missing required field '" + std::string(path) + "'";
        }

        std::string fieldPath(std::string_view path, std::string_view field) {
            return std::string(path) + "." + std::string(field);
        }

        std::string indexPath(std::string_view path, std::size_t index) {
            return std::string(path) + "[" + std::to_string(index) + "]";
        }

        const Json* member(const Json& object, std::string_view field) {
            if (!object.is_object()) {
                return nullptr;
            }
            const auto found = object.find(std::string(field));
            return found == object.end() ? nullptr : &*found;
        }

        bool decodeObject(const Json& value,
                          std::string& error,
                          std::string_view surface,
                          std::string_view path) {
            if (!value.is_object()) {
                expected(error, surface, path, "an object");
                return false;
            }
            return true;
        }

        bool decodeString(const Json& value,
                          std::string& output,
                          std::string& error,
                          std::string_view surface,
                          std::string_view path) {
            if (!value.is_string()) {
                expected(error, surface, path, "a string");
                return false;
            }
            output = value.get<std::string>();
            return true;
        }

        bool decodeInt64(const Json& value,
                         std::int64_t& output,
                         std::string& error,
                         std::string_view surface,
                         std::string_view path) {
            if (value.is_number_unsigned()) {
                const std::uint64_t number = value.get<std::uint64_t>();
                if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    expected(error, surface, path, "an integer in the int64 range");
                    return false;
                }
                output = static_cast<std::int64_t>(number);
                return true;
            }
            if (value.is_number_integer()) {
                output = value.get<std::int64_t>();
                return true;
            }
            expected(error, surface, path, "an integer in the int64 range");
            return false;
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

        bool decodeStringArray(const Json& value,
                               std::vector<std::string>& output,
                               std::string& error,
                               std::string_view surface,
                               std::string_view path) {
            return decodeArray<std::string>(value, output, error, surface, path, decodeString);
        }

        template <typename T, typename Decoder>
        bool decodeRequired(const Json& object,
                            std::string_view field,
                            T& output,
                            std::string& error,
                            std::string_view surface,
                            std::string_view path,
                            Decoder&& decoder) {
            const std::string child = fieldPath(path, field);
            const Json* value = member(object, field);
            if (value == nullptr) {
                missing(error, surface, child);
                return false;
            }
            return decoder(*value, output, error, surface, child);
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

        bool decodeMigrationItemType(const Json& value,
                                     typed::ExternalAgentConfigMigrationItemType& output,
                                     std::string& error,
                                     std::string_view surface,
                                     std::string_view path,
                                     std::vector<typed::DecodeDiagnostic>& diagnostics) {
            if (!decodeString(value, output.value, error, surface, path)) {
                return false;
            }
            if (!output.isKnown()) {
                diagnostics.emplace_back(unknownEnumDiagnostic("ExternalAgentConfigMigrationItemType", std::string(path)));
            }
            return true;
        }

        template <typename Migration>
        bool decodeNamedMigration(const Json& value,
                                  Migration& output,
                                  std::string& error,
                                  std::string_view surface,
                                  std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeRequired(value, "name", output.name, error, surface, path, decodeString);
        }

        bool decodePluginsMigration(const Json& value,
                                    typed::PluginsMigration& output,
                                    std::string& error,
                                    std::string_view surface,
                                    std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeRequired(value, "marketplaceName", output.marketplaceName, error, surface, path, decodeString) &&
                   decodeRequired(value, "pluginNames", output.pluginNames, error, surface, path, decodeStringArray);
        }

        bool decodeSessionMigration(const Json& value,
                                    typed::SessionMigration& output,
                                    std::string& error,
                                    std::string_view surface,
                                    std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeRequired(value, "cwd", output.cwd, error, surface, path, decodeString) &&
                   decodeRequired(value, "path", output.path, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "title", output.title, error, surface, path, decodeString);
        }

        bool decodeMigrationDetails(const Json& value,
                                    typed::MigrationDetails& output,
                                    std::string& error,
                                    std::string_view surface,
                                    std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptional(
                       value,
                       "commands",
                       output.commands,
                       error,
                       surface,
                       path,
                       [](const Json& input,
                          std::vector<typed::CommandMigration>& decoded,
                          std::string& nestedError,
                          std::string_view nestedSurface,
                          std::string_view nestedPath) {
                           return decodeArray<typed::CommandMigration>(
                               input, decoded, nestedError, nestedSurface, nestedPath, decodeNamedMigration<typed::CommandMigration>);
                       }) &&
                   decodeOptional(
                       value,
                       "hooks",
                       output.hooks,
                       error,
                       surface,
                       path,
                       [](const Json& input,
                          std::vector<typed::HookMigration>& decoded,
                          std::string& nestedError,
                          std::string_view nestedSurface,
                          std::string_view nestedPath) {
                           return decodeArray<typed::HookMigration>(
                               input, decoded, nestedError, nestedSurface, nestedPath, decodeNamedMigration<typed::HookMigration>);
                       }) &&
                   decodeOptional(
                       value,
                       "mcpServers",
                       output.mcpServers,
                       error,
                       surface,
                       path,
                       [](const Json& input,
                          std::vector<typed::McpServerMigration>& decoded,
                          std::string& nestedError,
                          std::string_view nestedSurface,
                          std::string_view nestedPath) {
                           return decodeArray<typed::McpServerMigration>(
                               input, decoded, nestedError, nestedSurface, nestedPath, decodeNamedMigration<typed::McpServerMigration>);
                       }) &&
                   decodeOptional(
                       value,
                       "plugins",
                       output.plugins,
                       error,
                       surface,
                       path,
                       [](const Json& input,
                          std::vector<typed::PluginsMigration>& decoded,
                          std::string& nestedError,
                          std::string_view nestedSurface,
                          std::string_view nestedPath) {
                           return decodeArray<typed::PluginsMigration>(
                               input, decoded, nestedError, nestedSurface, nestedPath, decodePluginsMigration);
                       }) &&
                   decodeOptional(
                       value,
                       "sessions",
                       output.sessions,
                       error,
                       surface,
                       path,
                       [](const Json& input,
                          std::vector<typed::SessionMigration>& decoded,
                          std::string& nestedError,
                          std::string_view nestedSurface,
                          std::string_view nestedPath) {
                           return decodeArray<typed::SessionMigration>(
                               input, decoded, nestedError, nestedSurface, nestedPath, decodeSessionMigration);
                       }) &&
                   decodeOptional(
                       value,
                       "skills",
                       output.skills,
                       error,
                       surface,
                       path,
                       [](const Json& input,
                          std::vector<typed::SkillMigration>& decoded,
                          std::string& nestedError,
                          std::string_view nestedSurface,
                          std::string_view nestedPath) {
                           return decodeArray<typed::SkillMigration>(
                               input, decoded, nestedError, nestedSurface, nestedPath, decodeNamedMigration<typed::SkillMigration>);
                       }) &&
                   decodeOptional(
                       value,
                       "subagents",
                       output.subagents,
                       error,
                       surface,
                       path,
                       [](const Json& input,
                          std::vector<typed::SubagentMigration>& decoded,
                          std::string& nestedError,
                          std::string_view nestedSurface,
                          std::string_view nestedPath) {
                           return decodeArray<typed::SubagentMigration>(
                               input, decoded, nestedError, nestedSurface, nestedPath, decodeNamedMigration<typed::SubagentMigration>);
                       });
        }

        bool decodeMigrationItem(const Json& value,
                                 typed::ExternalAgentConfigMigrationItem& output,
                                 std::string& error,
                                 std::string_view surface,
                                 std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            if (!decodeOptionalNullable(value, "cwd", output.cwd, error, surface, path, decodeString) ||
                !decodeRequired(value, "description", output.description, error, surface, path, decodeString) ||
                !decodeOptionalNullable(value, "details", output.details, error, surface, path, decodeMigrationDetails) ||
                !decodeRequired(
                    value,
                    "itemType",
                    output.itemType,
                    error,
                    surface,
                    path,
                    [&](const Json& input,
                        typed::ExternalAgentConfigMigrationItemType& decoded,
                        std::string& nestedError,
                        std::string_view nestedSurface,
                        std::string_view nestedPath) {
                        return decodeMigrationItemType(
                            input, decoded, nestedError, nestedSurface, nestedPath, output.diagnostics);
                    })) {
                return false;
            }
            if (output.details.hasValue()) {
                appendDiagnostics(output, *output.details);
            }
            return true;
        }

        bool decodeImportFailure(const Json& value,
                                 typed::ExternalAgentConfigImportItemTypeFailure& output,
                                 std::string& error,
                                 std::string_view surface,
                                 std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptionalNullable(value, "cwd", output.cwd, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "errorType", output.errorType, error, surface, path, decodeString) &&
                   decodeRequired(value, "failureStage", output.failureStage, error, surface, path, decodeString) &&
                   decodeRequired(
                       value,
                       "itemType",
                       output.itemType,
                       error,
                       surface,
                       path,
                       [&](const Json& input,
                           typed::ExternalAgentConfigMigrationItemType& decoded,
                           std::string& nestedError,
                           std::string_view nestedSurface,
                           std::string_view nestedPath) {
                           return decodeMigrationItemType(
                               input, decoded, nestedError, nestedSurface, nestedPath, output.diagnostics);
                       }) &&
                   decodeRequired(value, "message", output.message, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "source", output.source, error, surface, path, decodeString);
        }

        bool decodeImportSuccess(const Json& value,
                                 typed::ExternalAgentConfigImportItemTypeSuccess& output,
                                 std::string& error,
                                 std::string_view surface,
                                 std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptionalNullable(value, "cwd", output.cwd, error, surface, path, decodeString) &&
                   decodeRequired(
                       value,
                       "itemType",
                       output.itemType,
                       error,
                       surface,
                       path,
                       [&](const Json& input,
                           typed::ExternalAgentConfigMigrationItemType& decoded,
                           std::string& nestedError,
                           std::string_view nestedSurface,
                           std::string_view nestedPath) {
                           return decodeMigrationItemType(
                               input, decoded, nestedError, nestedSurface, nestedPath, output.diagnostics);
                       }) &&
                   decodeOptionalNullable(value, "source", output.source, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "target", output.target, error, surface, path, decodeString);
        }

        bool decodeImportTypeResult(const Json& value,
                                    typed::ExternalAgentConfigImportTypeResult& output,
                                    std::string& error,
                                    std::string_view surface,
                                    std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            if (!decodeRequired(
                    value,
                    "failures",
                    output.failures,
                    error,
                    surface,
                    path,
                    [](const Json& input,
                       std::vector<typed::ExternalAgentConfigImportItemTypeFailure>& decoded,
                       std::string& nestedError,
                       std::string_view nestedSurface,
                       std::string_view nestedPath) {
                        return decodeArray<typed::ExternalAgentConfigImportItemTypeFailure>(
                            input, decoded, nestedError, nestedSurface, nestedPath, decodeImportFailure);
                    }) ||
                !decodeRequired(
                    value,
                    "itemType",
                    output.itemType,
                    error,
                    surface,
                    path,
                    [&](const Json& input,
                        typed::ExternalAgentConfigMigrationItemType& decoded,
                        std::string& nestedError,
                        std::string_view nestedSurface,
                        std::string_view nestedPath) {
                        return decodeMigrationItemType(
                            input, decoded, nestedError, nestedSurface, nestedPath, output.diagnostics);
                    }) ||
                !decodeRequired(
                    value,
                    "successes",
                    output.successes,
                    error,
                    surface,
                    path,
                    [](const Json& input,
                       std::vector<typed::ExternalAgentConfigImportItemTypeSuccess>& decoded,
                       std::string& nestedError,
                       std::string_view nestedSurface,
                       std::string_view nestedPath) {
                        return decodeArray<typed::ExternalAgentConfigImportItemTypeSuccess>(
                            input, decoded, nestedError, nestedSurface, nestedPath, decodeImportSuccess);
                    })) {
                return false;
            }
            for (const auto& failure : output.failures) {
                appendDiagnostics(output, failure);
            }
            for (const auto& success : output.successes) {
                appendDiagnostics(output, success);
            }
            return true;
        }

        bool decodeImportHistory(const Json& value,
                                 typed::ExternalAgentConfigImportHistory& output,
                                 std::string& error,
                                 std::string_view surface,
                                 std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            if (!decodeRequired(value, "completedAtMs", output.completedAtMs, error, surface, path, decodeInt64) ||
                !decodeRequired(
                    value,
                    "failures",
                    output.failures,
                    error,
                    surface,
                    path,
                    [](const Json& input,
                       std::vector<typed::ExternalAgentConfigImportItemTypeFailure>& decoded,
                       std::string& nestedError,
                       std::string_view nestedSurface,
                       std::string_view nestedPath) {
                        return decodeArray<typed::ExternalAgentConfigImportItemTypeFailure>(
                            input, decoded, nestedError, nestedSurface, nestedPath, decodeImportFailure);
                    }) ||
                !decodeRequired(value, "importId", output.importId, error, surface, path, decodeString) ||
                !decodeRequired(
                    value,
                    "successes",
                    output.successes,
                    error,
                    surface,
                    path,
                    [](const Json& input,
                       std::vector<typed::ExternalAgentConfigImportItemTypeSuccess>& decoded,
                       std::string& nestedError,
                       std::string_view nestedSurface,
                       std::string_view nestedPath) {
                        return decodeArray<typed::ExternalAgentConfigImportItemTypeSuccess>(
                            input, decoded, nestedError, nestedSurface, nestedPath, decodeImportSuccess);
                    })) {
                return false;
            }
            for (const auto& failure : output.failures) {
                appendDiagnostics(output, failure);
            }
            for (const auto& success : output.successes) {
                appendDiagnostics(output, success);
            }
            return true;
        }

        Json encoderObject(const Json& raw,
                           std::string& error,
                           std::string_view surface,
                           std::string_view path,
                           std::initializer_list<std::string_view> knownFields) {
            if (!raw.is_object()) {
                expected(error, surface, path, "an object");
                return Json();
            }
            Json result = raw;
            for (const std::string_view field : knownFields) {
                result.erase(std::string(field));
            }
            return result;
        }

        template <typename T, typename Encoder>
        bool encodeOptionalNullable(Json& object,
                                    std::string_view field,
                                    const typed::OptionalNullable<T>& value,
                                    std::string& error,
                                    std::string_view surface,
                                    Encoder&& encoder) {
            if (!value.present && value.value.has_value()) {
                error = std::string(surface) + " field '$." + std::string(field) + "' has an inconsistent omission state";
                return false;
            }
            if (value.isOmitted()) {
                return true;
            }
            if (value.isNull()) {
                object[std::string(field)] = nullptr;
                return true;
            }
            std::optional<Json> encoded = encoder(*value, error);
            if (!encoded) {
                return false;
            }
            object[std::string(field)] = std::move(*encoded);
            return true;
        }

        std::optional<Json> encodeString(const std::string& value, std::string&) {
            return std::optional<Json>{Json(value)};
        }

        std::optional<Json> encodeStringArray(const std::vector<std::string>& values, std::string&) {
            Json result = Json::array();
            for (const auto& value : values) {
                result.push_back(value);
            }
            return std::optional<Json>{std::move(result)};
        }

        template <typename T, typename Encoder>
        std::optional<Json> encodeArray(const std::vector<T>& values, std::string& error, Encoder&& encoder) {
            Json result = Json::array();
            for (const auto& value : values) {
                std::optional<Json> encoded = encoder(value, error);
                if (!encoded) {
                    return std::nullopt;
                }
                result.push_back(std::move(*encoded));
            }
            return std::optional<Json>{std::move(result)};
        }

        template <typename Migration>
        std::optional<Json> encodeNamedMigration(const Migration& value, std::string& error, std::string_view surface) {
            Json result = encoderObject(value.raw, error, surface, "$.raw", {"name"});
            if (!error.empty()) {
                return std::nullopt;
            }
            result["name"] = value.name;
            return std::optional<Json>{std::move(result)};
        }

        std::optional<Json> encodePluginsMigration(const typed::PluginsMigration& value, std::string& error) {
            Json result = encoderObject(value.raw, error, "PluginsMigration", "$.raw", {"marketplaceName", "pluginNames"});
            if (!error.empty()) {
                return std::nullopt;
            }
            result["marketplaceName"] = value.marketplaceName;
            result["pluginNames"] = *encodeStringArray(value.pluginNames, error);
            return std::optional<Json>{std::move(result)};
        }

        std::optional<Json> encodeSessionMigration(const typed::SessionMigration& value, std::string& error) {
            Json result = encoderObject(value.raw, error, "SessionMigration", "$.raw", {"cwd", "path", "title"});
            if (!error.empty()) {
                return std::nullopt;
            }
            result["cwd"] = value.cwd;
            result["path"] = value.path;
            if (!encodeOptionalNullable(result, "title", value.title, error, "SessionMigration", encodeString)) {
                return std::nullopt;
            }
            return std::optional<Json>{std::move(result)};
        }

        std::optional<Json> encodeMigrationDetails(const typed::MigrationDetails& value, std::string& error) {
            Json result = encoderObject(
                value.raw, error, "MigrationDetails", "$.raw", {"commands", "hooks", "mcpServers", "plugins", "sessions", "skills", "subagents"});
            if (!error.empty()) {
                return std::nullopt;
            }
            if (value.commands) {
                auto encoded = encodeArray(
                    *value.commands,
                    error,
                    [](const typed::CommandMigration& item, std::string& nestedError) {
                        return encodeNamedMigration(item, nestedError, "CommandMigration");
                    });
                if (!encoded) {
                    return std::nullopt;
                }
                result["commands"] = std::move(*encoded);
            }
            if (value.hooks) {
                auto encoded = encodeArray(
                    *value.hooks,
                    error,
                    [](const typed::HookMigration& item, std::string& nestedError) {
                        return encodeNamedMigration(item, nestedError, "HookMigration");
                    });
                if (!encoded) {
                    return std::nullopt;
                }
                result["hooks"] = std::move(*encoded);
            }
            if (value.mcpServers) {
                auto encoded = encodeArray(
                    *value.mcpServers,
                    error,
                    [](const typed::McpServerMigration& item, std::string& nestedError) {
                        return encodeNamedMigration(item, nestedError, "McpServerMigration");
                    });
                if (!encoded) {
                    return std::nullopt;
                }
                result["mcpServers"] = std::move(*encoded);
            }
            if (value.plugins) {
                auto encoded = encodeArray(*value.plugins, error, encodePluginsMigration);
                if (!encoded) {
                    return std::nullopt;
                }
                result["plugins"] = std::move(*encoded);
            }
            if (value.sessions) {
                auto encoded = encodeArray(*value.sessions, error, encodeSessionMigration);
                if (!encoded) {
                    return std::nullopt;
                }
                result["sessions"] = std::move(*encoded);
            }
            if (value.skills) {
                auto encoded = encodeArray(
                    *value.skills,
                    error,
                    [](const typed::SkillMigration& item, std::string& nestedError) {
                        return encodeNamedMigration(item, nestedError, "SkillMigration");
                    });
                if (!encoded) {
                    return std::nullopt;
                }
                result["skills"] = std::move(*encoded);
            }
            if (value.subagents) {
                auto encoded = encodeArray(
                    *value.subagents,
                    error,
                    [](const typed::SubagentMigration& item, std::string& nestedError) {
                        return encodeNamedMigration(item, nestedError, "SubagentMigration");
                    });
                if (!encoded) {
                    return std::nullopt;
                }
                result["subagents"] = std::move(*encoded);
            }
            return std::optional<Json>{std::move(result)};
        }

        std::optional<Json> encodeMigrationItem(const typed::ExternalAgentConfigMigrationItem& value, std::string& error) {
            Json result = encoderObject(
                value.raw, error, "ExternalAgentConfigMigrationItem", "$.raw", {"cwd", "description", "details", "itemType"});
            if (!error.empty()) {
                return std::nullopt;
            }
            result["description"] = value.description;
            result["itemType"] = value.itemType.value;
            if (!encodeOptionalNullable(result, "cwd", value.cwd, error, "ExternalAgentConfigMigrationItem", encodeString) ||
                !encodeOptionalNullable(
                    result, "details", value.details, error, "ExternalAgentConfigMigrationItem", encodeMigrationDetails)) {
                return std::nullopt;
            }
            return std::optional<Json>{std::move(result)};
        }

        template <typename NotificationType>
        std::optional<NotificationType> decodeImportNotification(const Notification& notification,
                                                                 std::string& error,
                                                                 std::string_view surface) {
            if (!decodeObject(notification.params, error, surface, "$.params")) {
                return std::nullopt;
            }
            NotificationType result;
            result.raw = notification.raw;
            if (!decodeRequired(notification.params, "importId", result.importId, error, surface, "$.params", decodeString) ||
                !decodeRequired(
                    notification.params,
                    "itemTypeResults",
                    result.itemTypeResults,
                    error,
                    surface,
                    "$.params",
                    [](const Json& input,
                       std::vector<typed::ExternalAgentConfigImportTypeResult>& decoded,
                       std::string& nestedError,
                       std::string_view nestedSurface,
                       std::string_view nestedPath) {
                        return decodeArray<typed::ExternalAgentConfigImportTypeResult>(
                            input, decoded, nestedError, nestedSurface, nestedPath, decodeImportTypeResult);
                    })) {
                return std::nullopt;
            }
            for (const auto& item : result.itemTypeResults) {
                appendDiagnostics(result, item);
            }
            return std::optional<NotificationType>{std::move(result)};
        }
    } // namespace

    std::optional<Json>
    encodeExternalAgentConfigDetectParams(const typed::ExternalAgentConfigDetectParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(
                value.raw, error, "ExternalAgentConfigDetectParams", "$.raw", {"cwds", "includeHome"});
            if (!error.empty()) {
                return std::nullopt;
            }
            if (!encodeOptionalNullable(
                    result, "cwds", value.cwds, error, "ExternalAgentConfigDetectParams", encodeStringArray)) {
                return std::nullopt;
            }
            if (value.includeHome) {
                result["includeHome"] = *value.includeHome;
            }
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            error = "ExternalAgentConfigDetectParams encoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<Json>
    encodeExternalAgentConfigImportParams(const typed::ExternalAgentConfigImportParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(
                value.raw, error, "ExternalAgentConfigImportParams", "$.raw", {"migrationItems", "source"});
            if (!error.empty()) {
                return std::nullopt;
            }
            auto migrationItems = encodeArray(value.migrationItems, error, encodeMigrationItem);
            if (!migrationItems ||
                !encodeOptionalNullable(result, "source", value.source, error, "ExternalAgentConfigImportParams", encodeString)) {
                return std::nullopt;
            }
            result["migrationItems"] = std::move(*migrationItems);
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            error = "ExternalAgentConfigImportParams encoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::ExternalAgentConfigDetectResponse>
    decodeExternalAgentConfigDetectResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            if (!decodeObject(value, error, "ExternalAgentConfigDetectResponse", "$")) {
                return std::nullopt;
            }
            typed::ExternalAgentConfigDetectResponse result;
            result.raw = value;
            if (!decodeRequired(
                    value,
                    "items",
                    result.items,
                    error,
                    "ExternalAgentConfigDetectResponse",
                    "$",
                    [](const Json& input,
                       std::vector<typed::ExternalAgentConfigMigrationItem>& decoded,
                       std::string& nestedError,
                       std::string_view nestedSurface,
                       std::string_view nestedPath) {
                        return decodeArray<typed::ExternalAgentConfigMigrationItem>(
                            input, decoded, nestedError, nestedSurface, nestedPath, decodeMigrationItem);
                    })) {
                return std::nullopt;
            }
            for (const auto& item : result.items) {
                appendDiagnostics(result, item);
            }
            return result;
        } catch (...) {
            error = "ExternalAgentConfigDetectResponse decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::ExternalAgentConfigImportResponse>
    decodeExternalAgentConfigImportResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            if (!decodeObject(value, error, "ExternalAgentConfigImportResponse", "$")) {
                return std::nullopt;
            }
            typed::ExternalAgentConfigImportResponse result;
            result.raw = value;
            if (!decodeRequired(value, "importId", result.importId, error, "ExternalAgentConfigImportResponse", "$", decodeString)) {
                return std::nullopt;
            }
            return result;
        } catch (...) {
            error = "ExternalAgentConfigImportResponse decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::ExternalAgentConfigImportHistoriesReadResponse>
    decodeExternalAgentConfigImportHistoriesReadResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            if (!decodeObject(value, error, "ExternalAgentConfigImportHistoriesReadResponse", "$")) {
                return std::nullopt;
            }
            typed::ExternalAgentConfigImportHistoriesReadResponse result;
            result.raw = value;
            if (!decodeRequired(
                    value,
                    "data",
                    result.data,
                    error,
                    "ExternalAgentConfigImportHistoriesReadResponse",
                    "$",
                    [](const Json& input,
                       std::vector<typed::ExternalAgentConfigImportHistory>& decoded,
                       std::string& nestedError,
                       std::string_view nestedSurface,
                       std::string_view nestedPath) {
                        return decodeArray<typed::ExternalAgentConfigImportHistory>(
                            input, decoded, nestedError, nestedSurface, nestedPath, decodeImportHistory);
                    })) {
                return std::nullopt;
            }
            for (const auto& history : result.data) {
                appendDiagnostics(result, history);
            }
            return result;
        } catch (...) {
            error = "ExternalAgentConfigImportHistoriesReadResponse decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::ExternalAgentConfigImportCompletedNotification>
    decodeExternalAgentConfigImportCompletedNotification(const Notification& notification, std::string& error) noexcept {
        try {
            error.clear();
            return decodeImportNotification<typed::ExternalAgentConfigImportCompletedNotification>(
                notification, error, "externalAgentConfig/import/completed");
        } catch (...) {
            error = "externalAgentConfig/import/completed decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::ExternalAgentConfigImportProgressNotification>
    decodeExternalAgentConfigImportProgressNotification(const Notification& notification, std::string& error) noexcept {
        try {
            error.clear();
            return decodeImportNotification<typed::ExternalAgentConfigImportProgressNotification>(
                notification, error, "externalAgentConfig/import/progress");
        } catch (...) {
            error = "externalAgentConfig/import/progress decoding failed safely";
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::detail
