/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_CODEX_MODEL_RESPONSESSERVICE_H
#define AISUITE_CODEX_MODEL_RESPONSESSERVICE_H
#include "ai/model/Provider.h"

#include <functional>
#include <memory>
#include <string>
namespace ai::openai::codex::model {
    class ResponsesService {
    public:
        explicit ResponsesService(ai::model::Provider& provider);
        ~ResponsesService();
        ResponsesService(const ResponsesService&) = delete;
        ResponsesService& operator=(const ResponsesService&) = delete;
        // Empty error on success. The listener is bound before Codex is started.
        void listen(std::function<void(std::string error)> ready);
        void stop() noexcept;
        const std::string& baseUrl() const noexcept;
        const std::string& bearerToken() const noexcept;

    private:
        class Impl;
        std::unique_ptr<Impl> impl_;
    };
} // namespace ai::openai::codex::model
#endif
