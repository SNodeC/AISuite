/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "ai/http/StreamingClient.h"

#include "ai/http/detail/ResponseParser.h"
#include "core/socket/stream/QueueResult.h"
#include "core/socket/stream/SocketContext.h"
#include "core/socket/stream/SocketContextFactory.h"
#include "net/in/stream/legacy/SocketClient.h"
#include "net/in/stream/tls/SocketClient.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cctype>
#include <charconv>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <stdexcept>
#include <utility>

namespace ai::http {
    namespace {
        using detail::Address;
        using detail::ResponseParser;
        using detail::State;
        Address address(const std::string& url) {
            if (url.size() > 8192 || url.find('#') != std::string::npos || std::any_of(url.begin(), url.end(), [](unsigned char c) {
                    return c <= 32 || c == 127;
                }))
                throw std::invalid_argument("invalid provider URL");
            const bool tls = url.starts_with("https://");
            if (!tls && !url.starts_with("http://"))
                throw std::invalid_argument("provider URL must use HTTP or HTTPS");
            const auto start = tls ? 8U : 7U;
            const auto slash = url.find('/', start);
            const auto authority = url.substr(start, slash == std::string::npos ? slash : slash - start);
            if (authority.empty() || authority.find_first_of("@[]?") != std::string::npos)
                throw std::invalid_argument("provider URL requires a hostname or IPv4 address without userinfo");
            const auto colon = authority.find(':');
            const auto host = authority.substr(0, colon);
            unsigned port = tls ? 443U : 80U;
            if (colon != std::string::npos) {
                const auto text = authority.substr(colon + 1);
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || port == 0 || port > 65535)
                    throw std::invalid_argument("invalid provider port");
            }
            if (host.empty())
                throw std::invalid_argument("empty provider hostname");
            return {tls, host, authority, slash == std::string::npos ? "/" : url.substr(slash), static_cast<unsigned short>(port)};
        }
        class Context final : public core::socket::stream::SocketContext {
        public:
            Context(core::socket::stream::SocketConnection* connection, std::shared_ptr<State> state)
                : SocketContext(connection)
                , state_(std::move(state))
                , parser_(this, state_) {
                state_->context = this;
            }
            ~Context() override {
                state_->context = nullptr;
            }

        private:
            void onConnected() override {
                if (state_->done) {
                    close();
                    return;
                }
                std::string header =
                    "POST " + state_->address.path + " HTTP/1.1\r\nHost: " + state_->address.authority +
                    "\r\nConnection: close\r\nAccept-Encoding: identity\r\nContent-Length: " + std::to_string(state_->request.body.size()) +
                    "\r\n";
                for (const auto& [key, value] : state_->request.headers)
                    header += key + ": " + value + "\r\n";
                header += "\r\n";
                if (trySendToPeer(header) != core::socket::stream::QueueResult::Queued ||
                    trySendToPeer(state_->request.body) != core::socket::stream::QueueResult::Queued)
                    state_->fail("HTTP request write queue rejected");
                state_->request.body.clear();
                state_->request.headers.clear();
            }
            void onDisconnected() override {
                state_->context = nullptr;
                parser_.disconnected();
            }
            std::size_t onReceivedFromPeer() override {
                if (state_->done) {
                    char bytes[16384];
                    return readFromPeer(bytes, sizeof(bytes));
                }
                try {
                    return parser_.parse();
                } catch (const std::exception& e) {
                    state_->fail(e.what());
                    return 0;
                }
            }
            bool onSignal(int) override {
                return true;
            }
            void onReadError(int error) override {
                if (error == 0)
                    parser_.disconnected();
                else
                    state_->fail("HTTP transport read failed");
            }
            void onWriteError(int) override {
                state_->fail("HTTP transport write failed");
            }
            std::shared_ptr<State> state_;
            ResponseParser parser_;
        };
        class Factory final : public core::socket::stream::SocketContextFactory {
        public:
            explicit Factory(std::shared_ptr<State> state)
                : state_(std::move(state)) {
            }
            core::socket::stream::SocketContext* create(core::socket::stream::SocketConnection* connection) override {
                return new Context(connection, state_);
            }

        private:
            std::shared_ptr<State> state_;
        };
        template <typename Client>
        class Operation final : public model::Operation {
        public:
            explicit Operation(std::shared_ptr<State> state)
                : state_(std::move(state)) {
                Client client("", std::shared_ptr<State>(state_));
                auto* config = client.getConfig();
                config->Remote::setHost(state_->address.host)->setPort(state_->address.port);
                config->setRetry(false);
                config->setReconnect(false);
                config->Connection::setReadTimeout(utils::Timeval(120));
                config->Connection::setWriteTimeout(utils::Timeval(30));
                config->Connection::setMaximumWriteQueueBytes(64U * 1024U * 1024U + 65536U);
                if constexpr (requires { config->setSni(std::string{}); }) {
                    config->setSni(state_->address.host);
                    config->setCaCertDirUseDefault(true);
                    config->setCaCertAcceptUnknown(false);
                    // SNode.C verifies the certificate chain; check the peer identity
                    // before the HTTP context can send any credentials.
                    client.setOnConnected([state = state_](auto* connection) {
                        SSL* ssl = connection->getSSL();
                        X509* certificate = ssl ? SSL_get1_peer_certificate(ssl) : nullptr;
                        in_addr ip{};
                        const int matched = certificate == nullptr ? 0
                                            : ::inet_pton(AF_INET, state->address.host.c_str(), &ip) == 1
                                                ? X509_check_ip_asc(certificate, state->address.host.c_str(), 0)
                                                : X509_check_host(certificate,
                                                                  state->address.host.c_str(),
                                                                  state->address.host.size(),
                                                                  X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS,
                                                                  nullptr);
                        if (certificate)
                            X509_free(certificate);
                        if (!ssl || SSL_get_verify_result(ssl) != X509_V_OK || matched != 1) {
                            state->fail("provider TLS certificate identity verification failed");
                            connection->close();
                        }
                    });
                }
                client.setOnDisconnect([state = state_](auto*) {
                    state->fail("provider transport disconnected");
                });
                flow_ = client.connect([state = state_](const auto&, const core::socket::State& status) {
                    if (status != core::socket::State::OK)
                        state->fail("provider connection failed: " + status.what());
                });
            }
            ~Operation() override {
                cancel();
            }
            void cancel() noexcept override {
                state_->done = true;
                state_->receiver = {};
                if (state_->context)
                    state_->context->close();
                static_cast<void>(flow_->terminateFlow());
            }

        private:
            std::shared_ptr<State> state_;
            typename Client::FlowHandle flow_;
        };
        class Client final : public StreamingClient {
        public:
            std::unique_ptr<model::Operation> post(Request request, Receiver receiver) override {
                auto state = std::make_shared<State>();
                state->receiver = std::move(receiver);
                try {
                    state->address = address(request.url);
                    if (request.body.size() > 64U * 1024U * 1024U || request.maximumResponseBytes == 0)
                        throw std::invalid_argument("invalid HTTP request limits");
                    std::size_t headerBytes = 0;
                    for (const auto& [key, value] : request.headers) {
                        if (key.empty() ||
                            key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") !=
                                std::string::npos ||
                            std::any_of(value.begin(), value.end(), [](unsigned char c) {
                                return (c < 32 && c != '\t') || c == 127;
                            }))
                            throw std::invalid_argument("invalid HTTP request header");
                        auto lower = key;
                        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                            return static_cast<char>(std::tolower(c));
                        });
                        if (lower == "host" || lower == "connection" || lower == "content-length" || lower == "transfer-encoding" ||
                            lower == "accept-encoding")
                            throw std::invalid_argument("transport-owned HTTP request header");
                        headerBytes += key.size() + value.size() + 4;
                    }
                    if (headerBytes > 32768)
                        throw std::invalid_argument("HTTP request headers exceed limit");
                    state->request = std::move(request);
                    if (state->address.tls)
                        return std::make_unique<Operation<net::in::stream::tls::SocketClient<Factory, std::shared_ptr<State>>>>(state);
                    return std::make_unique<Operation<net::in::stream::legacy::SocketClient<Factory, std::shared_ptr<State>>>>(state);
                } catch (const std::exception& e) {
                    state->fail(e.what());
                    return {};
                }
            }
        };
    } // namespace
    std::unique_ptr<StreamingClient> makeStreamingClient() {
        return std::make_unique<Client>();
    }
} // namespace ai::http
