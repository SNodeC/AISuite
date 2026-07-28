/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/PluginCodec.h"

#include "ai/openai/codex/detail/DecodeDiagnostic.h"

#include <cstddef>
#include <initializer_list>
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

        bool prepareObject(const Json& raw,
                           Json& result,
                           std::initializer_list<std::string_view> fields,
                           std::string& error,
                           std::string_view context,
                           std::string_view path = "$.raw") {
            if (!raw.is_object()) {
                return fail(error, context, path, "must be an object");
            }
            result = raw;
            for (const std::string_view field : fields) {
                result.erase(std::string(field));
            }
            return true;
        }

        bool decodeStringAt(const Json& value, std::string& result, std::string& error, std::string_view context, std::string_view path) {
            if (!value.is_string()) {
                return fail(error, context, path, "must be a string");
            }
            result = value.get_ref<const std::string&>();
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

        template <typename T, typename Encode>
        void encodeOptionalNullable(Json& object, std::string_view name, const typed::OptionalNullable<T>& value, Encode&& encode) {
            if (!value.present) {
                return;
            }
            object[std::string(name)] = value.value ? Json(encode(*value.value)) : Json(nullptr);
        }

        bool encodePluginShareTargetAt(
            const typed::PluginShareTarget& value, Json& result, std::string& error, std::string_view context, std::string_view path) {
            if (!prepareObject(value.raw, result, {"principalId", "principalType", "role"}, error, context, path)) {
                return false;
            }
            result["principalId"] = value.principalId;
            result["principalType"] = value.principalType.value;
            result["role"] = value.role.value;
            return true;
        }

        bool encodePluginShareTargets(const std::vector<typed::PluginShareTarget>& value,
                                      Json& result,
                                      std::string& error,
                                      std::string_view context,
                                      std::string_view path) {
            result = Json::array();
            for (std::size_t index = 0; index < value.size(); ++index) {
                Json target;
                if (!encodePluginShareTargetAt(value[index], target, error, context, indexedPath(path, index) + ".raw")) {
                    return false;
                }
                result.push_back(std::move(target));
            }
            return true;
        }

        bool decodeAppSummaryAt(
            const Json& value, typed::AppSummary& result, std::string& error, std::string_view context, std::string_view path) {
            if (!requireObject(value, context, error, path)) {
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            return decodeOptionalNullable(
                       value,
                       "category",
                       result.category,
                       [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                           return decodeStringAt(item, decoded, error, context, itemPath);
                       },
                       error,
                       context,
                       path) &&
                   decodeOptionalNullable(
                       value,
                       "description",
                       result.description,
                       [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                           return decodeStringAt(item, decoded, error, context, itemPath);
                       },
                       error,
                       context,
                       path) &&
                   decodeRequired(
                       value,
                       "id",
                       result.id,
                       [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                           return decodeStringAt(item, decoded, error, context, itemPath);
                       },
                       error,
                       context,
                       path) &&
                   decodeOptionalNullable(
                       value,
                       "installUrl",
                       result.installUrl,
                       [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                           return decodeStringAt(item, decoded, error, context, itemPath);
                       },
                       error,
                       context,
                       path) &&
                   decodeRequired(
                       value,
                       "name",
                       result.name,
                       [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                           return decodeStringAt(item, decoded, error, context, itemPath);
                       },
                       error,
                       context,
                       path);
        }

        bool
        decodePluginSharePrincipalAt(const Json& value, typed::PluginSharePrincipal& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "PluginSharePrincipal";
            if (!requireObject(value, Context, error, path)) {
                return false;
            }
            result.raw = value;
            result.diagnostics.clear();
            return decodeRequired(
                       value,
                       "name",
                       result.name,
                       [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                           return decodeStringAt(item, decoded, error, Context, itemPath);
                       },
                       error,
                       Context,
                       path) &&
                   decodeRequired(
                       value,
                       "principalId",
                       result.principalId,
                       [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                           return decodeStringAt(item, decoded, error, Context, itemPath);
                       },
                       error,
                       Context,
                       path) &&
                   decodeRequired(
                       value,
                       "principalType",
                       result.principalType,
                       [&](const Json& item, typed::PluginSharePrincipalType& decoded, std::string_view itemPath) {
                           return decodeOpenEnumAt(item, decoded, result.diagnostics, "PluginSharePrincipalType", itemPath, error, Context);
                       },
                       error,
                       Context,
                       path) &&
                   decodeRequired(
                       value,
                       "role",
                       result.role,
                       [&](const Json& item, typed::PluginSharePrincipalRole& decoded, std::string_view itemPath) {
                           return decodeOpenEnumAt(item, decoded, result.diagnostics, "PluginSharePrincipalRole", itemPath, error, Context);
                       },
                       error,
                       Context,
                       path);
        }

        void appendDiagnostics(std::vector<typed::DecodeDiagnostic>& target, const std::vector<typed::DecodeDiagnostic>& source) {
            target.insert(target.end(), source.begin(), source.end());
        }

        void setUnexpectedFailure(std::string& error, std::string_view context) noexcept {
            try {
                error = std::string(context) + " failed while processing JSON";
            } catch (...) {
            }
        }

    } // namespace

    std::optional<Json> encodePluginInstallParams(const typed::PluginInstallParams& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginInstallParams";
            Json result;
            if (!prepareObject(value.raw, result, {"marketplacePath", "pluginName", "remoteMarketplaceName"}, error, Context) ||
                !validateOptionalNullable(value.marketplacePath, error, Context, "$.marketplacePath") ||
                !validateOptionalNullable(value.remoteMarketplaceName, error, Context, "$.remoteMarketplaceName")) {
                return std::nullopt;
            }
            encodeOptionalNullable(result, "marketplacePath", value.marketplacePath, [](const typed::AbsolutePathBuf& item) {
                return Json(item.value);
            });
            result["pluginName"] = value.pluginName;
            encodeOptionalNullable(result, "remoteMarketplaceName", value.remoteMarketplaceName, [](const std::string& item) {
                return Json(item);
            });
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginInstallParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodePluginShareCheckoutParams(const typed::PluginShareCheckoutParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result;
            if (!prepareObject(value.raw, result, {"remotePluginId"}, error, "PluginShareCheckoutParams")) {
                return std::nullopt;
            }
            result["remotePluginId"] = value.remotePluginId;
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginShareCheckoutParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodePluginShareDeleteParams(const typed::PluginShareDeleteParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result;
            if (!prepareObject(value.raw, result, {"remotePluginId"}, error, "PluginShareDeleteParams")) {
                return std::nullopt;
            }
            result["remotePluginId"] = value.remotePluginId;
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginShareDeleteParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodePluginShareSaveParams(const typed::PluginShareSaveParams& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginShareSaveParams";
            Json result;
            if (!prepareObject(value.raw, result, {"discoverability", "pluginPath", "remotePluginId", "shareTargets"}, error, Context) ||
                !validateOptionalNullable(value.discoverability, error, Context, "$.discoverability") ||
                !validateOptionalNullable(value.remotePluginId, error, Context, "$.remotePluginId") ||
                !validateOptionalNullable(value.shareTargets, error, Context, "$.shareTargets")) {
                return std::nullopt;
            }
            encodeOptionalNullable(result, "discoverability", value.discoverability, [](const typed::PluginShareDiscoverability& item) {
                return Json(item.value);
            });
            result["pluginPath"] = value.pluginPath.value;
            encodeOptionalNullable(result, "remotePluginId", value.remotePluginId, [](const std::string& item) {
                return Json(item);
            });
            if (value.shareTargets.isNull()) {
                result["shareTargets"] = nullptr;
            } else if (value.shareTargets.hasValue()) {
                Json targets;
                if (!encodePluginShareTargets(*value.shareTargets, targets, error, Context, "$.shareTargets")) {
                    return std::nullopt;
                }
                result["shareTargets"] = std::move(targets);
            }
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginShareSaveParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodePluginShareUpdateTargetsParams(const typed::PluginShareUpdateTargetsParams& value,
                                                             std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginShareUpdateTargetsParams";
            Json result;
            if (!prepareObject(value.raw, result, {"discoverability", "remotePluginId", "shareTargets"}, error, Context)) {
                return std::nullopt;
            }
            Json targets;
            if (!encodePluginShareTargets(value.shareTargets, targets, error, Context, "$.shareTargets")) {
                return std::nullopt;
            }
            result["discoverability"] = value.discoverability.value;
            result["remotePluginId"] = value.remotePluginId;
            result["shareTargets"] = std::move(targets);
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginShareUpdateTargetsParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodePluginSkillReadParams(const typed::PluginSkillReadParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result;
            if (!prepareObject(
                    value.raw, result, {"remoteMarketplaceName", "remotePluginId", "skillName"}, error, "PluginSkillReadParams")) {
                return std::nullopt;
            }
            result["remoteMarketplaceName"] = value.remoteMarketplaceName;
            result["remotePluginId"] = value.remotePluginId;
            result["skillName"] = value.skillName;
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginSkillReadParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodePluginUninstallParams(const typed::PluginUninstallParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result;
            if (!prepareObject(value.raw, result, {"pluginId"}, error, "PluginUninstallParams")) {
                return std::nullopt;
            }
            result["pluginId"] = value.pluginId;
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginUninstallParams");
            return std::nullopt;
        }
    }

    std::optional<typed::PluginInstallResponse> decodePluginInstallResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginInstallResponse";
            typed::PluginInstallResponse result;
            if (!requireObject(value, Context, error) ||
                !decodeRequired(
                    value,
                    "appsNeedingAuth",
                    result.appsNeedingAuth,
                    [&](const Json& item, std::vector<typed::AppSummary>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::AppSummary& app, const std::string& childPath) {
                                return decodeAppSummaryAt(child, app, error, Context, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "authPolicy",
                    result.authPolicy,
                    [&](const Json& item, typed::PluginAuthPolicy& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "PluginAuthPolicy", itemPath, error, Context);
                    },
                    error,
                    Context)) {
                return std::nullopt;
            }
            result.raw = value;
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "PluginInstallResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::PluginShareCheckoutResponse> decodePluginShareCheckoutResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginShareCheckoutResponse";
            typed::PluginShareCheckoutResponse result;
            if (!requireObject(value, Context, error) ||
                !decodeRequired(
                    value,
                    "marketplaceName",
                    result.marketplaceName,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "marketplacePath",
                    result.marketplacePath,
                    [&](const Json& item, typed::AbsolutePathBuf& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "pluginId",
                    result.pluginId,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "pluginName",
                    result.pluginName,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "pluginPath",
                    result.pluginPath,
                    [&](const Json& item, typed::AbsolutePathBuf& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "remotePluginId",
                    result.remotePluginId,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context) ||
                !decodeOptionalNullable(
                    value,
                    "remoteVersion",
                    result.remoteVersion,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context)) {
                return std::nullopt;
            }
            result.raw = value;
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "PluginShareCheckoutResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::PluginShareSaveResponse> decodePluginShareSaveResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginShareSaveResponse";
            typed::PluginShareSaveResponse result;
            if (!requireObject(value, Context, error) ||
                !decodeRequired(
                    value,
                    "remotePluginId",
                    result.remotePluginId,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "shareUrl",
                    result.shareUrl,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context)) {
                return std::nullopt;
            }
            result.raw = value;
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "PluginShareSaveResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::PluginShareUpdateTargetsResponse> decodePluginShareUpdateTargetsResponse(const Json& value,
                                                                                                  std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginShareUpdateTargetsResponse";
            typed::PluginShareUpdateTargetsResponse result;
            if (!requireObject(value, Context, error) ||
                !decodeRequired(
                    value,
                    "discoverability",
                    result.discoverability,
                    [&](const Json& item, typed::PluginShareDiscoverability& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "PluginShareDiscoverability", itemPath, error, Context);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "principals",
                    result.principals,
                    [&](const Json& item, std::vector<typed::PluginSharePrincipal>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::PluginSharePrincipal& principal, const std::string& childPath) {
                                return decodePluginSharePrincipalAt(child, principal, error, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context)) {
                return std::nullopt;
            }
            for (const auto& principal : result.principals) {
                appendDiagnostics(result.diagnostics, principal.diagnostics);
            }
            result.raw = value;
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "PluginShareUpdateTargetsResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::PluginSkillReadResponse> decodePluginSkillReadResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginSkillReadResponse";
            typed::PluginSkillReadResponse result;
            if (!requireObject(value, Context, error) || !decodeOptionalNullable(
                                                             value,
                                                             "contents",
                                                             result.contents,
                                                             [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                                                                 return decodeStringAt(item, decoded, error, Context, itemPath);
                                                             },
                                                             error,
                                                             Context)) {
                return std::nullopt;
            }
            result.raw = value;
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "PluginSkillReadResponse");
            return std::nullopt;
        }
    }


} // namespace ai::openai::codex::detail
