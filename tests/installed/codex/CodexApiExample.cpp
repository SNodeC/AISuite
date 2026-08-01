/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include <ai/openai/codex/Api.h>
#include <core/SNodeC.h>

#include <variant>

int main(int argc, char* argv[]) {
    namespace codex = ai::openai::codex;
    namespace typed = codex::typed;

    core::SNodeC::init(argc, argv);
    codex::stdio::Client client;

    client.requests().setOnRequest([&](const typed::TypedServerRequest& request) {
        if (const auto* approval = std::get_if<typed::CommandApprovalRequest>(&request)) {
            (void) client.requests().respond(*approval, typed::ApprovalDecision::decline());
        } else if (const auto* approval = std::get_if<typed::FileChangeApprovalRequest>(&request)) {
            (void) client.requests().respond(*approval, typed::ApprovalDecision::decline());
        }
    });

    client.setOnStateChanged([&](const codex::StateChange& change) {
        if (change.current == codex::State::Ready) {
            client.threads().start(typed::AbsolutePath{"/synthetic/project"}, [&](const auto& thread) {
                if (!thread) {
                    client.stop();
                    return;
                }

                client.turns().start(thread->thread.id, "Hello", [&](const auto&) {
                    client.stop();
                });
            });
        } else if (change.current == codex::State::Stopped) {
            core::SNodeC::stop();
        }
    });

    client.start();
    const int result = core::SNodeC::start();
    core::SNodeC::free();
    return result;
}
