/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/Configuration.h"
#include "core/SNodeC.h"
#include "net/un/stream/legacy/config/ConfigSocketClient.h"
#include "support/TestResult.h"
#include "utils/Config.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace {
    namespace app = apps::codex_backend_client;

    void configure(net::un::stream::legacy::config::ConfigSocketClient& configuration, const std::string& suffix, bool disabled) {
        configuration.Remote::setSunPath("/tmp/aisuite-client-transport-selection-" + std::to_string(::getpid()) + "-" + suffix + ".sock");
        configuration.Instance::setDisabled(disabled);
    }
} // namespace

int main() {
    tests::support::TestResult result;
    const std::filesystem::path policyConfig =
        std::filesystem::temp_directory_path() / ("aisuite-codex-client-policy-" + std::to_string(::getpid()) + ".conf");
    {
        std::ofstream output(policyConfig);
        output << "maximum-queued-commands=7\n"
                  "maximum-queued-command-bytes=2048\n";
    }
    app::ClientPolicyConfiguration clientPolicyConfiguration;
    std::array<std::string, 3> arguments{
        "CodexBackendClientTransportSelectionTest", "--config-file=" + policyConfig.string(), "--maximum-queued-command-bytes=4096"};
    std::array<char*, arguments.size()> argv{};
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        argv[index] = arguments[index].data();
    }

    core::SNodeC::init(static_cast<int>(argv.size()), argv.data());
    {
        net::un::stream::legacy::config::ConfigSocketClient zeroFirst("transport-selection-zero-first");
        net::un::stream::legacy::config::ConfigSocketClient zeroSecond("transport-selection-zero-second");
        configure(zeroFirst, "zero-first", true);
        configure(zeroSecond, "zero-second", true);

        net::un::stream::legacy::config::ConfigSocketClient oneFirst("transport-selection-one-first");
        net::un::stream::legacy::config::ConfigSocketClient oneSecond("transport-selection-one-second");
        configure(oneFirst, "one-first", true);
        configure(oneSecond, "one-second", false);

        net::un::stream::legacy::config::ConfigSocketClient twoFirst("transport-selection-two-first");
        net::un::stream::legacy::config::ConfigSocketClient twoSecond("transport-selection-two-second");
        configure(twoFirst, "two-first", false);
        configure(twoSecond, "two-second", false);

        result.expectTrue(utils::Config::bootstrap(), "native SNode.C named client configurations bootstrap successfully");

        const app::CommandQueueLimits queueLimits = clientPolicyConfiguration.commandQueueLimits();
        result.expectTrue(queueLimits.maximumCommands == 7 && queueLimits.maximumCommandBytes == 4096,
                          "config-file queue count and command-line byte override use the standard SNode.C configuration hierarchy");

        const std::array zeroDisabled{zeroFirst.Instance::getDisabled(), zeroSecond.Instance::getDisabled()};
        const app::OutgoingTransportPreflight zero = app::preflightOutgoingTransports(zeroDisabled);
        result.expectTrue(!zero.accepted() && zero.enabledCount == 0 && !zero.selectedIndex,
                          "preflight rejects zero effective enabled SNode.C client instances");

        const std::array oneDisabled{oneFirst.Instance::getDisabled(), oneSecond.Instance::getDisabled()};
        const app::OutgoingTransportPreflight one = app::preflightOutgoingTransports(oneDisabled);
        result.expectTrue(one.accepted() && one.enabledCount == 1 && one.selectedIndex == 1,
                          "preflight accepts and identifies exactly one effective enabled SNode.C client instance");

        const std::array twoDisabled{twoFirst.Instance::getDisabled(), twoSecond.Instance::getDisabled()};
        const app::OutgoingTransportPreflight two = app::preflightOutgoingTransports(twoDisabled);
        result.expectTrue(!two.accepted() && two.enabledCount == 2 && !two.selectedIndex,
                          "preflight rejects multiple effective enabled SNode.C client instances");
    }
    core::SNodeC::free();
    std::error_code removalError;
    std::filesystem::remove(policyConfig, removalError);

    return result.processResult();
}
