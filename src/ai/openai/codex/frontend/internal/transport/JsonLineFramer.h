/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_INTERNAL_TRANSPORT_JSONLINEFRAMER_H
#define AI_OPENAI_CODEX_FRONTEND_INTERNAL_TRANSPORT_JSONLINEFRAMER_H

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace ai::openai::codex::frontend::internal::transport {

    class JsonLineFramer {
    public:
        enum class Result { Accepted, FrameTooLarge };
        using FrameHandler = std::function<void(std::string)>;

        explicit JsonLineFramer(std::size_t maximumFrameSize);

        Result push(std::string_view bytes, const FrameHandler& onFrame);
        void clear() noexcept;

        [[nodiscard]] std::size_t bufferedSize() const noexcept;
        [[nodiscard]] std::size_t maximumFrameSize() const noexcept;

    private:
        std::size_t maximumSize;
        std::string buffered;
    };

} // namespace ai::openai::codex::frontend::internal::transport

#endif // AI_OPENAI_CODEX_FRONTEND_INTERNAL_TRANSPORT_JSONLINEFRAMER_H
