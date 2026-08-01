/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/MarketplaceCodec.h"

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

        bool decodeMarketplaceUpgradeErrorInfo(const Json& value,
                                               typed::MarketplaceUpgradeErrorInfo& result,
                                               std::string& error,
                                               std::string_view path) {
            constexpr std::string_view Context = "MarketplaceUpgradeErrorInfo";
            if (!requireObject(value, Context, error, path) ||
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

        void setUnexpectedFailure(std::string& error, std::string_view context) noexcept {
            try {
                error = std::string(context) + " failed while processing JSON";
            } catch (...) {
            }
        }

    } // namespace

    std::optional<Json> encodeMarketplaceAddParams(const typed::MarketplaceAddParams& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "MarketplaceAddParams";
            if (!validateOptionalNullable(value.refName, error, Context, "$.refName") ||
                !validateOptionalNullable(value.sparsePaths, error, Context, "$.sparsePaths")) {
                return std::nullopt;
            }
            Json result = encoderObject(value.raw, error, Context, {"refName", "source", "sparsePaths"});
            if (!error.empty()) {
                return std::nullopt;
            }
            encodeOptionalNullable(result, "refName", value.refName, [](const std::string& item) {
                return Json(item);
            });
            result["source"] = value.source;
            encodeOptionalNullable(result, "sparsePaths", value.sparsePaths, [](const std::vector<std::string>& item) {
                return Json(item);
            });
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "MarketplaceAddParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodeMarketplaceRemoveParams(const typed::MarketplaceRemoveParams& value, std::string& error) noexcept {
        try {
            error.clear();
            Json result = encoderObject(value.raw, error, "MarketplaceRemoveParams", {"marketplaceName"});
            if (!error.empty()) {
                return std::nullopt;
            }
            result["marketplaceName"] = value.marketplaceName;
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "MarketplaceRemoveParams");
            return std::nullopt;
        }
    }

    std::optional<Json> encodeMarketplaceUpgradeParams(const typed::MarketplaceUpgradeParams& value, std::string& error) noexcept {
        try {
            error.clear();
            if (!validateOptionalNullable(value.marketplaceName, error, "MarketplaceUpgradeParams", "$.marketplaceName")) {
                return std::nullopt;
            }
            Json result = encoderObject(value.raw, error, "MarketplaceUpgradeParams", {"marketplaceName"});
            if (!error.empty()) {
                return std::nullopt;
            }
            encodeOptionalNullable(result, "marketplaceName", value.marketplaceName, [](const std::string& item) {
                return Json(item);
            });
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            setUnexpectedFailure(error, "MarketplaceUpgradeParams");
            return std::nullopt;
        }
    }

    std::optional<typed::MarketplaceAddResponse> decodeMarketplaceAddResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "MarketplaceAddResponse";
            typed::MarketplaceAddResponse result;
            if (!requireObject(value, Context, error) ||
                !decodeRequired(
                    value,
                    "alreadyAdded",
                    result.alreadyAdded,
                    [&](const Json& item, bool& decoded, std::string_view itemPath) {
                        return decodeBoolAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "installedRoot",
                    result.installedRoot,
                    [&](const Json& item, typed::AbsolutePath& decoded, std::string_view itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context) ||
                !decodeRequired(
                    value,
                    "marketplaceName",
                    result.marketplaceName,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context)) {
                return std::nullopt;
            }
            result.raw = value;
            error.clear();
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "MarketplaceAddResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::MarketplaceRemoveResponse> decodeMarketplaceRemoveResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "MarketplaceRemoveResponse";
            typed::MarketplaceRemoveResponse result;
            if (!requireObject(value, Context, error)) {
                return std::nullopt;
            }
            const Json* installedRoot = member(value, "installedRoot");
            if (installedRoot == nullptr) {
                result.installedRoot = typed::OptionalNullable<typed::AbsolutePath>::omitted();
            } else if (installedRoot->is_null()) {
                result.installedRoot = typed::OptionalNullable<typed::AbsolutePath>::explicitNull();
            } else {
                typed::AbsolutePath decoded;
                if (!decodeStrongStringAt(*installedRoot, decoded, error, Context, "$.installedRoot")) {
                    return std::nullopt;
                }
                result.installedRoot = typed::OptionalNullable<typed::AbsolutePath>::withValue(std::move(decoded));
            }
            if (!decodeRequired(
                    value,
                    "marketplaceName",
                    result.marketplaceName,
                    [&](const Json& item, std::string& decoded, std::string_view itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context)) {
                return std::nullopt;
            }
            result.raw = value;
            error.clear();
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "MarketplaceRemoveResponse");
            return std::nullopt;
        }
    }

    std::optional<typed::MarketplaceUpgradeResponse> decodeMarketplaceUpgradeResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            constexpr std::string_view Context = "MarketplaceUpgradeResponse";
            typed::MarketplaceUpgradeResponse result;
            if (!requireObject(value, Context, error)) {
                return std::nullopt;
            }
            const Json* errors = member(value, "errors");
            const Json* selectedMarketplaces = member(value, "selectedMarketplaces");
            const Json* upgradedRoots = member(value, "upgradedRoots");
            if (errors == nullptr) {
                fail(error, Context, "$.errors", "is required");
                return std::nullopt;
            }
            if (!decodeArrayAt(
                    *errors,
                    result.errors,
                    [&](const Json& item, typed::MarketplaceUpgradeErrorInfo& decoded, const std::string& itemPath) {
                        return decodeMarketplaceUpgradeErrorInfo(item, decoded, error, itemPath);
                    },
                    error,
                    Context,
                    "$.errors")) {
                return std::nullopt;
            }
            if (selectedMarketplaces == nullptr) {
                fail(error, Context, "$.selectedMarketplaces", "is required");
                return std::nullopt;
            }
            if (!decodeArrayAt(
                    *selectedMarketplaces,
                    result.selectedMarketplaces,
                    [&](const Json& item, std::string& decoded, const std::string& itemPath) {
                        return decodeStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    "$.selectedMarketplaces")) {
                return std::nullopt;
            }
            if (upgradedRoots == nullptr) {
                fail(error, Context, "$.upgradedRoots", "is required");
                return std::nullopt;
            }
            if (!decodeArrayAt(
                    *upgradedRoots,
                    result.upgradedRoots,
                    [&](const Json& item, typed::AbsolutePath& decoded, const std::string& itemPath) {
                        return decodeStrongStringAt(item, decoded, error, Context, itemPath);
                    },
                    error,
                    Context,
                    "$.upgradedRoots")) {
                return std::nullopt;
            }
            result.raw = value;
            error.clear();
            return result;
        } catch (...) {
            setUnexpectedFailure(error, "MarketplaceUpgradeResponse");
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::detail
