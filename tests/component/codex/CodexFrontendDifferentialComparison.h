/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef TESTS_COMPONENT_CODEX_CODEXFRONTENDDIFFERENTIALCOMPARISON_H
#define TESTS_COMPONENT_CODEX_CODEXFRONTENDDIFFERENTIALCOMPARISON_H

#include "ai/openai/codex/frontend/Messages.h"

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace tests::codex::differential {

    struct JsonMismatch {
        std::string path;
        std::string oldValue;
        std::string newValue;

        bool operator==(const JsonMismatch&) const = default;
    };

    inline bool sensitivePath(std::string_view path) {
        std::string lowered;
        lowered.reserve(path.size());
        for (const char rawCharacter : path) {
            const unsigned char character = static_cast<unsigned char>(rawCharacter);
            if (std::isalnum(character) != 0) {
                lowered.push_back(static_cast<char>(std::tolower(character)));
            }
        }
        return lowered.find("token") != std::string::npos || lowered.find("secret") != std::string::npos ||
               lowered.find("authorization") != std::string::npos || lowered.find("credential") != std::string::npos ||
               lowered.find("password") != std::string::npos || lowered.find("apikey") != std::string::npos ||
               lowered.find("answer") != std::string::npos;
    }

    inline std::string boundedValue(const ai::openai::codex::frontend::Json& value, std::string_view path) {
        if (sensitivePath(path)) {
            return "<redacted>";
        }
        if (value.is_object()) {
            return "object(" + std::to_string(value.size()) + ")";
        }
        if (value.is_array()) {
            return "array(" + std::to_string(value.size()) + ")";
        }
        std::string encoded = value.dump(-1, ' ', true);
        constexpr std::size_t MaximumDiagnosticBytes = 160;
        if (encoded.size() > MaximumDiagnosticBytes) {
            encoded.resize(MaximumDiagnosticBytes);
            encoded += "...";
        }
        return encoded;
    }

    inline std::optional<JsonMismatch> firstMismatch(const ai::openai::codex::frontend::Json& oldValue,
                                                     const ai::openai::codex::frontend::Json& newValue,
                                                     std::string path = "$") {
        // The wire border has one JSON number token kind.  nlohmann_json keeps
        // the signed/unsigned origin in memory, so compare exact number tokens
        // before comparing its internal value type.
        if (oldValue.is_number() && newValue.is_number() && oldValue.dump() == newValue.dump()) {
            return std::nullopt;
        }
        if (oldValue.type() != newValue.type()) {
            const std::string oldDiagnostic = boundedValue(oldValue, path);
            const std::string newDiagnostic = boundedValue(newValue, path);
            return JsonMismatch{std::move(path), oldDiagnostic, newDiagnostic};
        }
        if (oldValue.is_object()) {
            for (const auto& [key, value] : oldValue.items()) {
                const std::string childPath = path + "/" + key;
                const auto found = newValue.find(key);
                if (found == newValue.end()) {
                    return JsonMismatch{childPath, boundedValue(value, childPath), "<omitted>"};
                }
                if (auto mismatch = firstMismatch(value, *found, childPath)) {
                    return mismatch;
                }
            }
            for (const auto& [key, value] : newValue.items()) {
                if (!oldValue.contains(key)) {
                    const std::string childPath = path + "/" + key;
                    return JsonMismatch{childPath, "<omitted>", boundedValue(value, childPath)};
                }
            }
            return std::nullopt;
        }
        if (oldValue.is_array()) {
            if (oldValue.size() != newValue.size()) {
                return JsonMismatch{path + "/size", std::to_string(oldValue.size()), std::to_string(newValue.size())};
            }
            for (std::size_t index = 0; index < oldValue.size(); ++index) {
                if (auto mismatch = firstMismatch(oldValue[index], newValue[index], path + "/" + std::to_string(index))) {
                    return mismatch;
                }
            }
            return std::nullopt;
        }
        if (oldValue != newValue) {
            const std::string oldDiagnostic = boundedValue(oldValue, path);
            const std::string newDiagnostic = boundedValue(newValue, path);
            return JsonMismatch{std::move(path), oldDiagnostic, newDiagnostic};
        }
        return std::nullopt;
    }

} // namespace tests::codex::differential

#endif // TESTS_COMPONENT_CODEX_CODEXFRONTENDDIFFERENTIALCOMPARISON_H
