/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/protocol/JsonLineFramer.h"

#include "CommunicationTrace.h"
#include "TestHarness.h"

#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <vector>

int main() {
    namespace protocol = ai::openai::codex::protocol;

    tests::codex::TestHarness test;
    std::vector<nlohmann::json> messages;
    std::vector<std::string> errors;
    const auto onMessage = [&messages](nlohmann::json message) {
        tests::codex::traceCommunication("JsonLineFramer", "framer", "decoded", "json-message", message);
        messages.push_back(std::move(message));
    };
    const auto onError = [&errors](std::string error) {
        tests::codex::traceCommunication("JsonLineFramer", "framer", "terminal", "framing-error", {{"reason", error}});
        errors.push_back(std::move(error));
    };

    protocol::JsonLineFramer fragmented(64);
    tests::codex::traceCommunication("JsonLineFramer", "byte-stream", "inbound", "fragment", {{"bytes", 9}, {"data", "{\"first\":"}});
    test.expect(fragmented.consume("{\"first\":", onMessage, onError), "a fragmented frame prefix is retained");
    test.expectEqual(fragmented.bufferedBytes(), std::size_t{9}, "the exact fragmented prefix remains buffered");
    tests::codex::traceCommunication(
        "JsonLineFramer", "byte-stream", "inbound", "coalesced-fragment", {{"bytes", 19}, {"data", "1}\\r\\n{\\\"second\\\":2}\\n\\n"}});
    test.expect(fragmented.consume("1}\r\n{\"second\":2}\n\n", onMessage, onError),
                "fragment completion, coalesced frame, CRLF, and empty lines are accepted");
    test.expectEqual(messages.size(), std::size_t{2}, "two coalesced JSON messages are emitted exactly once");
    test.expect(messages.size() == 2 && messages[0] == nlohmann::json{{"first", 1}} && messages[1] == nlohmann::json{{"second", 2}},
                "fragmented and coalesced payloads retain content and order");
    test.expectEqual(fragmented.bufferedBytes(), std::size_t{0}, "complete frames leave no buffered bytes");
    test.expect(errors.empty() && !fragmented.failed(), "valid framing produces no terminal error");

    protocol::JsonLineFramer callbackBoundary(64);
    bool callbackThrew = false;
    try {
        callbackBoundary.consume(
            "{\"valid\":true}\n",
            [](nlohmann::json) {
                throw nlohmann::json::type_error::create(
                    302, "application callback failure", nullptr);
            },
            onError);
    } catch (const nlohmann::json::exception&) {
        callbackThrew = true;
    }
    test.expect(callbackThrew && !callbackBoundary.failed(),
                "application JSON exceptions propagate without poisoning framing");
    test.expect(callbackBoundary.consume("{}\n", onMessage, onError),
                "framing remains usable after an application callback failure");

    protocol::JsonLineFramer invalid(64);
    test.expect(!invalid.consume("{not-json}\n", onMessage, onError), "invalid JSON is rejected");
    test.expect(invalid.failed() && !invalid.consume("{}\n", onMessage, onError), "a failed framer remains terminal until reset");
    invalid.reset();
    test.expect(invalid.consume("{}\n", onMessage, onError) && !invalid.failed(), "reset permits a new framing lifecycle");

    protocol::JsonLineFramer oversized(8);
    test.expect(!oversized.consume("123456789", onMessage, onError), "an unterminated frame over the configured limit is rejected");
    test.expect(errors.size() == 2, "invalid JSON and oversize each report one bounded error");

    bool encodeRejected = false;
    try {
        static_cast<void>(protocol::JsonLineFramer::encode(nlohmann::json{{"payload", "too-large"}}, 8));
    } catch (const std::length_error&) {
        encodeRejected = true;
    }
    test.expect(encodeRejected, "outbound serialization enforces the same frame bound");

    return test.result();
}
