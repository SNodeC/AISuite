/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/PluginCodec.h"

#include "ai/openai/codex/detail/DecodeDiagnostic.h"
#include "ai/openai/codex/typed/Apps.h"
#include "ai/openai/codex/typed/Hooks.h"
#include "ai/openai/codex/typed/Skills.h"
#include "ai/openai/codex/typed/Types.h"

#include <cstddef>
#include <initializer_list>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
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

        bool decodeBooleanAt(const Json& value, bool& result, std::string& error, std::string_view context, std::string_view path) {
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
            result = std::move(decoded);
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

        bool decodeStringVectorAt(
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

        bool decodePathVectorAt(const Json& value,
                                std::vector<typed::AbsolutePath>& result,
                                std::string& error,
                                std::string_view context,
                                std::string_view path) {
            return decodeArrayAt(
                value,
                result,
                [&](const Json& item, typed::AbsolutePath& decoded, const std::string& itemPath) {
                    return decodeStrongStringAt(item, decoded, error, context, itemPath);
                },
                error,
                context,
                path);
        }

        void preserveMalformedPluginSource(const Json& value,
                                           std::optional<std::string> discriminator,
                                           std::string path,
                                           typed::PluginSource& result) {
            typed::DecodeDiagnostic diagnostic = malformedKnownDiagnostic("PluginSource", std::move(path));
            result = typed::UnknownPluginSource{std::move(discriminator), value, diagnostic};
        }

        void preserveUnknownPluginSource(const Json& value, std::string discriminator, std::string path, typed::PluginSource& result) {
            typed::DecodeDiagnostic diagnostic = unknownDiscriminatorDiagnostic("PluginSource", std::move(path));
            result = typed::UnknownPluginSource{std::move(discriminator), value, diagnostic};
        }

        bool decodePluginSourceAt(const Json& value, typed::PluginSource& result, std::string_view path) {
            if (!value.is_object()) {
                preserveMalformedPluginSource(value, std::nullopt, std::string(path), result);
                return true;
            }
            const Json* typeValue = member(value, "type");
            if (typeValue == nullptr || !typeValue->is_string()) {
                preserveMalformedPluginSource(value, std::nullopt, fieldPath(path, "type"), result);
                return true;
            }
            const std::string type = typeValue->get_ref<const std::string&>();
            std::string decodeError;
            if (type == "git") {
                typed::GitPluginSource decoded;
                if (!decodeRequired(
                        value,
                        "url",
                        decoded.url,
                        [&](const Json& item, std::string& output, std::string_view itemPath) {
                            return decodeStringAt(item, output, decodeError, "PluginSource", itemPath);
                        },
                        decodeError,
                        "PluginSource",
                        path)) {
                    preserveMalformedPluginSource(value, type, fieldPath(path, "url"), result);
                    return true;
                }
                if (!decodeOptionalNullable(
                        value,
                        "path",
                        decoded.path,
                        [&](const Json& item, std::string& output, std::string_view itemPath) {
                            return decodeStringAt(item, output, decodeError, "PluginSource", itemPath);
                        },
                        decodeError,
                        "PluginSource",
                        path)) {
                    preserveMalformedPluginSource(value, type, fieldPath(path, "path"), result);
                    return true;
                }
                if (!decodeOptionalNullable(
                        value,
                        "refName",
                        decoded.refName,
                        [&](const Json& item, std::string& output, std::string_view itemPath) {
                            return decodeStringAt(item, output, decodeError, "PluginSource", itemPath);
                        },
                        decodeError,
                        "PluginSource",
                        path)) {
                    preserveMalformedPluginSource(value, type, fieldPath(path, "refName"), result);
                    return true;
                }
                if (!decodeOptionalNullable(
                        value,
                        "sha",
                        decoded.sha,
                        [&](const Json& item, std::string& output, std::string_view itemPath) {
                            return decodeStringAt(item, output, decodeError, "PluginSource", itemPath);
                        },
                        decodeError,
                        "PluginSource",
                        path)) {
                    preserveMalformedPluginSource(value, type, fieldPath(path, "sha"), result);
                    return true;
                }
                decoded.raw = value;
                result = std::move(decoded);
                return true;
            }
            if (type == "local") {
                typed::LocalPluginSource decoded;
                if (!decodeRequired(
                        value,
                        "path",
                        decoded.path,
                        [&](const Json& item, typed::AbsolutePath& output, std::string_view itemPath) {
                            return decodeStrongStringAt(item, output, decodeError, "PluginSource", itemPath);
                        },
                        decodeError,
                        "PluginSource",
                        path)) {
                    preserveMalformedPluginSource(value, type, fieldPath(path, "path"), result);
                    return true;
                }
                decoded.raw = value;
                result = std::move(decoded);
                return true;
            }
            if (type == "npm") {
                typed::NpmPluginSource decoded;
                if (!decodeRequired(
                        value,
                        "package",
                        decoded.package,
                        [&](const Json& item, std::string& output, std::string_view itemPath) {
                            return decodeStringAt(item, output, decodeError, "PluginSource", itemPath);
                        },
                        decodeError,
                        "PluginSource",
                        path)) {
                    preserveMalformedPluginSource(value, type, fieldPath(path, "package"), result);
                    return true;
                }
                if (!decodeOptionalNullable(
                        value,
                        "registry",
                        decoded.registry,
                        [&](const Json& item, std::string& output, std::string_view itemPath) {
                            return decodeStringAt(item, output, decodeError, "PluginSource", itemPath);
                        },
                        decodeError,
                        "PluginSource",
                        path)) {
                    preserveMalformedPluginSource(value, type, fieldPath(path, "registry"), result);
                    return true;
                }
                if (!decodeOptionalNullable(
                        value,
                        "version",
                        decoded.version,
                        [&](const Json& item, std::string& output, std::string_view itemPath) {
                            return decodeStringAt(item, output, decodeError, "PluginSource", itemPath);
                        },
                        decodeError,
                        "PluginSource",
                        path)) {
                    preserveMalformedPluginSource(value, type, fieldPath(path, "version"), result);
                    return true;
                }
                decoded.raw = value;
                result = std::move(decoded);
                return true;
            }
            if (type == "remote") {
                typed::RemotePluginSource decoded;
                decoded.raw = value;
                result = std::move(decoded);
                return true;
            }
            preserveUnknownPluginSource(value, type, fieldPath(path, "type"), result);
            return true;
        }

        void appendPluginSourceDiagnostics(std::vector<typed::DecodeDiagnostic>& target, const typed::PluginSource& source) {
            std::visit(
                [&](const auto& alternative) {
                    using Alternative = std::decay_t<decltype(alternative)>;
                    if constexpr (std::is_same_v<Alternative, typed::UnknownPluginSource>) {
                        if (alternative.diagnostic) {
                            target.emplace_back(*alternative.diagnostic);
                        }
                    } else {
                        appendDiagnostics(target, alternative.diagnostics);
                    }
                },
                source);
        }

        bool
        decodeMarketplaceInterfaceAt(const Json& value, typed::MarketplaceInterface& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "MarketplaceInterface";
            if (!requireObject(value, Context, error, path) || !decodeOptionalNullable(
                                                                   value,
                                                                   "displayName",
                                                                   result.displayName,
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

        bool decodeMarketplaceLoadErrorInfoAt(const Json& value,
                                              typed::MarketplaceLoadErrorInfo& result,
                                              std::string& error,
                                              std::string_view path) {
            constexpr std::string_view Context = "MarketplaceLoadErrorInfo";
            if (!requireObject(value, Context, error, path) ||
                !decodeRequired(
                    value,
                    "marketplacePath",
                    result.marketplacePath,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "message",
                    result.message,
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

        bool decodePluginInterfaceAt(const Json& value, typed::PluginInterface& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "PluginInterface";
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(
                    value,
                    "brandColor",
                    result.brandColor,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "capabilities",
                    result.capabilities,
                    [&](const Json& item, std::vector<std::string>& decoded, std::string_view itemPath) {
                        return decodeStringVectorAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "category",
                    result.category,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "composerIcon",
                    result.composerIcon,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "composerIconUrl",
                    result.composerIconUrl,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "defaultPrompt",
                    result.defaultPrompt,
                    [&](const Json& item, std::vector<std::string>& decoded, std::string_view itemPath) {
                        return decodeStringVectorAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "developerName",
                    result.developerName,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "displayName",
                    result.displayName,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "logo",
                    result.logo,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "logoDark",
                    result.logoDark,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "logoUrl",
                    result.logoUrl,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "logoUrlDark",
                    result.logoUrlDark,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "longDescription",
                    result.longDescription,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "privacyPolicyUrl",
                    result.privacyPolicyUrl,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "screenshotUrls",
                    result.screenshotUrls,
                    [&](const Json& item, std::vector<std::string>& decoded, std::string_view itemPath) {
                        return decodeStringVectorAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "screenshots",
                    result.screenshots,
                    [&](const Json& item, std::vector<typed::AbsolutePath>& decoded, std::string_view itemPath) {
                        return decodePathVectorAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "shortDescription",
                    result.shortDescription,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "termsOfServiceUrl",
                    result.termsOfServiceUrl,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "websiteUrl",
                    result.websiteUrl,
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

        bool decodePluginShareContextAt(const Json& value, typed::PluginShareContext& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "PluginShareContext";
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(
                    value,
                    "creatorAccountUserId",
                    result.creatorAccountUserId,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "creatorName",
                    result.creatorName,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "discoverability",
                    result.discoverability,
                    [&](const Json& item, typed::PluginShareDiscoverability& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "PluginShareDiscoverability", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "remotePluginId",
                    result.remotePluginId,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "remoteVersion",
                    result.remoteVersion,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "sharePrincipals",
                    result.sharePrincipals,
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
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "shareUrl",
                    result.shareUrl,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }
            if (result.sharePrincipals.hasValue()) {
                for (const auto& principal : *result.sharePrincipals) {
                    appendDiagnostics(result.diagnostics, principal.diagnostics);
                }
            }
            result.raw = value;
            return true;
        }

        bool decodePluginSummaryAt(const Json& value, typed::PluginSummary& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "PluginSummary";
            if (!requireObject(value, Context, error, path) ||
                !decodeRequired(
                    value,
                    "authPolicy",
                    result.authPolicy,
                    [&](const Json& item, typed::PluginAuthPolicy& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "PluginAuthPolicy", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptional(
                    value,
                    "availability",
                    result.availability,
                    [&](const Json& item, typed::PluginAvailability& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "PluginAvailability", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "enabled",
                    result.enabled,
                    [&](const Json& item, bool& decoded, std::string_view itemPath) {
                        return decodeBooleanAt(item, decoded, error, Context, itemPath);
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
                    "installPolicy",
                    result.installPolicy,
                    [&](const Json& item, typed::PluginInstallPolicy& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "PluginInstallPolicy", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "installPolicySource",
                    result.installPolicySource,
                    [&](const Json& item, typed::PluginInstallPolicySource& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(item, decoded, result.diagnostics, "PluginInstallPolicySource", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "installed",
                    result.installed,
                    [&](const Json& item, bool& decoded, std::string_view itemPath) {
                        return decodeBooleanAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "interface",
                    result.interface,
                    [&](const Json& item, typed::PluginInterface& decoded, std::string_view itemPath) {
                        return decodePluginInterfaceAt(item, decoded, error, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptional(
                    value,
                    "keywords",
                    result.keywords,
                    [&](const Json& item, std::vector<std::string>& decoded, std::string_view itemPath) {
                        return decodeStringVectorAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "localVersion",
                    result.localVersion,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "name",
                    result.name,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "remotePluginId",
                    result.remotePluginId,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "shareContext",
                    result.shareContext,
                    [&](const Json& item, typed::PluginShareContext& decoded, std::string_view itemPath) {
                        return decodePluginShareContextAt(item, decoded, error, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "source",
                    result.source,
                    [&](const Json& item, typed::PluginSource& decoded, std::string_view itemPath) {
                        return decodePluginSourceAt(item, decoded, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "version",
                    result.version,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }
            if (result.interface.hasValue()) {
                appendDiagnostics(result.diagnostics, result.interface->diagnostics);
            }
            if (result.shareContext.hasValue()) {
                appendDiagnostics(result.diagnostics, result.shareContext->diagnostics);
            }
            appendPluginSourceDiagnostics(result.diagnostics, result.source);
            result.raw = value;
            return true;
        }

        bool decodePluginMarketplaceEntryAt(const Json& value,
                                            typed::PluginMarketplaceEntry& result,
                                            std::string& error,
                                            std::string_view path) {
            constexpr std::string_view Context = "PluginMarketplaceEntry";
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(
                    value,
                    "interface",
                    result.interface,
                    [&](const Json& item, typed::MarketplaceInterface& decoded, std::string_view itemPath) {
                        return decodeMarketplaceInterfaceAt(item, decoded, error, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "name",
                    result.name,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "path",
                    result.path,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "plugins",
                    result.plugins,
                    [&](const Json& item, std::vector<typed::PluginSummary>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::PluginSummary& plugin, const std::string& childPath) {
                                return decodePluginSummaryAt(child, plugin, error, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }
            if (result.interface.hasValue()) {
                appendDiagnostics(result.diagnostics, result.interface->diagnostics);
            }
            for (const auto& plugin : result.plugins) {
                appendDiagnostics(result.diagnostics, plugin.diagnostics);
            }
            result.raw = value;
            return true;
        }

        bool decodeAppTemplateSummaryAt(const Json& value, typed::AppTemplateSummary& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "AppTemplateSummary";
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(
                    value,
                    "canonicalConnectorId",
                    result.canonicalConnectorId,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "category",
                    result.category,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "description",
                    result.description,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "logoUrl",
                    result.logoUrl,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "logoUrlDark",
                    result.logoUrlDark,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "materializedAppIds",
                    result.materializedAppIds,
                    [&](const Json& item, std::vector<std::string>& decoded, std::string_view itemPath) {
                        return decodeStringVectorAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "name",
                    result.name,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "reason",
                    result.reason,
                    [&](const Json& item, typed::AppTemplateUnavailableReason& decoded, std::string_view itemPath) {
                        return decodeOpenEnumAt(
                            item, decoded, result.diagnostics, "AppTemplateUnavailableReason", itemPath, error, Context);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "templateId",
                    result.templateId,
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

        bool decodeSkillInterfaceAt(const Json& value, typed::SkillInterface& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "SkillInterface";
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(
                    value,
                    "brandColor",
                    result.brandColor,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "defaultPrompt",
                    result.defaultPrompt,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "displayName",
                    result.displayName,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "iconLarge",
                    result.iconLarge,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "iconSmall",
                    result.iconSmall,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "shortDescription",
                    result.shortDescription,
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

        bool decodePluginHookSummaryAt(const Json& value, typed::PluginHookSummary& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "PluginHookSummary";
            if (!requireObject(value, Context, error, path) ||
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
                    "key",
                    result.key,
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

        bool decodeSkillSummaryAt(const Json& value, typed::SkillSummary& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "SkillSummary";
            if (!requireObject(value, Context, error, path) ||
                !decodeRequired(
                    value,
                    "description",
                    result.description,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "enabled",
                    result.enabled,
                    [&](const Json& item, bool& decoded, std::string_view itemPath) {
                        return decodeBooleanAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "interface",
                    result.interface,
                    [&](const Json& item, typed::SkillInterface& decoded, std::string_view itemPath) {
                        return decodeSkillInterfaceAt(item, decoded, error, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "name",
                    result.name,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "path",
                    result.path,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "shortDescription",
                    result.shortDescription,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }
            if (result.interface.hasValue()) {
                appendDiagnostics(result.diagnostics, result.interface->diagnostics);
            }
            result.raw = value;
            return true;
        }

        bool decodePluginDetailAt(const Json& value, typed::PluginDetail& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "PluginDetail";
            if (!requireObject(value, Context, error, path) ||
                !decodeRequired(
                    value,
                    "appTemplates",
                    result.appTemplates,
                    [&](const Json& item, std::vector<typed::AppTemplateSummary>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::AppTemplateSummary& summary, const std::string& childPath) {
                                return decodeAppTemplateSummaryAt(child, summary, error, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "apps",
                    result.apps,
                    [&](const Json& item, std::vector<typed::AppSummary>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::AppSummary& summary, const std::string& childPath) {
                                return decodeAppSummaryAt(child, summary, error, Context, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "description",
                    result.description,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "hooks",
                    result.hooks,
                    [&](const Json& item, std::vector<typed::PluginHookSummary>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::PluginHookSummary& summary, const std::string& childPath) {
                                return decodePluginHookSummaryAt(child, summary, error, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "marketplaceName",
                    result.marketplaceName,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "marketplacePath",
                    result.marketplacePath,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "mcpServers",
                    result.mcpServers,
                    [&](const Json& item, std::vector<std::string>& decoded, std::string_view itemPath) {
                        return decodeStringVectorAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeOptionalNullable(
                    value,
                    "shareUrl",
                    result.shareUrl,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "skills",
                    result.skills,
                    [&](const Json& item, std::vector<typed::SkillSummary>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::SkillSummary& summary, const std::string& childPath) {
                                return decodeSkillSummaryAt(child, summary, error, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "summary",
                    result.summary,
                    [&](const Json& item, typed::PluginSummary& decoded, std::string_view itemPath) {
                        return decodePluginSummaryAt(item, decoded, error, itemPath);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }
            for (const auto& summary : result.appTemplates) {
                appendDiagnostics(result.diagnostics, summary.diagnostics);
            }
            for (const auto& app : result.apps) {
                appendDiagnostics(result.diagnostics, app.diagnostics);
            }
            for (const auto& hook : result.hooks) {
                appendDiagnostics(result.diagnostics, hook.diagnostics);
            }
            for (const auto& skill : result.skills) {
                appendDiagnostics(result.diagnostics, skill.diagnostics);
            }
            appendDiagnostics(result.diagnostics, result.summary.diagnostics);
            result.raw = value;
            return true;
        }

        bool decodePluginShareListItemAt(const Json& value, typed::PluginShareListItem& result, std::string& error, std::string_view path) {
            constexpr std::string_view Context = "PluginShareListItem";
            if (!requireObject(value, Context, error, path) ||
                !decodeOptionalNullable(
                    value,
                    "localPluginPath",
                    result.localPluginPath,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    path) ||
                !decodeRequired(
                    value,
                    "plugin",
                    result.plugin,
                    [&](const Json& item, typed::PluginSummary& decoded, std::string_view itemPath) {
                        return decodePluginSummaryAt(item, decoded, error, itemPath);
                    },
                    error,
                    Context,
                    path)) {
                return false;
            }
            appendDiagnostics(result.diagnostics, result.plugin.diagnostics);
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
            encodeOptionalNullable(result, "marketplacePath", value.marketplacePath, [](const typed::AbsolutePath& item) {
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

    std::optional<Json> encodePluginInstalledParams(const typed::PluginInstalledParams& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginInstalledParams";
            Json result;
            if (!prepareObject(value.raw, result, {"cwds", "installSuggestionPluginNames"}, error, Context) ||
                !validateOptionalNullable(value.cwds, error, Context, "$.cwds") ||
                !validateOptionalNullable(value.installSuggestionPluginNames, error, Context, "$.installSuggestionPluginNames")) {
                return std::nullopt;
            }
            if (value.cwds.isNull()) {
                result["cwds"] = nullptr;
            } else if (value.cwds.hasValue()) {
                Json paths = Json::array();
                for (const auto& path : *value.cwds) {
                    paths.push_back(path.value);
                }
                result["cwds"] = std::move(paths);
            }
            encodeOptionalNullable(
                result, "installSuggestionPluginNames", value.installSuggestionPluginNames, [](const std::vector<std::string>& items) {
                    return Json(items);
                });
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginInstalledParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodePluginListParams(const typed::PluginListParams& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginListParams";
            Json result;
            if (!prepareObject(value.raw, result, {"cwds", "marketplaceKinds"}, error, Context) ||
                !validateOptionalNullable(value.cwds, error, Context, "$.cwds") ||
                !validateOptionalNullable(value.marketplaceKinds, error, Context, "$.marketplaceKinds")) {
                return std::nullopt;
            }
            if (value.cwds.isNull()) {
                result["cwds"] = nullptr;
            } else if (value.cwds.hasValue()) {
                Json paths = Json::array();
                for (const auto& path : *value.cwds) {
                    paths.push_back(path.value);
                }
                result["cwds"] = std::move(paths);
            }
            if (value.marketplaceKinds.isNull()) {
                result["marketplaceKinds"] = nullptr;
            } else if (value.marketplaceKinds.hasValue()) {
                Json kinds = Json::array();
                for (const auto& kind : *value.marketplaceKinds) {
                    kinds.push_back(kind.value);
                }
                result["marketplaceKinds"] = std::move(kinds);
            }
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginListParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodePluginReadParams(const typed::PluginReadParams& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginReadParams";
            Json result;
            if (!prepareObject(value.raw, result, {"marketplacePath", "pluginName", "remoteMarketplaceName"}, error, Context) ||
                !validateOptionalNullable(value.marketplacePath, error, Context, "$.marketplacePath") ||
                !validateOptionalNullable(value.remoteMarketplaceName, error, Context, "$.remoteMarketplaceName")) {
                return std::nullopt;
            }
            encodeOptionalNullable(result, "marketplacePath", value.marketplacePath, [](const typed::AbsolutePath& item) {
                return Json(item.value);
            });
            result["pluginName"] = value.pluginName;
            encodeOptionalNullable(result, "remoteMarketplaceName", value.remoteMarketplaceName, [](const std::string& item) {
                return Json(item);
            });
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginReadParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodePluginShareListParams(const typed::PluginShareListParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result;
            if (!prepareObject(value.raw, result, {}, error, "PluginShareListParams")) {
                return std::nullopt;
            }
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "PluginShareListParams");
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
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
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
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
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

    std::optional<typed::PluginInstalledResponse> decodePluginInstalledResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginInstalledResponse";
            typed::PluginInstalledResponse result;
            if (!requireObject(value, Context, error) ||
                !decodeOptional(
                    value,
                    "marketplaceLoadErrors",
                    result.marketplaceLoadErrors,
                    [&](const Json& item, std::vector<typed::MarketplaceLoadErrorInfo>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::MarketplaceLoadErrorInfo& info, const std::string& childPath) {
                                return decodeMarketplaceLoadErrorInfoAt(child, info, error, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "marketplaces",
                    result.marketplaces,
                    [&](const Json& item, std::vector<typed::PluginMarketplaceEntry>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::PluginMarketplaceEntry& entry, const std::string& childPath) {
                                return decodePluginMarketplaceEntryAt(child, entry, error, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context)) {
                return std::nullopt;
            }
            if (result.marketplaceLoadErrors) {
                for (const auto& info : *result.marketplaceLoadErrors) {
                    appendDiagnostics(result.diagnostics, info.diagnostics);
                }
            }
            for (const auto& marketplace : result.marketplaces) {
                appendDiagnostics(result.diagnostics, marketplace.diagnostics);
            }
            result.raw = value;
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "PluginInstalledResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::PluginListResponse> decodePluginListResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginListResponse";
            typed::PluginListResponse result;
            if (!requireObject(value, Context, error) ||
                !decodeOptional(
                    value,
                    "featuredPluginIds",
                    result.featuredPluginIds,
                    [&](const Json& item, std::vector<std::string>& decoded, std::string_view itemPath) {
                        return decodeStringVectorAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context) ||
                !decodeOptional(
                    value,
                    "marketplaceLoadErrors",
                    result.marketplaceLoadErrors,
                    [&](const Json& item, std::vector<typed::MarketplaceLoadErrorInfo>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::MarketplaceLoadErrorInfo& info, const std::string& childPath) {
                                return decodeMarketplaceLoadErrorInfoAt(child, info, error, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "marketplaces",
                    result.marketplaces,
                    [&](const Json& item, std::vector<typed::PluginMarketplaceEntry>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::PluginMarketplaceEntry& entry, const std::string& childPath) {
                                return decodePluginMarketplaceEntryAt(child, entry, error, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context)) {
                return std::nullopt;
            }
            if (result.marketplaceLoadErrors) {
                for (const auto& info : *result.marketplaceLoadErrors) {
                    appendDiagnostics(result.diagnostics, info.diagnostics);
                }
            }
            for (const auto& marketplace : result.marketplaces) {
                appendDiagnostics(result.diagnostics, marketplace.diagnostics);
            }
            result.raw = value;
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "PluginListResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::PluginReadResponse> decodePluginReadResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginReadResponse";
            typed::PluginReadResponse result;
            if (!requireObject(value, Context, error) ||
                !decodeRequired(
                    value,
                    "plugin",
                    result.plugin,
                    [&](const Json& item, typed::PluginDetail& decoded, std::string_view itemPath) {
                        return decodePluginDetailAt(item, decoded, error, itemPath);
                    },
                    error,
                    Context)) {
                return std::nullopt;
            }
            appendDiagnostics(result.diagnostics, result.plugin.diagnostics);
            result.raw = value;
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "PluginReadResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::PluginShareListResponse> decodePluginShareListResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "PluginShareListResponse";
            typed::PluginShareListResponse result;
            if (!requireObject(value, Context, error) ||
                !decodeRequired(
                    value,
                    "data",
                    result.data,
                    [&](const Json& item, std::vector<typed::PluginShareListItem>& decoded, std::string_view itemPath) {
                        return decodeArrayAt(
                            item,
                            decoded,
                            [&](const Json& child, typed::PluginShareListItem& entry, const std::string& childPath) {
                                return decodePluginShareListItemAt(child, entry, error, childPath);
                            },
                            error,
                            Context,
                            itemPath);
                    },
                    error,
                    Context)) {
                return std::nullopt;
            }
            for (const auto& item : result.data) {
                appendDiagnostics(result.diagnostics, item.diagnostics);
            }
            result.raw = value;
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "PluginShareListResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::PluginSource> decodePluginSource(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            typed::PluginSource result;
            if (!decodePluginSourceAt(value, result, "$")) {
                error = "PluginSource failed while processing JSON";
                return std::nullopt;
            }
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "PluginSource");
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::detail
