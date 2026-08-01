/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/HookCodec.h"

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

        bool decodeInt64At(const Json& value, std::int64_t& result, std::string& error, std::string_view context, std::string_view path) {
            if (value.is_number_unsigned()) {
                const std::uint64_t number = value.get_ref<const Json::number_unsigned_t&>();
                if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    return fail(error, context, path, "is outside the int64 range");
                }
                result = static_cast<std::int64_t>(number);
                return true;
            }
            if (!value.is_number_integer()) {
                return fail(error, context, path, "must be an int64 integer");
            }
            result = value.get_ref<const Json::number_integer_t&>();
            return true;
        }

        bool decodeUint64At(const Json& value, std::uint64_t& result, std::string& error, std::string_view context, std::string_view path) {
            if (value.is_number_unsigned()) {
                result = value.get_ref<const Json::number_unsigned_t&>();
                return true;
            }
            if (!value.is_number_integer()) {
                return fail(error, context, path, "must be a non-negative integer");
            }
            const std::int64_t number = value.get_ref<const Json::number_integer_t&>();
            if (number < 0) {
                return fail(error, context, path, "must be a non-negative integer");
            }
            result = static_cast<std::uint64_t>(number);
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
        bool decodeOptional(const Json& object,
                            std::string_view name,
                            std::optional<T>& result,
                            Decode&& decode,
                            std::string& error,
                            std::string_view context,
                            std::string_view path = "$") {
            result.reset();
            const Json* value = member(object, name);
            if (value == nullptr) {
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
            result.emplace(std::move(decoded));
            return true;
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

        bool decodeStringArrayAt(
            const Json& value, std::vector<std::string>& result, std::string& error, std::string_view context, std::string_view path) {
            return decodeArrayAt(
                value,
                result,
                [&](const Json& item, std::string& decoded, const std::string& itemPath) {
                    return decodeStringAt(item, decoded, error, context, itemPath);
                },
                error,
                context,
                path);
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

        bool decodeHookErrorInfo(const Json& value, typed::HookErrorInfo& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "HookErrorInfo";
            if (!requireObject(value, Context, error, path) ||
                !decodeRequired(
                    value,
                    "message",
                    result.message,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "path",
                    result.path,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }
            result.raw = value;
            return true;
        }

        bool decodeHookMetadata(const Json& value, typed::HookMetadata& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "HookMetadata";
            const auto stringDecoder = [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                return decodeStringAt(item, decoded, error, Context, itemPath);
            };
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(value, "command", result.command, stringDecoder, error, Context, path) ||
                !decodeRequired(value, "currentHash", result.currentHash, stringDecoder, error, Context, path) ||
                !decodeRequired(
                    value,
                    "displayOrder",
                    result.displayOrder,
                    [&](const Json& item, std::int64_t& decoded, std::string_view itemPath) {
                        return decodeInt64At(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
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
                !decodeRequired(
                    value,
                    "eventName",
                    result.eventName,
                    [&](const Json& item, typed::HookEventName& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookEventName", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "handlerType",
                    result.handlerType,
                    [&](const Json& item, typed::HookHandlerType& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookHandlerType", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "isManaged",
                    result.isManaged,
                    [&](const Json& item, bool& decoded, std::string_view itemPath) {
                        return decodeBoolAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(value, "key", result.key, stringDecoder, error, Context, path) ||
                !decodeOptionalNullable(value, "matcher", result.matcher, stringDecoder, error, Context, path) ||
                !decodeOptionalNullable(value, "pluginId", result.pluginId, stringDecoder, error, Context, path) ||
                !decodeRequired(
                    value,
                    "source",
                    result.source,
                    [&](const Json& item, typed::HookSource& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookSource", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "sourcePath",
                    result.sourcePath,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(value, "statusMessage", result.statusMessage, stringDecoder, error, Context, path) ||
                !decodeRequired(
                    value,
                    "timeoutSec",
                    result.timeoutSec,
                    [&](const Json& item, std::uint64_t& decoded, std::string_view itemPath) {
                        return decodeUint64At(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "trustStatus",
                    result.trustStatus,
                    [&](const Json& item, typed::HookTrustStatus& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookTrustStatus", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }
            result.raw = value;
            return true;
        }

        bool decodeHooksListEntry(const Json& value, typed::HooksListEntry& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "HooksListEntry";
            const Json* errors = member(value, "errors");
            const Json* hooks = member(value, "hooks");
            const Json* warnings = member(value, "warnings");
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
            if (errors == nullptr) {
                return fail(error, Context, fieldPath(path, "errors"), "is required");
            }
            if (!decodeArrayAt(
                    *errors,
                    result.errors,
                    [&](const Json& item, typed::HookErrorInfo& decoded, const std::string& itemPath) {
                        if (!decodeHookErrorInfo(item, decoded, error, itemPath)) {
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
            if (hooks == nullptr) {
                return fail(error, Context, fieldPath(path, "hooks"), "is required");
            }
            if (!decodeArrayAt(
                    *hooks,
                    result.hooks,
                    [&](const Json& item, typed::HookMetadata& decoded, const std::string& itemPath) {
                        if (!decodeHookMetadata(item, decoded, error, itemPath)) {
                            return false;
                        }
                        appendDiagnostics(result.diagnostics, decoded.diagnostics);
                        return true;
                    },
                    error,
                    Context,
                    fieldPath(path, "hooks"))) {
                return false;
            }
            if (warnings == nullptr) {
                return fail(error, Context, fieldPath(path, "warnings"), "is required");
            }
            if (!decodeStringArrayAt(*warnings, result.warnings, error, Context, fieldPath(path, "warnings"))) {
                return false;
            }
            result.raw = value;
            return true;
        }

        bool decodeHookOutputEntry(const Json& value, typed::HookOutputEntry& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "HookOutputEntry";
            if (!requireObject(value, Context, error, path) ||
                !decodeRequired(
                    value,
                    "kind",
                    result.kind,
                    [&](const Json& item, typed::HookOutputEntryKind& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookOutputEntryKind", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "text",
                    result.text,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }
            result.raw = value;
            return true;
        }

        bool decodeHookRunSummary(const Json& value, typed::HookRunSummary& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "HookRunSummary";
            const auto int64Decoder = [&](const Json& item, std::int64_t& decoded, std::string_view itemPath) {
                return decodeInt64At(item, decoded, error, Context, itemPath);
            };
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(value, "completedAt", result.completedAt, int64Decoder, error, Context, path) ||
                !decodeRequired(value, "displayOrder", result.displayOrder, int64Decoder, error, Context, path) ||
                !decodeOptionalNullable(value, "durationMs", result.durationMs, int64Decoder, error, Context, path)) {
                return false;
            }

            const Json* entries = member(value, "entries");
            if (entries == nullptr) {
                return fail(error, Context, fieldPath(path, "entries"), "is required");
            }
            if (!decodeArrayAt(
                    *entries,
                    result.entries,
                    [&](const Json& item, typed::HookOutputEntry& decoded, const std::string& itemPath) {
                        if (!decodeHookOutputEntry(item, decoded, error, itemPath)) {
                            return false;
                        }
                        appendDiagnostics(result.diagnostics, decoded.diagnostics);
                        return true;
                    },
                    error,
                    Context,
                    fieldPath(path, "entries")) ||
                !decodeRequired(
                    value,
                    "eventName",
                    result.eventName,
                    [&](const Json& item, typed::HookEventName& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookEventName", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "executionMode",
                    result.executionMode,
                    [&](const Json& item, typed::HookExecutionMode& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookExecutionMode", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "handlerType",
                    result.handlerType,
                    [&](const Json& item, typed::HookHandlerType& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookHandlerType", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "id",
                    result.id,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "scope",
                    result.scope,
                    [&](const Json& item, typed::HookScope& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookScope", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }

            if (!decodeOptional(
                    value,
                    "source",
                    result.source,
                    [&](const Json& item, typed::HookSource& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookSource", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "sourcePath",
                    result.sourcePath,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(value, "startedAt", result.startedAt, int64Decoder, error, Context, path) ||
                !decodeRequired(
                    value,
                    "status",
                    result.status,
                    [&](const Json& item, typed::HookRunStatus& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "HookRunStatus", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "statusMessage",
                    result.statusMessage,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }

            result.raw = value;
            return true;
        }

        template <typename NotificationType>
        std::optional<NotificationType>
        decodeHookNotification(const Notification& notification, std::string& error, std::string_view context) {
            NotificationType result;
            if (!requireObject(notification.params, context, error, "$.params") ||
                !decodeRequired(
                    notification.params,
                    "run",
                    result.run,
                    [&](const Json& item, typed::HookRunSummary& decoded, std::string_view itemPath) {
                        return decodeHookRunSummary(item, decoded, error, itemPath);
                    },
                    error,
                    context,
                    "$.params") ||
                !decodeRequired(
                    notification.params,
                    "threadId",
                    result.threadId,
                    [&](const Json& item, typed::ThreadId& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, context, itemPath);
                    },
                    error,
                    context,
                    "$.params") ||
                !decodeOptionalNullable(
                    notification.params,
                    "turnId",
                    result.turnId,
                    [&](const Json& item, typed::TurnId& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, context, itemPath);
                    },
                    error,
                    context,
                    "$.params")) {
                return std::nullopt;
            }
            appendDiagnostics(result.diagnostics, result.run.diagnostics);
            result.raw = notification.raw;
            return result;
        }

        void setUnexpectedFailure(std::string& error, std::string_view context) noexcept {
            try {
                error = std::string(context) + " failed while processing JSON";
            } catch (...) {
            }
        }

    } // namespace

    std::optional<Json> encodeHooksListParams(const typed::HooksListParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(value.raw, error, "HooksListParams", {"cwds"});
            if (!error.empty()) {
                return std::nullopt;
            }
            if (value.cwds) {
                result["cwds"] = *value.cwds;
            }
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "HooksListParams");
            return std::nullopt;
        }
    }

    std::optional<typed::HooksListResponse> decodeHooksListResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            typed::HooksListResponse result;
            if (!requireObject(value, "HooksListResponse", error)) {
                return std::nullopt;
            }
            const Json* data = member(value, "data");
            if (data == nullptr) {
                fail(error, "HooksListResponse", "$.data", "is required");
                return std::nullopt;
            }
            if (!decodeArrayAt(
                    *data,
                    result.data,
                    [&](const Json& item, typed::HooksListEntry& decoded, const std::string& itemPath) {
                        if (!decodeHooksListEntry(item, decoded, error, itemPath)) {
                            return false;
                        }
                        appendDiagnostics(result.diagnostics, decoded.diagnostics);
                        return true;
                    },
                    error,
                    "HooksListResponse",
                    "$.data")) {
                return std::nullopt;
            }
            result.raw = value;
            error.clear();
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "HooksListResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::HookCompletedNotification> decodeHookCompletedNotification(const Notification& notification,
                                                                                    std::string& error) noexcept {
        try {
            error.clear();
            return decodeHookNotification<typed::HookCompletedNotification>(notification, error, "HookCompletedNotification");
        } catch (...) {
            setUnexpectedFailure(error, "HookCompletedNotification");
            return std::nullopt;
        }
    }

    std::optional<typed::HookStartedNotification> decodeHookStartedNotification(const Notification& notification,
                                                                                std::string& error) noexcept {
        try {
            error.clear();
            return decodeHookNotification<typed::HookStartedNotification>(notification, error, "HookStartedNotification");
        } catch (...) {
            setUnexpectedFailure(error, "HookStartedNotification");
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::detail
