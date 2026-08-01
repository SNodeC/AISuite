/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *               2020, 2021, 2022, 2023, 2024, 2025, 2026
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include <ai/openai/codex/Protocol.h>
#include <ai/openai/codex/stdio/Client.h>
#include <optional>
#include <string>

int main() {
    using namespace ai::openai::codex;

    stdio::Client client;
    AppServerClient::RawProtocol& protocol = client.raw();

    protocol.setOnNotification([](const Notification&) {
    });
    protocol.setOnServerRequest([](const ServerRequest&) {
    });
    protocol.setOnUnknownMessage([](const UnknownMessage&) {
    });

    const Submission request = protocol.request("thread/start", Json::object(), [](const Response&) {
    });
    const SendResult notification = protocol.notify("client/example", Json::object());

    const ServerRequestId integerId(7);
    const SendResult response = protocol.respond(integerId, Json{{"decision", "accept"}});

    const ServerRequestId stringId(std::string("request-7"));
    const SendResult rejection =
        protocol.reject(stringId, ProtocolError{.code = -32000, .message = "Request rejected", .data = std::nullopt});

    const std::optional<typed::InitializeResponse> initializeResponse = client.getInitializeResponse();

    return request || notification || response || rejection || initializeResponse ? 1 : 0;
}
