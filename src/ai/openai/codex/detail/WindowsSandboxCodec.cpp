/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/detail/WindowsSandboxCodec.h"

#include "ai/openai/codex/detail/DecodeDiagnostic.h"

#include <string>
#include <string_view>
#include <utility>

namespace ai::openai::codex::detail {

    namespace {

        const Json* member(const Json& object, std::string_view name) noexcept {
            if (!object.is_object()) {
                return nullptr;
            }
            const auto iterator = object.find(name);
            return iterator == object.end() ? nullptr : &*iterator;
        }

        void expected(std::string& error, std::string_view surface, std::string_view path, std::string_view type) {
            error = std::string(surface) + " field '" + std::string(path) + "' must be " + std::string(type);
        }

        void missing(std::string& error, std::string_view surface, std::string_view path) {
            error = std::string(surface) + " is missing required field '" + std::string(path) + "'";
        }

        bool decodeObject(const Json& value, std::string& error, std::string_view surface) {
            if (!value.is_object()) {
                expected(error, surface, "$", "an object");
                return false;
            }
            return true;
        }

        bool decodeBoolean(const Json& value, bool& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!value.is_boolean()) {
                expected(error, surface, path, "a boolean");
                return false;
            }
            output = value.get<bool>();
            return true;
        }

        bool decodeString(const Json& value, std::string& output, std::string& error, std::string_view surface, std::string_view path) {
            if (!value.is_string()) {
                expected(error, surface, path, "a string");
                return false;
            }
            output = value.get_ref<const std::string&>();
            return true;
        }

        template <typename T, typename Decoder>
        bool decodeRequired(
            const Json& object, std::string_view field, T& output, std::string& error, std::string_view surface, Decoder&& decoder) {
            const std::string path = "$." + std::string(field);
            const Json* value = member(object, field);
            if (value == nullptr) {
                missing(error, surface, path);
                return false;
            }
            return decoder(*value, output, error, surface, path);
        }

    } // namespace

    std::optional<Json> encodeWindowsSandboxSetupStartParams(const typed::WindowsSandboxSetupStartParams& value,
                                                             std::string& error) noexcept {
        try {
            error.clear();
            if (!value.raw.is_object()) {
                expected(error, "WindowsSandboxSetupStartParams", "$.raw", "an object");
                return std::nullopt;
            }

            Json result = value.raw;
            result.erase("cwd");
            result.erase("mode");
            if (value.cwd.isNull()) {
                result["cwd"] = nullptr;
            } else if (value.cwd.hasValue()) {
                result["cwd"] = value.cwd.value->value;
            }
            result["mode"] = value.mode.value;
            return std::optional<Json>{std::move(result)};
        } catch (...) {
            error = "WindowsSandboxSetupStartParams encoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::WindowsSandboxReadinessResponse> decodeWindowsSandboxReadinessResponse(const Json& value,
                                                                                                std::string& error) noexcept {
        try {
            error.clear();
            typed::WindowsSandboxReadinessResponse result;
            if (!decodeObject(value, error, "WindowsSandboxReadinessResponse") ||
                !decodeRequired(value, "status", result.status.value, error, "WindowsSandboxReadinessResponse", decodeString)) {
                return std::nullopt;
            }
            result.raw = value;
            if (!result.status.isKnown()) {
                result.diagnostics.emplace_back(unknownEnumDiagnostic("WindowsSandboxReadiness", "$.status"));
            }
            return result;
        } catch (...) {
            error = "WindowsSandboxReadinessResponse decoding failed safely";
            return std::nullopt;
        }
    }

    std::optional<typed::WindowsSandboxSetupStartResponse> decodeWindowsSandboxSetupStartResponse(const Json& value,
                                                                                                  std::string& error) noexcept {
        try {
            error.clear();
            typed::WindowsSandboxSetupStartResponse result;
            if (!decodeObject(value, error, "WindowsSandboxSetupStartResponse") ||
                !decodeRequired(value, "started", result.started, error, "WindowsSandboxSetupStartResponse", decodeBoolean)) {
                return std::nullopt;
            }
            result.raw = value;
            return result;
        } catch (...) {
            error = "WindowsSandboxSetupStartResponse decoding failed safely";
            return std::nullopt;
        }
    }

} // namespace ai::openai::codex::detail
