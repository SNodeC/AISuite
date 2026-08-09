/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Messages.h"
#include "ai/openai/codex/frontend/Protocol.h"

#include <iostream>
#include <string_view>

namespace {
    using namespace ai::openai::codex::frontend;

    bool require(bool condition, std::string_view message) {
        if (!condition) {
            std::cerr << "installed protocol consumer: " << message << '\n';
        }
        return condition;
    }
} // namespace

int main() {
    using namespace ai::openai::codex::frontend;

    bool passed = require(ProtocolIdentity == "snodec.codex-frontend", "protocol identity changed");
    passed = require(ProtocolVersion == 1, "protocol version changed") && passed;
    passed = require(generated::MethodCount == 105, "method metadata is incomplete") && passed;

    const auto method = generated::definedMethodFromString("thread.start");
    passed = require(method && generated::methodString(*method) == "thread.start", "method lookup failed") && passed;

    const ClientMessage client{Hello{SequenceNumber{7}}};
    const auto encodedClient = Codec::encodeClient(client);
    passed = require(encodedClient.hasValue(), "client encode failed") && passed;
    if (encodedClient) {
        const auto decoded = Codec::decodeClient(encodedClient.value());
        passed = require(decoded && decoded.value() == client, "client round trip failed") && passed;
    }

    const ServerMessage server{Welcome{"protocol-consumer", SessionRole::Observer, SequenceNumber{7}, SyncMode::Replay}};
    const auto encodedServer = Codec::encodeServer(server);
    passed = require(encodedServer.hasValue(), "server encode failed") && passed;
    if (encodedServer) {
        const auto decoded = Codec::decodeServer(encodedServer.value());
        passed = require(decoded && decoded.value() == server, "server round trip failed") && passed;
    }

    ExpandedSnapshot snapshot;
    snapshot.sequence = SequenceNumber{7};
    snapshot.state.provider = {
        {"lifecycle", "stopped"},
        {"generation", 0},
        {"desiredRunning", false},
        {"recovery", {{"status", "idle"}, {"attempts", 0}}},
    };
    snapshot.state.controller = {{"present", false}};
    snapshot.state.threadList = {{"hasLoadedPage", false}, {"complete", false}, {"pagesLoaded", 0}};
    snapshot.state.truncation = {{"truncated", false}};
    const auto encodedSnapshot = Codec::encodeExpandedSnapshot(snapshot);
    passed = require(encodedSnapshot.hasValue(), "expanded snapshot encode failed") && passed;
    if (encodedSnapshot) {
        const auto decoded = Codec::decodeExpandedSnapshot(encodedSnapshot.value());
        passed = require(decoded && decoded.value() == snapshot, "expanded snapshot round trip failed") && passed;
    }

    ExpandedFrontendEvent event{SequenceNumber{8}, ExpandedEventType::ProviderUpdated, {{"provider", snapshot.state.provider}}};
    const auto encodedEvent = Codec::encodeExpandedEvent(event);
    passed = require(encodedEvent.hasValue(), "expanded event encode failed") && passed;
    if (encodedEvent) {
        const auto decoded = Codec::decodeExpandedEvent(encodedEvent.value());
        passed = require(decoded && decoded.value() == event, "expanded event round trip failed") && passed;
    }

    return passed ? 0 : 1;
}
