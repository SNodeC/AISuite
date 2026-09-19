/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#include "ai/openai/codex/model/ResponsesService.h"

#include "ai/openai/codex/model/ResponsesAdapter.h"
#include "core/pipe/Source.h"
#include "net/in/stream/legacy/SocketServer.h"
#include "web/http/ConfigHttpParser.h"
#include "web/http/server/Request.h"
#include "web/http/server/Response.h"
#include "web/http/server/Server.h"
#include "web/http/server/SocketContext.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <openssl/crypto.h>
#include <openssl/rand.h>
#include <stdexcept>

namespace ai::openai::codex::model {
    namespace {
        using Response = web::http::server::Response;
        using Request = web::http::server::Request;
        class Session final : public core::pipe::Source {
        public:
            bool isOpen() override {
                return open_;
            }
            void start() override {
            }
            void suspend() override {
                error(ENOBUFS);
                stop();
            }
            void resume() override {
            }
            void stop() override {
                open_ = false;
                if (operation)
                    operation->cancel();
            }
            bool event(const nlohmann::json& value) {
                if (!open_)
                    return false;
                const std::string frame = "event: " + value.at("type").get<std::string>() + "\ndata: " + value.dump() + "\n\n";
                return send(frame.data(), frame.size()) >= 0 && open_;
            }
            void finish() {
                if (open_)
                    eof();
            }
            std::unique_ptr<ai::model::Operation> operation;

        private:
            bool open_ = true;
        };
        struct State {
            ai::model::Provider* provider;
            std::string token;
            std::string baseUrl;
            std::vector<std::weak_ptr<Session>> sessions;
            std::uint64_t sequence = 0;
            explicit State(ai::model::Provider& value)
                : provider(&value) {
                std::array<unsigned char, 32> bytes{};
                if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
                    throw std::runtime_error("cannot generate model endpoint credential");
                constexpr char hex[] = "0123456789abcdef";
                for (auto b : bytes) {
                    token += hex[b >> 4];
                    token += hex[b & 15];
                }
            }
            void handle(const std::shared_ptr<Request>& request, const std::shared_ptr<Response>& response) {
                response->set("Connection", "close");
                auto reject = [&](int status, const std::string& message) {
                    response->status(status).type("application/json");
                    response->send(nlohmann::json{{"error", {{"type", "invalid_request_error"}, {"message", message}}}}.dump());
                };
                const auto authorization = request->get("Authorization");
                const auto expected = "Bearer " + token;
                if (authorization.size() != expected.size() || CRYPTO_memcmp(authorization.data(), expected.data(), expected.size()) != 0) {
                    reject(401, "invalid local model endpoint credential");
                    return;
                }
                if (!provider) {
                    reject(503, "model service is stopping");
                    return;
                }
                if (request->url != "/responses" || !request->queries.empty()) {
                    reject(404, "only POST /responses is supported");
                    return;
                }
                if (request->method != "POST") {
                    reject(405, "only POST /responses is supported");
                    return;
                }
                if (!request->get("Content-Encoding").empty() && request->get("Content-Encoding") != "identity") {
                    reject(415, "compressed Responses requests are unsupported");
                    return;
                }
                if (!request->get("Content-Type").starts_with("application/json")) {
                    reject(415, "application/json is required");
                    return;
                }
                std::erase_if(sessions, [](const auto& value) {
                    auto session = value.lock();
                    return !session || !session->isOpen();
                });
                if (sessions.size() >= 16) {
                    reject(429, "too many concurrent model requests");
                    return;
                }
                ResponsesRequest parsed;
                try {
                    parsed = parseResponsesRequest(nlohmann::json::parse(request->body));
                    const auto& models = provider->info().models;
                    if (std::none_of(models.begin(), models.end(), [&](const auto& model) {
                            return model.id == parsed.request.model;
                        }))
                        throw std::invalid_argument("model is not registered with the selected AISuite provider");
                } catch (const std::exception& e) {
                    reject(400, e.what());
                    return;
                }
                auto session = std::make_shared<Session>();
                sessions.emplace_back(session);
                response->getSocketContext()->setOnDisconnected([session] {
                    session->stop();
                });
                response->type("text/event-stream").set("Cache-Control", "no-cache");
                // pipe() owns HTTP headers, chunk framing, bounded writer admission,
                // backpressure and EOF. Do not call Response::end after sendHeader.
                if (!response->pipe(session.get())) {
                    session->stop();
                    return;
                }
                const auto weak = std::weak_ptr<Session>(session);
                session->operation = startResponse(
                    *provider,
                    std::move(parsed),
                    "resp_aisuite_" + std::to_string(++sequence),
                    [weak](const auto& event) {
                        auto session = weak.lock();
                        return session && session->event(event);
                    },
                    [weak] {
                        if (auto session = weak.lock())
                            session->finish();
                    });
                if (!session->isOpen() && session->operation)
                    session->operation->cancel();
            }
        };
    } // namespace
    class ResponsesService::Impl {
    public:
        explicit Impl(ai::model::Provider& provider)
            : state(std::make_shared<State>(provider))
            , server("codex-model-responses", [state = state](const auto& request, const auto& response) {
                state->handle(request, response);
            }) {
            server.getConfig()->Local::setHost("127.0.0.1")->setPort(0);
            server.getConfig()->Connection::setMaximumWriteQueueBytes(8U * 1024U * 1024U);
            server.getConfig()->Connection::setReadTimeout(utils::Timeval(120));
            server.getConfig()->Connection::setWriteTimeout(utils::Timeval(120));
            auto* http = server.getConfig()->net::config::ConfigInstance::getSubCommand<web::http::server::ConfigHttpServer>();
            http->setMaximumPendingRequests(1)->setAllowPipelining(false)->setAllowChunkedTransfer(false);
            http->getParserConfig()
                ->setMaximumStartLineBytes(8192)
                ->setMaximumHeaderLineBytes(8192)
                ->setMaximumHeaderBytes(65536)
                ->setMaximumHeaderFields(128)
                ->setMaximumBodyBytes(64U * 1024U * 1024U);
        }
        std::shared_ptr<State> state;
        web::http::server::Server<net::in::stream::legacy::SocketServer> server;
    };
    ResponsesService::ResponsesService(ai::model::Provider& provider)
        : impl_(std::make_unique<Impl>(provider)) {
    }
    ResponsesService::~ResponsesService() {
        stop();
    }
    void ResponsesService::listen(std::function<void(std::string)> ready) {
        if (impl_->server.getConfig()->Local::getHost() != "127.0.0.1") {
            ready("model endpoint must bind IPv4 loopback");
            return;
        }
        impl_->server.listen(
            [state = impl_->state, ready = std::move(ready)](const net::in::SocketAddress& address, const core::socket::State& result) {
                if (!state->provider)
                    return;
                if (result != core::socket::State::OK) {
                    ready("model endpoint listen failed: " + result.what());
                    return;
                }
                state->baseUrl = "http://127.0.0.1:" + std::to_string(address.getPort());
                ready({});
            });
    }
    void ResponsesService::stop() noexcept {
        impl_->state->provider = nullptr;
        static_cast<void>(impl_->server.getFlowController()->terminateFlow());
        for (const auto& weak : impl_->state->sessions)
            if (auto session = weak.lock())
                session->stop();
        impl_->state->sessions.clear();
    }
    const std::string& ResponsesService::baseUrl() const noexcept {
        return impl_->state->baseUrl;
    }
    const std::string& ResponsesService::bearerToken() const noexcept {
        return impl_->state->token;
    }
} // namespace ai::openai::codex::model
