/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include <ai/openai/codex/backend/BackendCore.h>
#include <ai/openai/codex/frontend/Codec.h>
#include <ai/openai/codex/frontend/GeneratedProtocol.h>
#include <ai/openai/codex/frontend/Protocol.h>
#include <ai/openai/codex/frontend/Security.h>
#include <ai/openai/codex/stdio/Client.h>
#include <cstdint>
#include <string_view>

int main() {
    using ai::openai::codex::backend::BackendCore;
    using namespace ai::openai::codex::frontend;

    static_assert(ProtocolVersion == std::uint32_t{1});
    static_assert(ProtocolIdentity == std::string_view{"snodec.codex-frontend"});
    static_assert(generated::MethodCount == 105);
    static_assert(generated::ExistingMethodCount == 15);
    static_assert(DefaultRemoteScopes.size() == 2);
    static_assert(method::ThreadStart == std::string_view{"thread.start"});

    BackendCore<ai::openai::codex::stdio::Client> backend;
    const auto decoded = Codec::decodeClient(std::string_view{"{}"});
    return backend.isReady() || decoded.hasValue() ? 1 : 0;
}
