/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "../LegacyFrontendClientCapture.h"

#include "ai/openai/codex/frontend/client/Client.h"
#include "ai/openai/codex/frontend/client/detail/StateReducer.h"

#include <utility>

namespace tests::codex::oracle {
    namespace frontend = ai::openai::codex::frontend;
    namespace legacy = ai::openai::codex::frontend::legacy_client;

    LegacyFrontendClientCapture captureLegacyFrontendClient(std::span<const frontend::ServerMessage> messages,
                                                             bool expanded,
                                                             bool disconnectAfterMessages,
                                                             bool closeAfterMessages) {
        LegacyFrontendClientCapture capture;
        legacy::ClientOptions options;
        options.credentialProvider = [] {
            return legacy::AuthenticationContext{frontend::NoCredential{}, std::string{"p3-public-differential"}};
        };
        if (!expanded) {
            options.requestedCapabilities.clear();
        }

        legacy::ClientCallbacks callbacks;
        callbacks.onConnectionStateChanged = [&capture](const legacy::ConnectionStateChange& change) {
            capture.callbacks.emplace_back("connection");
            if (change.error.has_value()) {
                capture.diagnostics.push_back(change.error->message);
            }
        };
        callbacks.onStateUpdated = [&capture](const legacy::StateUpdate&) {
            capture.callbacks.emplace_back("state");
        };
        callbacks.onSynchronized = [&capture](const legacy::SynchronizationInfo&) {
            capture.callbacks.emplace_back("synchronized");
        };
        callbacks.onCursorAdvanced = [&capture](frontend::SequenceNumber) {
            capture.callbacks.emplace_back("cursor");
        };
        callbacks.onProtocolMessage = [&capture](const frontend::ServerMessage&) {
            capture.callbacks.emplace_back("protocol");
        };
        callbacks.onDiagnostic = [&capture](const legacy::Diagnostic& diagnostic) {
            capture.callbacks.emplace_back("diagnostic");
            capture.diagnostics.push_back(diagnostic.message);
        };

        legacy::Client sdk(std::move(options), std::move(callbacks));
        legacy::Connection connection = sdk.openConnection(
            {[&capture](legacy::OutboundMessage message) {
                 capture.outbound.push_back(std::move(message.compactJson));
                 return legacy::SendResult{legacy::SendStatus::Accepted, std::nullopt};
             },
             [&capture](std::string) {
                 ++capture.closes;
             }});
        connection.transportConnected();
        capture.states.push_back(legacy::detail::StateReducer::serializeForTesting(sdk.state()));

        capture.accepted = true;
        for (const frontend::ServerMessage& message : messages) {
            capture.accepted = connection.receive(message).accepted && capture.accepted;
            capture.states.push_back(legacy::detail::StateReducer::serializeForTesting(sdk.state()));
        }
        capture.ready = sdk.isReady();
        if (closeAfterMessages) {
            connection.close("p3 public differential local close");
            capture.states.push_back(legacy::detail::StateReducer::serializeForTesting(sdk.state()));
            capture.ready = sdk.isReady();
        } else if (disconnectAfterMessages) {
            connection.transportDisconnected(legacy::TransportError{"p3 public differential disconnect", true});
            capture.states.push_back(legacy::detail::StateReducer::serializeForTesting(sdk.state()));
            capture.ready = sdk.isReady();
        }
        // Return a frozen copy so local teardown cannot mutate the result via
        // optional NRVO after the requested differential border.
        return LegacyFrontendClientCapture{capture};
    }

} // namespace tests::codex::oracle
