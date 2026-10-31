/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "TestHarness.h"
#include "ai/http/StreamingClient.h"
#include "ai/http/detail/ResponseParser.h"
#include "core/SNodeC.h"
#include "core/socket/stream/SocketConnection.h"
#include "core/timer/Timer.h"
#include "net/in/SocketAddress.h"
#include "web/http/legacy/in/Server.h"
#include "web/http/server/Request.h"
#include "web/http/server/Response.h"

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
        bool sendToPeer(const char*, std::size_t) override {
            return true;
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
int main(int argc, char* argv[]) {
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
    core::SNodeC::init(argc, argv);
    {
        const std::string body(256U * 1024U, 'x');
        std::string receivedBody, responseBody, error;
        unsigned completed = 0;
        bool timedOut = false;
        auto client = ai::http::makeStreamingClient();
        std::unique_ptr<ai::model::Operation> operation;
        web::http::legacy::in::Server server("streaming-write-pressure", [&](const auto& request, const auto& response) {
            receivedBody.assign(request->body.begin(), request->body.end());
            response->set("Connection", "close").send("ok");
        });
        server.getConfig()->Instance::forceUnrequired();
        server.listen(net::in::SocketAddress("127.0.0.1", 0), [&](const auto& address, const auto& status) {
            test.expect(status == core::socket::State::OK, "streaming pressure test listens");
            if (status == core::socket::State::OK) {
                operation = client->post({"http://127.0.0.1:" + std::to_string(address.getPort()) + "/pressure", {}, body, 1024},
                                         {[&](unsigned statusCode, const auto&) {
                                              test.expect(statusCode == 200, "streaming pressure request receives HTTP 200");
                                          },
                                          [&](std::string_view data) {
                                              responseBody += data;
                                          },
                                          [&] {
                                              ++completed;
                                              core::SNodeC::stop();
                                          },
                                          [&](std::string reason) {
                                              error = std::move(reason);
                                              core::SNodeC::stop();
                                          }});
            } else {
                core::SNodeC::stop();
            }
        });
        [[maybe_unused]] const auto watchdog = core::timer::Timer::singleshotTimer(
            [&] {
                timedOut = true;
                core::SNodeC::stop();
            },
            utils::Timeval({3, 0}));
        const int result = core::SNodeC::start(utils::Timeval({1, 0}));
        test.expect(result == 0 && !timedOut && error.empty() && completed == 1, "socket pressure does not fail the accepted HTTP request");
        test.expect(receivedBody == body && responseBody == "ok", "request above the automatic watermark arrives exactly once and intact");
    }
    core::SNodeC::free();
    return test.result();
}
