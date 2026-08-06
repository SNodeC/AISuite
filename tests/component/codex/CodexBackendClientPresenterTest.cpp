/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "apps/codex-backend-client/Presenter.h"
#include "support/TestResult.h"

#include <optional>
#include <sstream>
#include <string>
#include <utility>

namespace {
    namespace client = apps::codex_backend_client;
    namespace frontend = ai::openai::codex::frontend;

    frontend::ServerMessage success(std::string requestId, frontend::Json result) {
        return frontend::ServerMessage{frontend::Response::success(std::move(requestId), std::move(result))};
    }

    frontend::ServerMessage failure(std::string requestId, std::string message) {
        return frontend::ServerMessage{frontend::Response::failure(
            std::move(requestId),
            frontend::CommandError{frontend::ErrorCode::InvalidCommand, std::move(message), std::nullopt, frontend::Json::object()})};
    }

    void testHumanProtocolPresentation(tests::support::TestResult& result) {
        std::ostringstream output;
        std::ostringstream diagnostics;
        client::Presenter presenter(client::OutputMode::Human, output, diagnostics);

        presenter.present(success("start-1", frontend::Json{{"thread", {{"id", "thread-created"}, {"turns", frontend::Json::array()}}}}));
        presenter.present(failure("start-failed", "controller ownership is required"));

        result.expectTrue(
            output.str() ==
                "response request-id=start-1 ok=true result=thread=thread-created status=unknown turns=0\n"
                "response request-id=start-failed ok=false error=invalid_command message=controller ownership is required\n",
            "human mode observes protocol responses without command correlation or result-specific decoding");
        result.expectTrue(diagnostics.str().empty(), "protocol responses remain protocol output rather than local diagnostics");
    }

    void testJsonPreservesOriginalMessages(tests::support::TestResult& result) {
        std::ostringstream output;
        std::ostringstream diagnostics;
        client::Presenter presenter(client::OutputMode::Json, output, diagnostics);
        const frontend::ServerMessage message =
            success("json-new-start", frontend::Json{{"thread", {{"id", "thread-json"}, {"preview", std::string(5000, 'p')}}}});

        presenter.present(message);
        const auto encoded = frontend::Codec::serializeServer(message);

        result.expectTrue(encoded.hasValue() && output.str() == encoded.value() + "\n",
                          "JSON mode emits the complete original Frontend Protocol response through read-only SDK observability");
        result.expectTrue(diagnostics.str().empty(), "JSON protocol observation adds no synthetic diagnostic or protocol message");
    }

    void testJsonLocalFailureDoesNotCorruptProtocolOutput(tests::support::TestResult& result) {
        std::ostringstream output;
        std::ostringstream diagnostics;
        client::Presenter presenter(client::OutputMode::Json, output, diagnostics);

        presenter.error("frontend SDK protocol failure: synchronization message outside synchronization");

        result.expectTrue(output.str().empty(), "a local SDK failure never writes a non-protocol record to JSON stdout");
        result.expectTrue(diagnostics.str() ==
                              "codex-backend-client: frontend SDK protocol failure: synchronization message outside synchronization\n",
                          "JSON mode routes one safe concrete SDK failure through the existing diagnostic presenter");
    }
} // namespace

int main() {
    tests::support::TestResult result;

    testHumanProtocolPresentation(result);
    testJsonPreservesOriginalMessages(result);
    testJsonLocalFailureDoesNotCorruptProtocolOutput(result);

    return result.processResult();
}
