/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "TestHarness.h"
#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/model/ProviderProfile.h"
#include "ai/openai/codex/provider/StdioAppServer.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

int main(int argc, char** argv) {
    if (argc != 2)
        return 1;
    const std::string executable = argv[1];
    std::string temp = "/tmp/aisuite-catalog-XXXXXX";
    if (!::mkdtemp(temp.data()))
        return 1;
    struct Cleanup {
        std::string path;
        ~Cleanup() {
            std::filesystem::remove_all(path);
        }
    } cleanup{temp};
    ::setenv("XDG_RUNTIME_DIR", temp.c_str(), 1);
    core::SNodeC::init(1, argv);
    tests::codex::TestHarness test;
    namespace codex = ai::openai::codex;
    bool completed = false;
    bool listed = false;
    codex::bridge::CodexBridge bridge;
    codex::model::ProviderProfile profile({"anthropic", "Anthropic", {{"test-configured-model", "Configured test model"}}}, 200000);
    codex::provider::StdioAppServerOptions options;
    options.executable = executable;
    options.environment.emplace_back("CODEX_HOME", temp);
    profile.configure(options, "test-configured-model", "http://127.0.0.1:9", "unused-test-token");
    options.onExit = [&](int) {
        if (!completed)
            test.expect(false, "Codex did not exit before catalog/thread verification");
        core::SNodeC::stop();
    };
    bridge.onProviderLifecycle([&](bool connected) {
        if (!connected)
            return;
        using Init = codex::generated::client_requests::Initialize;
        bridge.initialize(
            Init::Params({{"clientInfo", {{"name", "aisuite_model_test"}, {"version", "1"}}}}), [&](Init::Response& response) {
                if (!response) {
                    test.expect(false, "Codex initialize accepted");
                    core::SNodeC::stop();
                    return;
                }
                bridge.initialized();
                bridge.setAppServerReady();
                bridge.modelList(
                    codex::generated::v2::ModelListParams({{"includeHidden", false}}), [&](codex::generated::v2::ModelListResponse& list) {
                        if (!list) {
                            test.expect(false, "Codex model/list accepted");
                            core::SNodeC::stop();
                            return;
                        }
                        for (const auto& model : list.getPayload().at("data"))
                            if (model.value("model", "") == "test-configured-model")
                                listed = true;
                        test.expect(listed, "real Codex discovers AISuite registered model without OpenAI login");
                        bridge.threadStart(codex::generated::v2::ThreadStartParams(
                                               {{"model", "test-configured-model"}, {"modelProvider", "anthropic"}, {"cwd", temp}}),
                                           [&](codex::generated::v2::ThreadStartResponse& thread) {
                                               completed = static_cast<bool>(thread);
                                               test.expect(completed && thread.getPayload().value("modelProvider", "") == "anthropic",
                                                           "real Codex creates thread with independent provider selection");
                                               if (!thread)
                                                   std::cerr << thread.jsonRpcErrorMessage().value_or("unknown thread error") << '\n';
                                               core::SNodeC::stop();
                                           });
                    });
            });
    });
    codex::provider::StdioAppServer endpoint(bridge, std::move(options));
    test.expect(endpoint.start(), "production stdio runtime started");
    const auto timeout = core::timer::Timer::singleshotTimer(
        [&] {
            test.expect(false, "catalog test timeout");
            core::SNodeC::stop();
        },
        utils::Timeval(20));
    static_cast<void>(timeout);
    core::SNodeC::start();
    endpoint.stop();
    test.expect(completed && listed, "real app-server profile verification completed");
    return test.result();
}
