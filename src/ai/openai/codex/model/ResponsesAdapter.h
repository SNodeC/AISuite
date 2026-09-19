/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_CODEX_MODEL_RESPONSESADAPTER_H
#define AISUITE_CODEX_MODEL_RESPONSESADAPTER_H
#include "ai/model/Provider.h"

#include <map>
#include <set>

namespace ai::openai::codex::model {
    struct ToolIdentity {
        std::string name;
        std::string nameSpace;
        bool custom = false;
    };
    struct ResponsesRequest {
        ai::model::Request request;
        std::map<std::string, ToolIdentity> tools;
    };
    ResponsesRequest parseResponsesRequest(const nlohmann::json& request);

    class ResponsesStream {
    public:
        using Sender = std::function<bool(const nlohmann::json&)>;
        ResponsesStream(const ResponsesRequest& request, std::string responseId, Sender sender);
        void receive(const ai::model::Event& event);
        bool terminal() const noexcept;

    private:
        void send(nlohmann::json event);
        void fail(const ai::model::Error& error);
        void finishText(bool final);
        nlohmann::json item(std::size_t index, const ai::model::Content& content, bool completed) const;
        std::string itemId(std::size_t index) const;
        // Decode the one string field used for freeform tools incrementally. JSON
        // escape/surrogate decoding is delegated to nlohmann, not hand-translated.
        struct CustomInput {
            enum class State { Open, Key, Colon, Quote, Value, Close, Done } state = State::Open;
            std::string key;
            std::string escape;
            std::string consume(std::string_view delta);
        };
        struct Active {
            ai::model::Content content;
            CustomInput custom;
            std::string deltas;
        };
        std::string model_;
        std::map<std::string, ToolIdentity> tools_;
        std::string id_;
        Sender sender_;
        ai::model::Usage usage_;
        std::map<std::size_t, Active> active_;
        nlohmann::json output_ = nlohmann::json::array();
        std::set<std::string> calls_;
        std::uint64_t sequence_ = 0;
        std::size_t bytes_ = 0;
        bool started_ = false;
        bool terminal_ = false;
    };

    // Shared by the HTTP service and deterministic protocol tests. Request scoped;
    // disconnect cancels the provider operation, with no conversation cache.
    std::unique_ptr<ai::model::Operation> startResponse(ai::model::Provider& provider,
                                                        ResponsesRequest request,
                                                        std::string responseId,
                                                        ResponsesStream::Sender sender,
                                                        std::function<void()> finished);
} // namespace ai::openai::codex::model
#endif
