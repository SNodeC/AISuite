/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX2_PROTOCOL_JSONLINEFRAMER_H
#define AI_OPENAI_CODEX2_PROTOCOL_JSONLINEFRAMER_H

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace ai::openai::codex2::protocol {

    class JsonLineFramer {
    public:
        using MessageHandler = std::function<void(nlohmann::json)>;
        using ErrorHandler = std::function<void(std::string)>;

        explicit JsonLineFramer(std::size_t maximumFrameBytes);

        bool consume(std::string_view bytes, const MessageHandler& onMessage, const ErrorHandler& onError);
        void reset();

        std::size_t maximumFrameBytes() const noexcept;
        std::size_t bufferedBytes() const noexcept;
        bool failed() const noexcept;

        static std::string encode(const nlohmann::json& message, std::size_t maximumFrameBytes);

    private:
        bool fail(std::string message, const ErrorHandler& onError);

        std::size_t maximumFrameBytes_;
        std::string buffer_;
        bool failed_ = false;
    };

} // namespace ai::openai::codex2::protocol

#endif
