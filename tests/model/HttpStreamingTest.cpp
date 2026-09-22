/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "TestHarness.h"
#include "ai/http/detail/ResponseParser.h"
#include "core/socket/stream/SocketConnection.h"
#include "net/in/SocketAddress.h"

#include <cstring>

namespace {
    // A byte source at SNode.C's existing SocketConnection boundary, not a
    // replacement HTTP parser. No sockets, private production paths or timers.
    class Connection final : public core::socket::stream::SocketConnection {
    public:
        Connection()
            : SocketConnection(-1, 1, "http-test", nullptr) {
        }
        ~Connection() override = default;
        std::string pending;
        bool closed = false;
        int getFd() const override {
            return -1;
        }
        void sendToPeer(const char*, std::size_t) override {
        }
        bool streamToPeer(core::pipe::Source*) override {
            return false;
        }
        void streamEof() override {
        }
        std::size_t readFromPeer(char* bytes, std::size_t length) override {
            const auto count = std::min(length, pending.size());
            std::memcpy(bytes, pending.data(), count);
            pending.erase(0, count);
            return count;
        }
        void shutdownRead() override {
        }
        void shutdownWrite() override {
        }
        void close() override {
            closed = true;
        }
        void setTimeout(const utils::Timeval&) override {
        }
        void setReadTimeout(const utils::Timeval&) override {
        }
        void setWriteTimeout(const utils::Timeval&) override {
        }
        std::size_t getTotalSent() const override {
            return 0;
        }
        std::size_t getTotalQueued() const override {
            return 0;
        }
        std::size_t getTotalRead() const override {
            return 0;
        }
        std::size_t getTotalProcessed() const override {
            return 0;
        }
        const core::socket::SocketAddress& getBindAddress() const override {
            return address;
        }
        const core::socket::SocketAddress& getLocalAddress() const override {
            return address;
        }
        const core::socket::SocketAddress& getRemoteAddress() const override {
            return address;
        }
        net::in::SocketAddress address;
    };
    class Context final : public core::socket::stream::SocketContext {
    public:
        explicit Context(Connection& connection)
            : SocketContext(&connection) {
        }
        ~Context() override = default;

    private:
        void onConnected() override {
        }
        void onDisconnected() override {
        }
        std::size_t onReceivedFromPeer() override {
            return 0;
        }
        bool onSignal(int) override {
            return true;
        }
    };
    struct Fixture {
        Connection connection;
        Context context{connection};
        std::string output, error;
        unsigned completed = 0;
        unsigned status = 0;
        std::shared_ptr<ai::http::detail::State> state = std::make_shared<ai::http::detail::State>();
        ai::http::detail::ResponseParser parser{&context, state};
        Fixture() {
            state->context = &context;
            state->receiver = {[&](unsigned value, const auto&) {
                                   status = value;
                               },
                               [&](std::string_view data) {
                                   output += data;
                               },
                               [&] {
                                   ++completed;
                               },
                               [&](std::string value) {
                                   error = std::move(value);
                               }};
        }
        void feed(std::string_view bytes) {
            for (char byte : bytes) {
                connection.pending += byte;
                parser.parse();
            }
        }
    };
} // namespace
int main() {
    tests::codex::TestHarness test;
    {
        Fixture f;
        f.feed("HTTP/1.1 200 OK\r\nContent-Length: 11\r\nContent-Type: text/event-stream\r\n\r\nhello");
        test.expect(f.output == "hello" && !f.completed && f.error.empty(),
                    "Content-Length delivers only received bytes before completion");
        f.feed(" world");
        f.parser.disconnected();
        test.expect(f.output == "hello world" && f.completed == 1 && f.error.empty(),
                    "fragmented fixed-length body completes once without invalidating storage");
    }
    {
        Fixture f;
        f.feed("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTrailer: X-End\r\n\r\n5\r\nhello\r\n");
        test.expect(f.output == "hello" && !f.completed && f.error.empty(), "SNode.C chunk decoder delivers before final chunk");
        f.feed("6\r\n world\r\n0\r\nX-End: done\r\n\r\n");
        test.expect(f.completed == 1 && f.output == "hello world" && f.error.empty(), "chunked response and trailers complete");
    }
    {
        Fixture f;
        f.feed("HTTP/1.0 429 Too Many Requests\r\n\r\nerror body");
        f.parser.disconnected();
        test.expect(f.status == 429 && f.output == "error body" && f.completed == 1 && f.error.empty(),
                    "close-delimited error body completes on EOF");
    }
    for (const auto& bytes : {"HTTP/1.1 200 OK\r\nContent-Length: 12\r\n\r\nshort",
                              "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nshort\r\n",
                              "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n",
                              "HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\n\r\n",
                              "HTTP/1.1 200 OK\r\nContent-Length: -1\r\n\r\n",
                              "HTTP/1.1 200 OK\r\nContent-Length: 999999999999\r\n\r\n"}) {
        Fixture f;
        f.feed(bytes);
        f.parser.disconnected();
        test.expect(!f.error.empty() && !f.completed, "truncated, ambiguous, oversized and compressed responses fail");
    }
    {
        Fixture f;
        f.feed("HTTP/1.1 204 No Content\r\nContent-Length: 0\r\n\r\n");
        test.expect(f.completed == 1 && f.output.empty(), "zero-length response completes immediately");
    }
    return test.result();
}
