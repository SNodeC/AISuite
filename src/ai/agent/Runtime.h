/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_AI_AGENT_RUNTIME_H
#define AISUITE_AI_AGENT_RUNTIME_H
#include <string>
namespace ai::agent {
    // Runtime identity and model selection are independent. Empty model preserves
    // the runtime's existing default (e.g. a directly authenticated Codex session).
    struct Selection {
        std::string agent = "codex";
        std::string provider = "openai";
        std::string model;
    };
    class Runtime {
    public:
        virtual ~Runtime() = default;
        // Initiates asynchronous startup; false denotes immediate failure.
        virtual bool start() = 0;
        virtual void stop() noexcept = 0;
    };
} // namespace ai::agent
#endif
