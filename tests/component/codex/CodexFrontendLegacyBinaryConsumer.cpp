/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/Codec.h"
#include "ai/openai/codex/frontend/GeneratedProtocol.h"
#include "ai/openai/codex/frontend/Messages.h"

#include <iostream>

int main() {
    using namespace ai::openai::codex::frontend;

    const ClientMessage client{Hello{SequenceNumber{91}}};
    const auto encodedClient = Codec::encodeClient(client);
    const auto decodedClient =
        encodedClient ? Codec::decodeClient(encodedClient.value()) : CodecResult<ClientMessage>{encodedClient.error()};
    if (!decodedClient || decodedClient.value() != client) {
        return 1;
    }

    const ServerMessage server{Welcome{"p0-linked-consumer", SessionRole::Observer, SequenceNumber{91}, SyncMode::Replay}};
    const auto encodedServer = Codec::encodeServer(server);
    const auto decodedServer =
        encodedServer ? Codec::decodeServer(encodedServer.value()) : CodecResult<ServerMessage>{encodedServer.error()};
    if (!decodedServer || decodedServer.value() != server) {
        return 2;
    }

    if (toString(ThreadItemKind::CommandExecution) != "commandExecution" ||
        threadItemKindFromString("commandExecution") != ThreadItemKind::CommandExecution) {
        return 3;
    }

    const auto method = generated::definedMethodFromString("thread.start");
    if (!method || generated::methodString(*method) != "thread.start") {
        return 4;
    }

    std::cout << "p0-linked-frontend-consumer-ok\n";
    return 0;
}
