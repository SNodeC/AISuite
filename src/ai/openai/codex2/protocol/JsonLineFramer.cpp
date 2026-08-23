/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex2/protocol/JsonLineFramer.h"

#include <stdexcept>
#include <utility>

namespace ai::openai::codex2::protocol {

    JsonLineFramer::JsonLineFramer(std::size_t maximumFrameBytes)
        : maximumFrameBytes_(maximumFrameBytes) {
        if (maximumFrameBytes_ == 0) {
            throw std::invalid_argument("maximum JSONL frame size must be greater than zero");
        }
    }

    bool JsonLineFramer::consume(std::string_view bytes, const MessageHandler& onMessage, const ErrorHandler& onError) {
        if (failed_) {
            return false;
        }

        buffer_.append(bytes);
        while (true) {
            const std::size_t newline = buffer_.find('\n');
            if (newline == std::string::npos) {
                if (buffer_.size() > maximumFrameBytes_) {
                    return fail("JSONL frame exceeds configured maximum before delimiter", onError);
                }
                return true;
            }
            if (newline > maximumFrameBytes_) {
                return fail("JSONL frame exceeds configured maximum", onError);
            }

            std::string line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }

            try {
                onMessage(nlohmann::json::parse(line));
            } catch (const nlohmann::json::exception& exception) {
                return fail(std::string("invalid JSONL frame: ") + exception.what(), onError);
            }
        }
    }

    void JsonLineFramer::reset() {
        buffer_.clear();
        failed_ = false;
    }

    std::size_t JsonLineFramer::maximumFrameBytes() const noexcept {
        return maximumFrameBytes_;
    }

    std::size_t JsonLineFramer::bufferedBytes() const noexcept {
        return buffer_.size();
    }

    bool JsonLineFramer::failed() const noexcept {
        return failed_;
    }

    std::string JsonLineFramer::encode(const nlohmann::json& message, std::size_t maximumFrameBytes) {
        std::string encoded = message.dump();
        if (encoded.size() > maximumFrameBytes) {
            throw std::length_error("serialized JSONL frame exceeds configured maximum");
        }
        encoded.push_back('\n');
        return encoded;
    }

    bool JsonLineFramer::fail(std::string message, const ErrorHandler& onError) {
        failed_ = true;
        buffer_.clear();
        if (onError) {
            onError(std::move(message));
        }
        return false;
    }

} // namespace ai::openai::codex2::protocol
