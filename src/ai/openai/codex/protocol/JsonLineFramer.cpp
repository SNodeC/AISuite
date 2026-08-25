/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/protocol/JsonLineFramer.h"

#include <stdexcept>
#include <utility>

namespace ai::openai::codex::protocol {

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

        compact();
        buffer_.append(bytes);
        while (true) {
            const std::size_t newline = buffer_.find('\n', bufferOffset_);
            if (newline == std::string::npos) {
                if (bufferedBytes() > maximumFrameBytes_) {
                    return fail("JSONL frame exceeds configured maximum before delimiter", onError);
                }
                compact();
                return true;
            }
            const std::size_t lineBytes = newline - bufferOffset_;
            if (lineBytes > maximumFrameBytes_) {
                return fail("JSONL frame exceeds configured maximum", onError);
            }

            std::string line = buffer_.substr(bufferOffset_, lineBytes);
            bufferOffset_ = newline + 1;
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.empty()) {
                continue;
            }

            nlohmann::json message;
            try {
                message = nlohmann::json::parse(line);
            } catch (const nlohmann::json::exception& exception) {
                return fail(std::string("invalid JSONL frame: ") + exception.what(), onError);
            }
            // Application dispatch is deliberately outside the parse-error
            // boundary. Its exceptions belong to the caller, not the byte
            // stream, and must not poison this framer.
            onMessage(std::move(message));
        }
    }

    void JsonLineFramer::reset() {
        buffer_.clear();
        bufferOffset_ = 0;
        failed_ = false;
    }

    std::size_t JsonLineFramer::maximumFrameBytes() const noexcept {
        return maximumFrameBytes_;
    }

    std::size_t JsonLineFramer::bufferedBytes() const noexcept {
        return buffer_.size() - bufferOffset_;
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
        bufferOffset_ = 0;
        if (onError) {
            onError(std::move(message));
        }
        return false;
    }

    void JsonLineFramer::compact() {
        if (bufferOffset_ == 0) {
            return;
        }
        if (bufferOffset_ == buffer_.size()) {
            buffer_.clear();
            bufferOffset_ = 0;
        } else if (bufferOffset_ >= buffer_.size() / 2) {
            buffer_.erase(0, bufferOffset_);
            bufferOffset_ = 0;
        }
    }

} // namespace ai::openai::codex::protocol
