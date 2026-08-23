/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include <iostream>
#include <nlohmann/json.hpp>
#include <string>

namespace {
    void trace(std::string_view direction, std::string_view event, const nlohmann::json& message) {
        std::cerr << nlohmann::json{{"trace", "codex2.communication"},
                                    {"test", "Provider stdio peer"},
                                    {"boundary", "fake-app-server-process"},
                                    {"direction", direction},
                                    {"event", event},
                                    {"details", message}}
                         .dump()
                  << '\n';
    }
} // namespace

int main() {
    std::string line;
    while (std::getline(std::cin, line)) {
        try {
            const nlohmann::json request = nlohmann::json::parse(line);
            trace("bridge-to-provider", "jsonl-request", request);
            if (!request.is_object() || !request.contains("id") || request.value("method", std::string{}) != "thread/list") {
                return 2;
            }

            const nlohmann::json response{
                {"jsonrpc", "2.0"},
                {"id", request.at("id")},
                {"result", {{"data", nlohmann::json::array()}, {"nextCursor", "stdio-response"}, {"wire", "stdio-jsonl"}}}};
            trace("provider-to-bridge", "jsonl-response", response);
            std::cout << response.dump() << '\n' << std::flush;

            const nlohmann::json notification{{"jsonrpc", "2.0"},
                                              {"method", "thread/name/updated"},
                                              {"params", {{"threadId", "stdio-thread"}, {"threadName", "stdio-provider"}}}};
            trace("provider-to-bridge", "jsonl-notification", notification);
            std::cout << notification.dump() << '\n' << std::flush;
        } catch (const nlohmann::json::exception& exception) {
            std::cerr << "fake stdio app-server JSON error: " << exception.what() << '\n';
            return 3;
        }
    }
    return 0;
}
