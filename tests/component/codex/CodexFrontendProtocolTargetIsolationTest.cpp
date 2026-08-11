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
#include "ai/openai/codex/frontend/Security.h"

#include <iostream>
#include <string_view>

namespace {
    using namespace ai::openai::codex::frontend;

    bool expect(bool condition, std::string_view description) {
        if (!condition) {
            std::cerr << "protocol target isolation failure: " << description << '\n';
        }
        return condition;
    }
} // namespace

int main() {
    bool passed = true;

    passed = expect(ProtocolIdentity == "snodec.codex-frontend", "protocol identity") && passed;
    passed = expect(ProtocolVersion == 1, "protocol version") && passed;
    passed = expect(generated::MethodCount == 105, "generated method authority") && passed;

    const auto threadStart = generated::definedMethodFromString("thread.start");
    passed = expect(threadStart.has_value(), "generated method lookup") && passed;
    passed = expect(threadStart && generated::methodString(*threadStart) == "thread.start", "generated method metadata") && passed;

    passed = expect(toString(ThreadItemKind::AgentMessage) == "agentMessage", "enum-to-string conversion") && passed;
    passed = expect(threadItemKindFromString("agentMessage") == ThreadItemKind::AgentMessage, "string-to-enum conversion") && passed;

    Hello hello;
    hello.resumeAfter = SequenceNumber{41};
    const ClientMessage clientMessage{hello};
    const auto encodedClient = Codec::encodeClient(clientMessage);
    passed = expect(encodedClient.hasValue(), "client-message encoding") && passed;
    if (encodedClient) {
        const auto decodedClient = Codec::decodeClient(encodedClient.value());
        passed = expect(decodedClient.hasValue(), "client-message decoding") && passed;
        passed = expect(decodedClient && decodedClient.value() == clientMessage, "client-message round trip") && passed;
    }

    const Welcome welcome{"protocol-target-session", SessionRole::Observer, SequenceNumber{41}, SyncMode::Replay};
    const ServerMessage serverMessage{welcome};
    const auto encodedServer = Codec::encodeServer(serverMessage);
    passed = expect(encodedServer.hasValue(), "server-message encoding") && passed;
    if (encodedServer) {
        const auto decodedServer = Codec::decodeServer(encodedServer.value());
        passed = expect(decodedServer.hasValue(), "server-message decoding") && passed;
        passed = expect(decodedServer && decodedServer.value() == serverMessage, "server-message round trip") && passed;
    }

    return passed ? 0 : 1;
}
