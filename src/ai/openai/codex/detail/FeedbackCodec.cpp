/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/FeedbackCodec.h"

#include "ai/openai/codex/typed/Types.h"

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

        template <typename T, typename Encoder>
        bool encodeOptionalNullable(
            Json& object, std::string_view field, const typed::OptionalNullable<T>& value, std::string& error, Encoder&& encoder) {
            if (!value.present && value.value.has_value()) {
                error = "FeedbackUploadParams field '$." + std::string(field) + "' has an inconsistent omission state";
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

        Json stringArray(const std::vector<std::string>& values) {
            Json result = Json::array();
            for (const auto& value : values) {
                result.push_back(value);
            }
            return result;
        }

        Json stringMap(const std::map<std::string, std::string>& values) {
            Json result = Json::object();
            for (const auto& [key, value] : values) {
                result[key] = value;
            }
            return result;
        }
    } // namespace

    std::optional<Json> encodeFeedbackUploadParams(const typed::FeedbackUploadParams& value, std::string& error) noexcept {
        try {
            error.clear();
            if (!value.raw.is_object()) {
                expected(error, "FeedbackUploadParams", "$.raw", "an object");
                return std::nullopt;
            }
            Json result = value.raw;
            for (const char* field : {"classification", "extraLogFiles", "includeLogs", "reason", "tags", "threadId"}) {
                result.erase(field);
            }
            result["classification"] = value.classification;
            if (!encodeOptionalNullable(result, "extraLogFiles", value.extraLogFiles, error, stringArray) ||
                !encodeOptionalNullable(result,
                                        "reason",
                                        value.reason,
                                        error,
                                        [](const std::string& input) {
                                            return Json(input);
                                        }) ||
                !encodeOptionalNullable(result, "tags", value.tags, error, stringMap) ||
                !encodeOptionalNullable(result, "threadId", value.threadId, error, [](const typed::ThreadId& input) {
                    return Json(input.value);
                })) {
                return std::nullopt;
            }
            if (value.includeLogs) {
                result["includeLogs"] = *value.includeLogs;
            }
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            error = "FeedbackUploadParams encoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::FeedbackUploadResponse> decodeFeedbackUploadResponse(const Json& value, std::string& error) noexcept {
        try {
            error.clear();
            if (!value.is_object()) {
                expected(error, "FeedbackUploadResponse", "$", "an object");
                return std::nullopt;
            }
            const auto threadId = value.find("threadId");
            if (threadId == value.end()) {
                error = "FeedbackUploadResponse is missing required field '$.threadId'";
                return std::nullopt;
            }
            if (!threadId->is_string()) {
                expected(error, "FeedbackUploadResponse", "$.threadId", "a string");
                return std::nullopt;
            }
            typed::FeedbackUploadResponse result;
            result.threadId.value = threadId->get<std::string>();
            result.raw = value;
            return result;
        } catch (...) {
            error = "FeedbackUploadResponse decoding failed safely";
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::detail
