/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_AI_PROVIDERS_ANTHROPIC_PROVIDER_H
#define AISUITE_AI_PROVIDERS_ANTHROPIC_PROVIDER_H
#include "ai/http/StreamingClient.h"
#include "ai/model/Provider.h"

#include <map>
#include <optional>
#include <set>

namespace ai::providers::anthropic {
    struct Configuration {
        std::string apiKey;
        std::string baseUrl = "https://api.anthropic.com";
        std::vector<model::Model> models;
        std::uint64_t maximumOutputTokens = 8192;
        std::uint64_t thinkingBudgetTokens = 0;
    };
    struct PreparedRequest {
        nlohmann::json body;
        std::map<std::string, std::string> toolNames; // wire -> neutral names
    };
    PreparedRequest prepareRequest(const model::Request& request, const Configuration& configuration);

    // Wire codec is independently testable; all provider-specific state lives here.
    class MessageStream {
    public:
        MessageStream(model::Provider::Receiver receiver, std::map<std::string, std::string> toolNames, bool thinkingEnabled = false);
        void event(std::string_view name, std::string_view data);
        void finish();
        void fail(std::string code, std::string message, bool retryable = false);
        void cancel() noexcept;
        bool terminal() const noexcept;

    private:
        void parse(const nlohmann::json& event);
        void emit(const model::Event& event);
        struct Block {
            nlohmann::json value;
            std::string input;
        };
        model::Content content(const Block& block) const;
        model::Provider::Receiver receiver_;
        std::map<std::string, std::string> toolNames_;
        std::map<std::size_t, Block> blocks_;
        std::optional<model::StopReason> stop_;
        std::set<std::string> callIds_;
        model::Usage usage_;
        std::size_t nextIndex_ = 0;
        std::size_t bytes_ = 0;
        bool thinkingEnabled_;
        bool started_ = false;
        bool terminal_ = false;
    };
    class AnthropicProvider final : public model::Provider {
    public:
        AnthropicProvider(Configuration configuration, http::StreamingClient& client);
        const model::ProviderInfo& info() const noexcept override;
        std::unique_ptr<model::Operation> start(const model::Request& request, Receiver receiver) override;

    private:
        Configuration configuration_;
        http::StreamingClient& client_;
        model::ProviderInfo info_;
    };
} // namespace ai::providers::anthropic
#endif
