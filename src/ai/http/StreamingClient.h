/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_AI_HTTP_STREAMINGCLIENT_H
#define AISUITE_AI_HTTP_STREAMINGCLIENT_H

#include "ai/model/Provider.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace ai::http {
    struct Request {
        std::string url;
        std::map<std::string, std::string> headers;
        std::string body;
        std::size_t maximumResponseBytes = 64U * 1024U * 1024U;
    };
    // Event-loop callbacks. Header/data views are valid only during the call.
    // Callbacks may cancel their operation; no callback follows cancellation.
    struct Receiver {
        std::function<void(unsigned, const std::map<std::string, std::string>&)> headers;
        std::function<void(std::string_view)> data;
        std::function<void()> completed;
        std::function<void(std::string)> error;
    };
    class StreamingClient {
    public:
        virtual ~StreamingClient() = default;
        virtual std::unique_ptr<model::Operation> post(Request request, Receiver receiver) = 0;
    };
    // SNode.C TCP/TLS, HTTP parser and transfer decoders; no threads or retries.
    std::unique_ptr<StreamingClient> makeStreamingClient();
} // namespace ai::http
#endif
