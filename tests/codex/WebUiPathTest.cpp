/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */

#include "TestHarness.h"
#include "apps/codex-bridge/WebSocketApplication.h"

#include <optional>
#include <string>

int main() {
    tests::codex::TestHarness test;
    const std::string root = "/tmp/codex-webui-test-root";
    test.expect(apps::codex_bridge::resolveWebUiPath(root, "/") ==
                    std::optional<std::string>{root + "/index.html"},
                "WebUI root resolves to index.html");
    test.expect(apps::codex_bridge::resolveWebUiPath(root, "/assets/app.js") ==
                    std::optional<std::string>{root + "/assets/app.js"},
                "WebUI assets remain confined below the configured root");
    test.expect(!apps::codex_bridge::resolveWebUiPath(root, "/../secret") &&
                    !apps::codex_bridge::resolveWebUiPath(root, "/%2e%2e/secret") &&
                    !apps::codex_bridge::resolveWebUiPath(root, "/assets\\secret") &&
                    !apps::codex_bridge::resolveWebUiPath({}, "/index.html"),
                "WebUI traversal, platform separators, and disabled roots are rejected");
    return test.result();
}
