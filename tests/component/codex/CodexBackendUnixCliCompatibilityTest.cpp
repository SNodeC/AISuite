/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "core/SNodeC.h"
#include "net/in/stream/legacy/config/ConfigSocketServer.h"
#include "net/in6/stream/legacy/config/ConfigSocketServer.h"
#include "net/un/stream/legacy/config/ConfigSocketServer.h"
#if defined(AISUITE_CODEX_FRONTEND_TLS)
#include "net/in/stream/tls/config/ConfigSocketServer.h"
#include "net/in6/stream/tls/config/ConfigSocketServer.h"
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
#include "net/rc/stream/legacy/config/ConfigSocketServer.h"
#include "net/rc/stream/tls/config/ConfigSocketServer.h"
#endif
#include "support/TestResult.h"
#include "utils/Config.h"

#include <array>
#include <string>
#include <unistd.h>

int main() {
    tests::support::TestResult result;
    const std::string requestedPath = "/tmp/aisuite-sun-path-compatibility-" + std::to_string(::getpid()) + ".sock";
    std::array<std::string, 5> arguments{"CodexBackendUnixCliCompatibilityTest", "codex-backend", "local", "--sun-path", requestedPath};
    std::array<char*, arguments.size()> argv{};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        argv[index] = arguments[index].data();
    }

    core::SNodeC::init(static_cast<int>(argv.size()), argv.data());
    {
        // This is the same construction order as codex-backend: the root is
        // parsed first, the listener registers its ordinary SNode.C option,
        // and bootstrap performs the authoritative final parse.
        net::un::stream::legacy::config::ConfigSocketServer configuration("codex-backend");
        configuration.Local::setSunPath("/tmp/aisuite-default-path.sock");

        net::in::stream::legacy::config::ConfigSocketServer optionalIpv4("codex-backend-ipv4");
        net::in6::stream::legacy::config::ConfigSocketServer optionalIpv6("codex-backend-ipv6");
        optionalIpv4.Instance::setDisabled(true);
        optionalIpv6.Instance::setDisabled(true);
#if defined(AISUITE_CODEX_FRONTEND_TLS)
        net::in::stream::tls::config::ConfigSocketServer optionalTlsIpv4("codex-backend-tls-ipv4");
        net::in6::stream::tls::config::ConfigSocketServer optionalTlsIpv6("codex-backend-tls-ipv6");
        optionalTlsIpv4.Instance::setDisabled(true);
        optionalTlsIpv6.Instance::setDisabled(true);
#endif
#if defined(AISUITE_CODEX_FRONTEND_RFCOMM)
        net::rc::stream::legacy::config::ConfigSocketServer optionalRfcomm("codex-backend-rfcomm");
        net::rc::stream::tls::config::ConfigSocketServer optionalRfcommTls("codex-backend-rfcomm-tls");
        optionalRfcomm.Instance::setDisabled(true);
        optionalRfcommTls.Instance::setDisabled(true);
#endif
        const bool parsed = utils::Config::bootstrap();
        result.expectTrue(
            parsed, "the default Unix-only application bootstraps while every compiled optional listener configuration remains inert");
        result.expectTrue(configuration.Local::getSunPath() == requestedPath,
                          "--sun-path overrides the application default and remains authoritative");
        result.expectTrue(optionalIpv4.Instance::getDisabled() && optionalIpv6.Instance::getDisabled(),
                          "native SNode.C instance configuration is the effective optional-listener enable authority");
    }
    core::SNodeC::free();

    return result.processResult();
}
