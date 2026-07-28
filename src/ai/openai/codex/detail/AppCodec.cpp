/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/AppCodec.h"

#include "ai/openai/codex/detail/DecodeDiagnostic.h"
#include "ai/openai/codex/typed/Types.h"

#include <cstddef>
#include <cstdint>
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

        bool decodeObject(const Json& value, std::string& error, std::string_view surface, std::string_view path) {
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

        bool decodeBoolean(const Json& value,
                           bool& output,
                           std::string& error,
                           std::string_view surface,
                           std::string_view path) {
            if (!value.is_boolean()) {
                expected(error, surface, path, "a boolean");
                return false;
            }
            output = value.get<bool>();
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

        bool decodeStringArray(const Json& value,
                               std::vector<std::string>& output,
                               std::string& error,
                               std::string_view surface,
                               std::string_view path) {
            return decodeArray<std::string>(value, output, error, surface, path, decodeString);
        }

        bool decodeStringMap(const Json& value,
                             std::map<std::string, std::string>& output,
                             std::string& error,
                             std::string_view surface,
                             std::string_view path) {
            if (!value.is_object()) {
                expected(error, surface, path, "an object with string values");
                return false;
            }
            output.clear();
            for (const auto& [key, item] : value.items()) {
                if (!item.is_string()) {
                    expected(error, surface, fieldPath(path, "*"), "a string");
                    return false;
                }
                output.emplace(key, item.get<std::string>());
            }
            return true;
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

        bool decodeAppReviewValue(const Json& value,
                                  typed::AppReview& output,
                                  std::string& error,
                                  std::string_view surface,
                                  std::string_view path);
        bool decodeAppScreenshotValue(const Json& value,
                                      typed::AppScreenshot& output,
                                      std::string& error,
                                      std::string_view surface,
                                      std::string_view path);
        bool decodeAppMetadataValue(const Json& value,
                                    typed::AppMetadata& output,
                                    std::string& error,
                                    std::string_view surface,
                                    std::string_view path);

        bool decodeAppBrandingValue(const Json& value,
                                    typed::AppBranding& output,
                                    std::string& error,
                                    std::string_view surface,
                                    std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptionalNullable(value, "category", output.category, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "developer", output.developer, error, surface, path, decodeString) &&
                   decodeRequired(value, "isDiscoverableApp", output.isDiscoverableApp, error, surface, path, decodeBoolean) &&
                   decodeOptionalNullable(value, "privacyPolicy", output.privacyPolicy, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "termsOfService", output.termsOfService, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "website", output.website, error, surface, path, decodeString);
        }

        bool decodeAppReviewValue(const Json& value,
                                  typed::AppReview& output,
                                  std::string& error,
                                  std::string_view surface,
                                  std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeRequired(value, "status", output.status, error, surface, path, decodeString);
        }

        bool decodeAppScreenshotValue(const Json& value,
                                      typed::AppScreenshot& output,
                                      std::string& error,
                                      std::string_view surface,
                                      std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptionalNullable(value, "fileId", output.fileId, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "url", output.url, error, surface, path, decodeString) &&
                   decodeRequired(value, "userPrompt", output.userPrompt, error, surface, path, decodeString);
        }

        bool decodeAppMetadataValue(const Json& value,
                                    typed::AppMetadata& output,
                                    std::string& error,
                                    std::string_view surface,
                                    std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            if (!decodeOptionalNullable(value, "categories", output.categories, error, surface, path, decodeStringArray) ||
                !decodeOptionalNullable(value, "developer", output.developer, error, surface, path, decodeString) ||
                !decodeOptionalNullable(
                    value, "firstPartyRequiresInstall", output.firstPartyRequiresInstall, error, surface, path, decodeBoolean) ||
                !decodeOptionalNullable(value, "firstPartyType", output.firstPartyType, error, surface, path, decodeString) ||
                !decodeOptionalNullable(value, "review", output.review, error, surface, path, decodeAppReviewValue) ||
                !decodeOptionalNullable(
                    value,
                    "screenshots",
                    output.screenshots,
                    error,
                    surface,
                    path,
                    [](const Json& input,
                       std::vector<typed::AppScreenshot>& decoded,
                       std::string& nestedError,
                       std::string_view nestedSurface,
                       std::string_view nestedPath) {
                        return decodeArray<typed::AppScreenshot>(
                            input, decoded, nestedError, nestedSurface, nestedPath, decodeAppScreenshotValue);
                    }) ||
                !decodeOptionalNullable(value, "seoDescription", output.seoDescription, error, surface, path, decodeString) ||
                !decodeOptionalNullable(
                    value, "showInComposerWhenUnlinked", output.showInComposerWhenUnlinked, error, surface, path, decodeBoolean) ||
                !decodeOptionalNullable(value, "subCategories", output.subCategories, error, surface, path, decodeStringArray) ||
                !decodeOptionalNullable(value, "version", output.version, error, surface, path, decodeString) ||
                !decodeOptionalNullable(value, "versionId", output.versionId, error, surface, path, decodeString) ||
                !decodeOptionalNullable(value, "versionNotes", output.versionNotes, error, surface, path, decodeString)) {
                return false;
            }
            if (output.review.hasValue()) {
                appendDiagnostics(output, *output.review);
            }
            if (output.screenshots.hasValue()) {
                for (const auto& screenshot : *output.screenshots) {
                    appendDiagnostics(output, screenshot);
                }
            }
            return true;
        }

        bool decodeAppInfoValue(const Json& value,
                                typed::AppInfo& output,
                                std::string& error,
                                std::string_view surface,
                                std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            if (!decodeOptionalNullable(value, "appMetadata", output.appMetadata, error, surface, path, decodeAppMetadataValue) ||
                !decodeOptionalNullable(value, "branding", output.branding, error, surface, path, decodeAppBrandingValue) ||
                !decodeOptionalNullable(value, "description", output.description, error, surface, path, decodeString) ||
                !decodeOptionalNullable(value, "distributionChannel", output.distributionChannel, error, surface, path, decodeString) ||
                !decodeOptionalNullable(value, "iconAssets", output.iconAssets, error, surface, path, decodeStringMap) ||
                !decodeOptionalNullable(value, "iconDarkAssets", output.iconDarkAssets, error, surface, path, decodeStringMap) ||
                !decodeRequired(value, "id", output.id, error, surface, path, decodeString) ||
                !decodeOptionalNullable(value, "installUrl", output.installUrl, error, surface, path, decodeString) ||
                !decodeOptional(value, "isAccessible", output.isAccessible, error, surface, path, decodeBoolean) ||
                !decodeOptional(value, "isEnabled", output.isEnabled, error, surface, path, decodeBoolean) ||
                !decodeOptionalNullable(value, "labels", output.labels, error, surface, path, decodeStringMap) ||
                !decodeOptionalNullable(value, "logoUrl", output.logoUrl, error, surface, path, decodeString) ||
                !decodeOptionalNullable(value, "logoUrlDark", output.logoUrlDark, error, surface, path, decodeString) ||
                !decodeRequired(value, "name", output.name, error, surface, path, decodeString) ||
                !decodeOptional(value, "pluginDisplayNames", output.pluginDisplayNames, error, surface, path, decodeStringArray)) {
                return false;
            }
            if (output.appMetadata.hasValue()) {
                appendDiagnostics(output, *output.appMetadata);
            }
            if (output.branding.hasValue()) {
                appendDiagnostics(output, *output.branding);
            }
            return true;
        }

        bool decodeAppSummaryValue(const Json& value,
                                   typed::AppSummary& output,
                                   std::string& error,
                                   std::string_view surface,
                                   std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptionalNullable(value, "category", output.category, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "description", output.description, error, surface, path, decodeString) &&
                   decodeRequired(value, "id", output.id, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "installUrl", output.installUrl, error, surface, path, decodeString) &&
                   decodeRequired(value, "name", output.name, error, surface, path, decodeString);
        }

        bool decodeTemplateReason(const Json& value,
                                  typed::AppTemplateUnavailableReason& output,
                                  std::string& error,
                                  std::string_view surface,
                                  std::string_view path,
                                  std::vector<typed::DecodeDiagnostic>& diagnostics) {
            if (!decodeString(value, output.value, error, surface, path)) {
                return false;
            }
            if (!output.isKnown()) {
                diagnostics.emplace_back(unknownEnumDiagnostic("AppTemplateUnavailableReason", std::string(path)));
            }
            return true;
        }

        bool decodeAppTemplateSummaryValue(const Json& value,
                                           typed::AppTemplateSummary& output,
                                           std::string& error,
                                           std::string_view surface,
                                           std::string_view path) {
            if (!decodeObject(value, error, surface, path)) {
                return false;
            }
            output.raw = value;
            output.diagnostics.clear();
            return decodeOptionalNullable(value, "canonicalConnectorId", output.canonicalConnectorId, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "category", output.category, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "description", output.description, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "logoUrl", output.logoUrl, error, surface, path, decodeString) &&
                   decodeOptionalNullable(value, "logoUrlDark", output.logoUrlDark, error, surface, path, decodeString) &&
                   decodeRequired(value, "materializedAppIds", output.materializedAppIds, error, surface, path, decodeStringArray) &&
                   decodeRequired(value, "name", output.name, error, surface, path, decodeString) &&
                   decodeOptionalNullable(
                       value,
                       "reason",
                       output.reason,
                       error,
                       surface,
                       path,
                       [&](const Json& input,
                           typed::AppTemplateUnavailableReason& decoded,
                           std::string& nestedError,
                           std::string_view nestedSurface,
                           std::string_view nestedPath) {
                           return decodeTemplateReason(
                               input, decoded, nestedError, nestedSurface, nestedPath, output.diagnostics);
                       }) &&
                   decodeRequired(value, "templateId", output.templateId, error, surface, path, decodeString);
        }

        bool decodeAppsListPayload(const Json& value,
                                   std::vector<typed::AppInfo>& data,
                                   typed::OptionalNullable<std::string>* nextCursor,
                                   std::vector<typed::DecodeDiagnostic>& diagnostics,
                                   std::string& error,
                                   std::string_view surface,
                                   std::string_view path) {
            if (!decodeObject(value, error, surface, path) ||
                !decodeRequired(
                    value,
                    "data",
                    data,
                    error,
                    surface,
                    path,
                    [](const Json& input,
                       std::vector<typed::AppInfo>& output,
                       std::string& nestedError,
                       std::string_view nestedSurface,
                       std::string_view nestedPath) {
                        return decodeArray<typed::AppInfo>(
                            input, output, nestedError, nestedSurface, nestedPath, decodeAppInfoValue);
                    })) {
                return false;
            }
            if (nextCursor != nullptr &&
                !decodeOptionalNullable(value, "nextCursor", *nextCursor, error, surface, path, decodeString)) {
                return false;
            }
            for (const auto& app : data) {
                diagnostics.insert(diagnostics.end(), app.diagnostics.begin(), app.diagnostics.end());
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
            object[std::string(field)] = encoder(*value);
            return true;
        }

        template <typename T>
        Json scalar(const T& value) {
            return Json(value);
        }
    } // namespace

    std::optional<Json> encodeAppsListParams(const typed::AppsListParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(
                value.raw, error, "AppsListParams", "$.raw", {"cursor", "forceRefetch", "limit", "threadId"});
            if (!error.empty()) {
                return std::nullopt;
            }
            if (!encodeOptionalNullable(result, "cursor", value.cursor, error, "AppsListParams", scalar<std::string>) ||
                !encodeOptionalNullable(result, "limit", value.limit, error, "AppsListParams", scalar<std::uint32_t>) ||
                !encodeOptionalNullable(
                    result,
                    "threadId",
                    value.threadId,
                    error,
                    "AppsListParams",
                    [](const typed::ThreadId& id) {
                        return Json(id.value);
                    })) {
                return std::nullopt;
            }
            if (value.forceRefetch) {
                result["forceRefetch"] = *value.forceRefetch;
            }
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            error = "AppsListParams encoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::AppsListResponse> decodeAppsListResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            typed::AppsListResponse result;
            result.raw = value;
            if (!decodeAppsListPayload(
                    value, result.data, &result.nextCursor, result.diagnostics, error, "AppsListResponse", "$")) {
                return std::nullopt;
            }
            return result;
        } catch (...) {
            error = "AppsListResponse decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::AppListUpdatedNotification>
    decodeAppListUpdatedNotification(const Notification& notification, std::string& error) noexcept {
        try {
            error.clear();
            typed::AppListUpdatedNotification result;
            result.raw = notification.raw;
            if (!decodeAppsListPayload(
                    notification.params, result.data, nullptr, result.diagnostics, error, "app/list/updated", "$.params")) {
                return std::nullopt;
            }
            return result;
        } catch (...) {
            error = "app/list/updated decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::AppSummary>
    decodeAppSummary(const Json& value, std::string& error, std::string_view fieldPathValue) noexcept {
        try {
            error.clear();
            typed::AppSummary result;
            if (!decodeAppSummaryValue(value, result, error, "AppSummary", fieldPathValue)) {
                return std::nullopt;
            }
            return result;
        } catch (...) {
            error = "AppSummary decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::AppTemplateSummary>
    decodeAppTemplateSummary(const Json& value, std::string& error, std::string_view fieldPathValue) noexcept {
        try {
            error.clear();
            typed::AppTemplateSummary result;
            if (!decodeAppTemplateSummaryValue(value, result, error, "AppTemplateSummary", fieldPathValue)) {
                return std::nullopt;
            }
            return result;
        } catch (...) {
            error = "AppTemplateSummary decoding failed safely";
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::detail
