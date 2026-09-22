/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_HTTP_DETAIL_RESPONSEPARSER_H
#define AISUITE_HTTP_DETAIL_RESPONSEPARSER_H
// Private transport implementation, shared with byte-level tests. Not installed.
#include "ai/http/StreamingClient.h"
#include "core/socket/stream/SocketContext.h"
#include "web/http/ContentDecoder.h"
#include "web/http/Parser.h"
#include "web/http/http_utils.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <utility>
namespace ai::http::detail {
    struct Address {
        bool tls;
        std::string host;
        std::string authority;
        std::string path;
        unsigned short port;
    };
    struct State {
        Request request;
        Receiver receiver;
        Address address;
        core::socket::stream::SocketContext* context = nullptr;
        bool done = false;
        std::size_t received = 0;
        void fail(std::string reason) {
            if (done)
                return;
            done = true;
            auto callback = std::move(receiver.error);
            receiver = {};
            if (context)
                context->close();
            if (callback)
                callback(std::move(reason));
        }
        void complete() {
            if (done)
                return;
            done = true;
            auto callback = std::move(receiver.completed);
            receiver = {};
            if (context)
                context->close();
            if (callback)
                callback();
        }
        void data(std::string_view bytes) {
            if (done)
                return;
            if (bytes.size() > request.maximumResponseBytes - received) {
                fail("HTTP response exceeds byte limit");
                return;
            }
            received += bytes.size();
            // Copy callback before invoking: cancellation may clear receiver in it.
            const auto callback = receiver.data;
            if (callback && !bytes.empty())
                callback(bytes);
        }
    };
    // Drain SNode.C's existing transfer decoder after each read, rather than
    // reimplementing chunking or retaining a complete streaming HTTP body.
    class DrainDecoder final : public web::http::ContentDecoder {
    public:
        DrainDecoder(web::http::ContentDecoder* decoder, std::shared_ptr<State> state)
            : decoder_(decoder)
            , state_(std::move(state)) {
        }
        std::size_t read() override {
            const auto consumed = decoder_->read();
            auto bytes = decoder_->getContent();
            completed = decoder_->isComplete();
            error = decoder_->isError();
            sizeLimitExceeded = decoder_->isSizeLimitExceeded();
            state_->data(std::string_view(bytes.data(), bytes.size()));
            return consumed;
        }

    private:
        std::unique_ptr<web::http::ContentDecoder> decoder_;
        std::shared_ptr<State> state_;
    };
    // SNode.C's Identity decoder owns a pre-sized body and cannot be
    // drained mid-read. This bounded decoder tracks remaining length, not
    // the size of a body buffer; chunk framing still belongs to SNode.C.
    class LengthDecoder final : public web::http::ContentDecoder {
    public:
        LengthDecoder(core::socket::stream::SocketContext* context, std::shared_ptr<State> state, std::size_t length)
            : context_(context)
            , state_(std::move(state))
            , remaining_(length) {
        }
        std::size_t read() override {
            if (state_->done)
                return 0;
            std::array<char, 16384> bytes{};
            const auto count = context_->readFromPeer(bytes.data(), std::min(bytes.size(), remaining_));
            remaining_ -= count;
            completed = remaining_ == 0;
            state_->data(std::string_view(bytes.data(), count));
            return count;
        }

    private:
        core::socket::stream::SocketContext* context_;
        std::shared_ptr<State> state_;
        std::size_t remaining_;
    };
    class ResponseParser final : public web::http::Parser {
    public:
        ResponseParser(core::socket::stream::SocketContext* context, std::shared_ptr<State> state)
            : Parser(context, HTTPCompliance::RFC7230, {8192, 8192, 65536, 128, state->request.maximumResponseBytes})
            , state_(std::move(state)) {
        }
        void disconnected() {
            if (closeDelimited_)
                state_->complete();
            else
                state_->fail("HTTP stream disconnected before response completed");
        }

    private:
        void begin() override {
        }
        void parseStartLine(const std::string& value) override {
            if (value.size() < 12 || (!value.starts_with("HTTP/1.1 ") && !value.starts_with("HTTP/1.0 ")) ||
                (value.size() > 12 && value[12] != ' ')) {
                parseError(400, "invalid HTTP response status line");
                return;
            }
            const auto parsed = std::from_chars(value.data() + 9, value.data() + 12, status_);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + 12 || status_ < 100 || status_ > 599) {
                parseError(400, "invalid HTTP response status");
                return;
            }
            httpMajor = 1;
            httpMinor = value[7] == '1' ? 1 : 0;
            transferEncoding = httpMinor == 1 ? web::http::TransferEncoding::Identity : web::http::TransferEncoding::HTTP10;
            parserState = ParserState::HEADER;
        }
        void analyzeHeader() override {
            if (status_ < 200) {
                parseError(400, "informational HTTP responses are unsupported for provider POST");
                return;
            }
            std::map<std::string, std::string> normalized;
            for (const auto& [key, value] : headers) {
                auto lower = key;
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                normalized.emplace(std::move(lower), value);
            }
            if (normalized.contains("content-encoding") && normalized.at("content-encoding") != "identity") {
                parseError(400, "unsupported compressed provider response");
                return;
            }
            if (normalized.contains("transfer-encoding") && normalized.at("transfer-encoding") != "chunked") {
                parseError(400, "unsupported HTTP transfer encoding");
                return;
            }
            if (normalized.contains("transfer-encoding") && normalized.contains("content-length")) {
                parseError(400, "ambiguous HTTP response framing");
                return;
            }
            closeDelimited_ = !normalized.contains("transfer-encoding") && !normalized.contains("content-length");
            const auto callback = state_->receiver.headers;
            if (!state_->done && callback)
                callback(status_, normalized);
            if (state_->done) {
                parserState = ParserState::ERROR;
                return;
            }
            if (normalized.contains("content-length")) {
                if (httputils::parseContentLength(headers, contentLength) != httputils::ContentLengthParseResult::Valid) {
                    parseError(400, "invalid HTTP Content-Length");
                    return;
                }
                if (contentLength > state_->request.maximumResponseBytes) {
                    parseError(413, "HTTP response exceeds byte limit");
                    return;
                }
                transferEncoding = web::http::TransferEncoding::Identity;
                decoderQueue.push_back(new LengthDecoder(socketContext, state_, contentLength));
            } else {
                Parser::analyzeHeader();
                if (parserState == ParserState::ERROR)
                    return;
                for (auto*& decoder : decoderQueue)
                    decoder = new DrainDecoder(decoder, state_);
            }
            parserState = ParserState::BODY;
            if (normalized.contains("content-length") && contentLength == 0)
                parsingFinished();
        }
        void parseError(int, const std::string& reason) override {
            parserState = ParserState::ERROR;
            state_->fail(reason);
        }
        void parsingFinished() override {
            parserState = ParserState::ERROR;
            state_->complete();
        }
        std::shared_ptr<State> state_;
        unsigned status_ = 0;
        bool closeDelimited_ = false;
    };
} // namespace ai::http::detail
#endif
