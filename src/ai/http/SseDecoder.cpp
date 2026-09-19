/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "ai/http/SseDecoder.h"

#include <stdexcept>
#include <utility>

namespace ai::http {
    SseDecoder::SseDecoder(Receiver receiver, std::size_t maximumEventBytes)
        : receiver_(std::move(receiver))
        , limit_(maximumEventBytes) {
        if (limit_ == 0)
            throw std::invalid_argument("SSE limit must be positive");
    }
    void SseDecoder::consume(std::string_view bytes) {
        for (char c : bytes) {
            if (afterCr_) {
                afterCr_ = false;
                if (c == '\n')
                    continue;
            }
            if (c == '\r' || c == '\n') {
                afterCr_ = c == '\r';
                line();
            } else {
                if (line_.size() + event_.size() + data_.size() >= limit_)
                    throw std::runtime_error("SSE event exceeds configured limit");
                line_ += c;
            }
        }
    }
    void SseDecoder::line() {
        std::string value = std::exchange(line_, {});
        if (firstLine_) {
            firstLine_ = false;
            if (value.starts_with("\xef\xbb\xbf"))
                value.erase(0, 3);
        }
        if (value.empty()) {
            std::string data = std::exchange(data_, {});
            std::string event = std::exchange(event_, {});
            if (!data.empty()) {
                data.pop_back();
                receiver_(event.empty() ? "message" : event, data);
            }
            return;
        }
        const auto colon = value.find(':');
        const auto field = value.substr(0, colon);
        auto body = colon == std::string::npos ? std::string{} : value.substr(colon + 1);
        if (body.starts_with(' '))
            body.erase(0, 1);
        if (field == "event")
            event_ = std::move(body);
        else if (field == "data") {
            data_ += body;
            data_ += '\n';
        }
        if (data_.size() + event_.size() > limit_)
            throw std::runtime_error("SSE event exceeds configured limit");
    }
    bool SseDecoder::empty() const noexcept {
        return line_.empty() && data_.empty() && event_.empty();
    }
} // namespace ai::http
