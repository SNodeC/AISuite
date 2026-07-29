/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/SkillCodec.h"

#include "ai/openai/codex/detail/DecodeDiagnostic.h"
#include "ai/openai/codex/typed/Types.h"

#include <cstddef>
#include <initializer_list>
#include <map>
#include <nlohmann/json.hpp>
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

        std::string fieldPath(std::string_view base, std::string_view name) {
            std::string result(base.empty() ? "$" : base);
            if (!name.empty()) {
                result.push_back('.');
                result.append(name);
            }
            return result;
        }

        std::string indexedPath(std::string_view base, std::size_t index) {
            return std::string(base) + "[" + std::to_string(index) + "]";
        }

        bool fail(std::string& error, std::string_view context, std::string_view path, std::string_view requirement) {
            error = std::string(context) + " field '" + std::string(path) + "' " + std::string(requirement);
            return false;
        }

        bool requireObject(const Json& value, std::string_view context, std::string& error, std::string_view path = "$") {
            return value.is_object() || fail(error, context, path, "must be an object");
        }

        bool decodeStringAt(const Json& value, std::string& result, std::string& error, std::string_view context, std::string_view path) {
            if (!value.is_string()) {
                return fail(error, context, path, "must be a string");
            }
            result = value.get_ref<const std::string&>();
            return true;
        }

        bool decodeBoolAt(const Json& value, bool& result, std::string& error, std::string_view context, std::string_view path) {
            if (!value.is_boolean()) {
                return fail(error, context, path, "must be a boolean");
            }
            result = value.get_ref<const Json::boolean_t&>();
            return true;
        }

        template <typename Strong>
        bool decodeStrongStringAt(const Json& value, Strong& result, std::string& error, std::string_view context, std::string_view path) {
            return decodeStringAt(value, result.value, error, context, path);
        }

        template <typename OpenEnum>
        bool decodeOpenEnumAt(const Json& value,
                              OpenEnum& result,
                              std::vector<typed::DecodeDiagnostic>& diagnostics,
                              std::string_view surface,
                              std::string_view path,
                              std::string& error,
                              std::string_view context) {
            if (!decodeStringAt(value, result.value, error, context, path)) {
                return false;
            }
            if (!result.isKnown()) {
                diagnostics.emplace_back(unknownEnumDiagnostic(std::string(surface), std::string(path)));
            }
            return true;
        }

        void appendDiagnostics(std::vector<typed::DecodeDiagnostic>& target, const std::vector<typed::DecodeDiagnostic>& source) {
            target.insert(target.end(), source.begin(), source.end());
        }

        template <typename T, typename Decode>
        bool decodeRequired(const Json& object,
                            std::string_view name,
                            T& result,
                            Decode&& decode,
                            std::string& error,
                            std::string_view context,
                            std::string_view path = "$") {
            const std::string nestedPath = fieldPath(path, name);
            const Json* value = member(object, name);
            if (value == nullptr) {
                return fail(error, context, nestedPath, "is required");
            }
            return decode(*value, result, nestedPath);
        }

        template <typename T, typename Decode>
        bool decodeOptionalNullable(const Json& object,
                                    std::string_view name,
                                    typed::OptionalNullable<T>& result,
                                    Decode&& decode,
                                    std::string& error,
                                    std::string_view context,
                                    std::string_view path = "$") {
            result = typed::OptionalNullable<T>::omitted();
            const Json* value = member(object, name);
            if (value == nullptr) {
                return true;
            }
            if (value->is_null()) {
                result = typed::OptionalNullable<T>::explicitNull();
                return true;
            }
            T decoded;
            const std::string nestedPath = fieldPath(path, name);
            if (!decode(*value, decoded, nestedPath)) {
                if (error.empty()) {
                    return fail(error, context, nestedPath, "has the wrong type");
                }
                return false;
            }
            result = typed::OptionalNullable<T>::withValue(std::move(decoded));
            return true;
        }

        template <typename T, typename Decode>
        bool decodeArrayAt(const Json& value,
                           std::vector<T>& result,
                           Decode&& decode,
                           std::string& error,
                           std::string_view context,
                           std::string_view path) {
            if (!value.is_array()) {
                return fail(error, context, path, "must be an array");
            }
            result.clear();
            result.reserve(value.size());
            for (std::size_t index = 0; index < value.size(); ++index) {
                T decoded;
                const std::string itemPath = indexedPath(path, index);
                if (!decode(value[index], decoded, itemPath)) {
                    if (error.empty()) {
                        return fail(error, context, itemPath, "has the wrong type");
                    }
                    return false;
                }
                result.emplace_back(std::move(decoded));
            }
            return true;
        }

        template <typename T>
        bool validateOptionalNullable(const typed::OptionalNullable<T>& value,
                                      std::string& error,
                                      std::string_view context,
                                      std::string_view path) {
            if (!value.present && value.value.has_value()) {
                return fail(error, context, path, "has an inconsistent omitted state");
            }
            return true;
        }

        Json
        encoderObject(const Json& raw, std::string& error, std::string_view context, std::initializer_list<std::string_view> knownFields) {
            if (!raw.is_object()) {
                fail(error, context, "$.raw", "must be an object");
                return Json();
            }
            Json result = raw;
            for (const std::string_view field : knownFields) {
                result.erase(std::string(field));
            }
            return result;
        }

        template <typename T, typename Encode>
        void encodeOptionalNullable(Json& object, std::string_view name, const typed::OptionalNullable<T>& value, Encode&& encode) {
            if (!value.present) {
                return;
            }
            object[std::string(name)] = value.value ? Json(encode(*value.value)) : Json(nullptr);
        }

        bool decodeSkillToolDependency(const Json& value, typed::SkillToolDependency& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "SkillToolDependency";
            const auto stringDecoder = [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                return decodeStringAt(item, decoded, error, Context, itemPath);
            };
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(value, "command", result.command, stringDecoder, error, Context, path) ||
                !decodeOptionalNullable(value, "description", result.description, stringDecoder, error, Context, path) ||
                !decodeOptionalNullable(value, "transport", result.transport, stringDecoder, error, Context, path) ||
                !decodeRequired(value, "type", result.type, stringDecoder, error, Context, path) ||
                !decodeOptionalNullable(value, "url", result.url, stringDecoder, error, Context, path) ||
                !decodeRequired(value, "value", result.value, stringDecoder, error, Context, path)) {
                return false;
            }
            result.raw = value;
            return true;
        }

        bool decodeSkillDependencies(const Json& value, typed::SkillDependencies& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "SkillDependencies";
            if (!requireObject(value, Context, error, path)) {
                return false;
            }
            const Json* tools = member(value, "tools");
            if (tools == nullptr) {
                return fail(error, Context, fieldPath(path, "tools"), "is required");
            }
            if (!decodeArrayAt(
                    *tools,
                    result.tools,
                    [&](const Json& item, typed::SkillToolDependency& decoded, const std::string& itemPath) {
                        if (!decodeSkillToolDependency(item, decoded, error, itemPath)) {
                            return false;
                        }
                        appendDiagnostics(result.diagnostics, decoded.diagnostics);
                        return true;
                    },
                    error,
                    Context,
                    fieldPath(path, "tools"))) {
                return false;
            }
            result.raw = value;
            return true;
        }

        bool decodeSkillErrorInfo(const Json& value, typed::SkillErrorInfo& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "SkillErrorInfo";
            const auto stringDecoder = [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                return decodeStringAt(item, decoded, error, Context, itemPath);
            };
            if (!requireObject(value, Context, error, path) ||
                !decodeRequired(value, "message", result.message, stringDecoder, error, Context, path) ||
                !decodeRequired(value, "path", result.path, stringDecoder, error, Context, path)) {
                return false;
            }
            result.raw = value;
            return true;
        }

        bool decodeSkillInterface(const Json& value, typed::SkillInterface& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "SkillInterface";
            const auto stringDecoder = [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                return decodeStringAt(item, decoded, error, Context, itemPath);
            };
            const auto pathDecoder = [&](const Json& item, typed::AbsolutePathBuf& decoded, std::string_view itemPath) {
                return decodeStrongStringAt(item, decoded, error, Context, itemPath);
            };
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(value, "brandColor", result.brandColor, stringDecoder, error, Context, path) ||
                !decodeOptionalNullable(value, "defaultPrompt", result.defaultPrompt, stringDecoder, error, Context, path) ||
                !decodeOptionalNullable(value, "displayName", result.displayName, stringDecoder, error, Context, path) ||
                !decodeOptionalNullable(value, "iconLarge", result.iconLarge, pathDecoder, error, Context, path) ||
                !decodeOptionalNullable(value, "iconSmall", result.iconSmall, pathDecoder, error, Context, path) ||
                !decodeOptionalNullable(value, "shortDescription", result.shortDescription, stringDecoder, error, Context, path)) {
                return false;
            }
            result.raw = value;
            return true;
        }

        bool decodeSkillMetadata(const Json& value, typed::SkillMetadata& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "SkillMetadata";
            const auto stringDecoder = [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                return decodeStringAt(item, decoded, error, Context, itemPath);
            };
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(
                    value,
                    "dependencies",
                    result.dependencies,
                    [&](const Json& item, typed::SkillDependencies& decoded, std::string_view itemPath) {
                        return decodeSkillDependencies(item, decoded, error, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(value, "description", result.description, stringDecoder, error, Context, path) ||
                !decodeRequired(
                    value,
                    "enabled",
                    result.enabled,
                    [&](const Json& item, bool& decoded, std::string_view itemPath) {
                        return decodeBoolAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "interface",
                    result.interface,
                    [&](const Json& item, typed::SkillInterface& decoded, std::string_view itemPath) {
                        return decodeSkillInterface(item, decoded, error, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(value, "name", result.name, stringDecoder, error, Context, path) ||
                !decodeRequired(
                    value,
                    "path",
                    result.path,
                    [&](const Json& item, typed::AbsolutePathBuf& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "scope",
                    result.scope,
                    [&](const Json& item, typed::SkillScope& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "SkillScope", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(value, "shortDescription", result.shortDescription, stringDecoder, error, Context, path)) {
                return false;
            }
            if (result.dependencies.value) {
                appendDiagnostics(result.diagnostics, result.dependencies.value->diagnostics);
            }
            if (result.interface.value) {
                appendDiagnostics(result.diagnostics, result.interface.value->diagnostics);
            }
            result.raw = value;
            return true;
        }

        bool decodeSkillsListEntry(const Json& value, typed::SkillsListEntry& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "SkillsListEntry";
            if (!requireObject(value, Context, error, path) || !decodeRequired(
                                                                   value,
                                                                   "cwd",
                                                                   result.cwd,
                                                                   [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                                                                       return decodeStringAt(item, decoded, error, Context, itemPath);
                                                                   },
                                                                   error,
                                                                   Context,
                                                                   path)) {
                return false;
            }

            const Json* errors = member(value, "errors");
            const Json* skills = member(value, "skills");
            if (errors == nullptr) {
                return fail(error, Context, fieldPath(path, "errors"), "is required");
            }
            if (!decodeArrayAt(
                    *errors,
                    result.errors,
                    [&](const Json& item, typed::SkillErrorInfo& decoded, const std::string& itemPath) {
                        if (!decodeSkillErrorInfo(item, decoded, error, itemPath)) {
                            return false;
                        }
                        appendDiagnostics(result.diagnostics, decoded.diagnostics);
                        return true;
                    },
                    error,
                    Context,
                    fieldPath(path, "errors"))) {
                return false;
            }
            if (skills == nullptr) {
                return fail(error, Context, fieldPath(path, "skills"), "is required");
            }
            if (!decodeArrayAt(
                    *skills,
                    result.skills,
                    [&](const Json& item, typed::SkillMetadata& decoded, const std::string& itemPath) {
                        if (!decodeSkillMetadata(item, decoded, error, itemPath)) {
                            return false;
                        }
                        appendDiagnostics(result.diagnostics, decoded.diagnostics);
                        return true;
                    },
                    error,
                    Context,
                    fieldPath(path, "skills"))) {
                return false;
            }
            result.raw = value;
            return true;
        }

        void setUnexpectedFailure(std::string& error, std::string_view context) noexcept {
            try {
                error = std::string(context) + " failed while processing JSON";
            } catch (...) {
            }
        }

    } // namespace

    std::optional<Json> encodeSkillsConfigWriteParams(const typed::SkillsConfigWriteParams& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "SkillsConfigWriteParams";
            if (!validateOptionalNullable(value.name, error, Context, "$.name") ||
                !validateOptionalNullable(value.path, error, Context, "$.path")) {
                return std::nullopt;
            }
            Json result = encoderObject(value.raw, error, Context, {"enabled", "name", "path"});
            if (!error.empty()) {
                return std::nullopt;
            }
            result["enabled"] = value.enabled;
            encodeOptionalNullable(result, "name", value.name, [](const std::string& item) {
                return Json(item);
            });
            encodeOptionalNullable(result, "path", value.path, [](const typed::AbsolutePathBuf& item) {
                return Json(item.value);
            });
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "SkillsConfigWriteParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodeSkillsExtraRootsSetParams(const typed::SkillsExtraRootsSetParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(value.raw, error, "SkillsExtraRootsSetParams", {"extraRoots"});
            if (!error.empty()) {
                return std::nullopt;
            }
            Json roots = Json::array();
            for (const typed::AbsolutePathBuf& root : value.extraRoots) {
                roots.push_back(root.value);
            }
            result["extraRoots"] = std::move(roots);
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "SkillsExtraRootsSetParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodeSkillsListParams(const typed::SkillsListParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(value.raw, error, "SkillsListParams", {"cwds", "forceReload"});
            if (!error.empty()) {
                return std::nullopt;
            }
            if (value.cwds) {
                result["cwds"] = *value.cwds;
            }
            if (value.forceReload) {
                result["forceReload"] = *value.forceReload;
            }
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "SkillsListParams");
            return std::nullopt;
        }
    }

    std::optional<typed::SkillsConfigWriteResponse> decodeSkillsConfigWriteResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "SkillsConfigWriteResponse";
            typed::SkillsConfigWriteResponse result;
            if (!requireObject(value, Context, error) || !decodeRequired(
                                                             value,
                                                             "effectiveEnabled",
                                                             result.effectiveEnabled,
                                                             [&](const Json& item, bool& decoded, std::string_view itemPath) {
                                                                 return decodeBoolAt(item, decoded, error, Context, itemPath);
                                                             },
                                                             error,
                                                             Context)) {
                return std::nullopt;
            }
            result.raw = value;
            error.clear();
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "SkillsConfigWriteResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::SkillsListResponse> decodeSkillsListResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "SkillsListResponse";
            typed::SkillsListResponse result;
            if (!requireObject(value, Context, error)) {
                return std::nullopt;
            }
            const Json* data = member(value, "data");
            if (data == nullptr) {
                fail(error, Context, "$.data", "is required");
                return std::nullopt;
            }
            if (!decodeArrayAt(
                    *data,
                    result.data,
                    [&](const Json& item, typed::SkillsListEntry& decoded, const std::string& itemPath) {
                        if (!decodeSkillsListEntry(item, decoded, error, itemPath)) {
                            return false;
                        }
                        appendDiagnostics(result.diagnostics, decoded.diagnostics);
                        return true;
                    },
                    error,
                    Context,
                    "$.data")) {
                return std::nullopt;
            }
            result.raw = value;
            error.clear();
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "SkillsListResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::SkillsChangedNotification> decodeSkillsChangedNotification(const Notification& notification,
                                                                                    std::string& error) noexcept {
        try {
            error.clear();
            if (!requireObject(notification.params, "SkillsChangedNotification", error, "$.params")) {
                return std::nullopt;
            }
            typed::SkillsChangedNotification result;
            result.raw = notification.raw;
            error.clear();
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "SkillsChangedNotification");
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::detail
