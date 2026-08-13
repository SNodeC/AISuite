/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/internal/transport/JsonLineFramer.h"

#include <utility>

namespace ai::openai::codex::frontend::internal::transport {

    JsonLineFramer::JsonLineFramer(const std::size_t maximumFrameSize)
        : maximumSize(maximumFrameSize) {
        buffered.reserve(maximumSize < 4096 ? maximumSize : 4096);
    }

    JsonLineFramer::Result JsonLineFramer::push(const std::string_view bytes, const FrameHandler& onFrame) {
        for (const char byte : bytes) {
            if (byte == '\n') {
                if (!buffered.empty() && buffered.back() == '\r') {
                    buffered.pop_back();
                }
                std::string frame;
                frame.swap(buffered);
                onFrame(std::move(frame));
                continue;
            }

            if (buffered.size() == maximumSize) {
                clear();
                return Result::FrameTooLarge;
            }
            buffered.push_back(byte);
        }

        return Result::Accepted;
    }

    void JsonLineFramer::clear() noexcept {
        buffered.clear();
    }

    std::size_t JsonLineFramer::bufferedSize() const noexcept {
        return buffered.size();
    }

    std::size_t JsonLineFramer::maximumFrameSize() const noexcept {
        return maximumSize;
    }

} // namespace ai::openai::codex::frontend::internal::transport
