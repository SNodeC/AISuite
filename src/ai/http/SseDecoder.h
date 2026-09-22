/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_AI_HTTP_SSEDECODER_H
#define AISUITE_AI_HTTP_SSEDECODER_H
#include <functional>
#include <string>
#include <string_view>

namespace ai::http {
    // Request-scoped SSE framing for POST streams (EventSource is GET/reconnecting).
    class SseDecoder {
    public:
        using Receiver = std::function<void(std::string_view, std::string_view)>;
        explicit SseDecoder(Receiver receiver, std::size_t maximumEventBytes = 1024U * 1024U);
        void consume(std::string_view bytes);
        bool empty() const noexcept;

    private:
        void line();
        Receiver receiver_;
        std::size_t limit_;
        std::string line_;
        std::string event_;
        std::string data_;
        bool afterCr_ = false;
        bool firstLine_ = true;
    };
} // namespace ai::http
#endif
