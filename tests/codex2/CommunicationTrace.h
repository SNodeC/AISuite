/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AISUITE_TESTS_CODEX2_COMMUNICATIONTRACE_H
#define AISUITE_TESTS_CODEX2_COMMUNICATIONTRACE_H

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace tests::codex2 {

    inline nlohmann::json boundedTraceValue(const nlohmann::json& value, std::size_t depth = 0) {
        constexpr std::size_t MaximumDepth = 6;
        constexpr std::size_t MaximumStringBytes = 160;
        constexpr std::size_t MaximumCollectionItems = 16;

        if (depth >= MaximumDepth) {
            return {{"summary", "maximum trace depth reached"}};
        }
        if (value.is_string()) {
            const std::string text = value.get<std::string>();
            if (text.size() <= MaximumStringBytes) {
                return text;
            }
            return {{"prefix", text.substr(0, MaximumStringBytes)}, {"originalBytes", text.size()}, {"truncated", true}};
        }
        if (value.is_array()) {
            nlohmann::json result = nlohmann::json::array();
            const std::size_t count = value.size() < MaximumCollectionItems ? value.size() : MaximumCollectionItems;
            for (std::size_t index = 0; index < count; ++index) {
                result.push_back(boundedTraceValue(value[index], depth + 1));
            }
            if (value.size() > count) {
                result.push_back({{"omittedItems", value.size() - count}});
            }
            return result;
        }
        if (value.is_object()) {
            nlohmann::json result = nlohmann::json::object();
            std::size_t count = 0;
            for (const auto& [key, member] : value.items()) {
                if (count == MaximumCollectionItems) {
                    result["traceOmittedMembers"] = value.size() - count;
                    break;
                }
                result[key] = boundedTraceValue(member, depth + 1);
                ++count;
            }
            return result;
        }
        return value;
    }

    inline void traceCommunication(std::string_view test,
                                   std::string_view boundary,
                                   std::string_view direction,
                                   std::string_view event,
                                   const nlohmann::json& details = nlohmann::json::object()) {
        static std::uint64_t sequence = 0;
        const nlohmann::json record{{"trace", "codex2.communication"},
                                    {"sequence", ++sequence},
                                    {"test", test},
                                    {"boundary", boundary},
                                    {"direction", direction},
                                    {"event", event},
                                    {"details", boundedTraceValue(details)}};
        std::cout << record.dump() << '\n';
    }

} // namespace tests::codex2

#endif
