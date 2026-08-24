/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/provider/StdioAppServer.h"

#include "ai/openai/codex/bridge/CodexBridge.h"
#include "ai/openai/codex/provider/ChildExitObserver.h"
#include "core/pipe/Pipe.h"
#include "core/pipe/PipeSink.h"
#include "core/pipe/PipeSource.h"
#include "utils/Timeval.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <spawn.h>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace ai::openai::codex::provider {

    namespace {
        std::vector<char*> pointers(std::vector<std::string>& values) {
            std::vector<char*> result;
            result.reserve(values.size() + 1);
            for (std::string& value : values) {
                result.push_back(value.data());
            }
            result.push_back(nullptr);
            return result;
        }

        std::vector<std::string> childEnvironment(const std::vector<std::pair<std::string, std::string>>& overrides) {
            std::vector<std::string> result;
            for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
                result.emplace_back(*entry);
            }
            for (const auto& [name, value] : overrides) {
                const std::string prefix = name + '=';
                std::erase_if(result, [&prefix](const std::string& entry) {
                    return entry.starts_with(prefix);
                });
                result.push_back(prefix + value);
            }
            return result;
        }

    } // namespace

    StdioAppServer::StdioAppServer(bridge::CodexBridge& bridge, StdioAppServerOptions options)
        : bridge_(bridge)
        , options_(std::move(options))
        , stdoutFramer_(options_.maximumFrameBytes) {
        bridge_.setAppServer(this);
    }

    StdioAppServer::~StdioAppServer() {
        stop();
        detachCallbacks();
        bridge_.setAppServer(nullptr);
    }

    bool StdioAppServer::start() {
        if (connected_ || childPid_ > 0) {
            return false;
        }
        stopping_ = false;
        stdoutFramer_.reset();

        core::pipe::Pipe stdinPipe(O_CLOEXEC, "codex-appserver-stdin");
        core::pipe::Pipe stdoutPipe(O_CLOEXEC, "codex-appserver-stdout");
        core::pipe::Pipe stderrPipe(O_CLOEXEC, "codex-appserver-stderr");
        if (!stdinPipe.hasReadFd() || !stdinPipe.hasWriteFd() || !stdoutPipe.hasReadFd() || !stdoutPipe.hasWriteFd() ||
            !stderrPipe.hasReadFd() || !stderrPipe.hasWriteFd()) {
            std::cerr << "codex-bridge: unable to create app-server pipes\n";
            return false;
        }

        posix_spawn_file_actions_t actions{};
        if (posix_spawn_file_actions_init(&actions) != 0) {
            return false;
        }
        int setupError = 0;
        const auto addAction = [&setupError](int result) {
            if (setupError == 0 && result != 0) {
                setupError = result;
            }
        };
        addAction(posix_spawn_file_actions_adddup2(&actions, stdinPipe.getReadFd(), STDIN_FILENO));
        addAction(posix_spawn_file_actions_adddup2(&actions, stdoutPipe.getWriteFd(), STDOUT_FILENO));
        addAction(posix_spawn_file_actions_adddup2(&actions, stderrPipe.getWriteFd(), STDERR_FILENO));
        for (const int descriptor : {stdinPipe.getReadFd(),
                                     stdinPipe.getWriteFd(),
                                     stdoutPipe.getReadFd(),
                                     stdoutPipe.getWriteFd(),
                                     stderrPipe.getReadFd(),
                                     stderrPipe.getWriteFd()}) {
            if (descriptor > STDERR_FILENO) {
                addAction(posix_spawn_file_actions_addclose(&actions, descriptor));
            }
        }

        posix_spawnattr_t attributes{};
        const int attributeResult = posix_spawnattr_init(&attributes);
        if (setupError == 0 && attributeResult != 0) {
            setupError = attributeResult;
        }
        if (attributeResult == 0) {
            addAction(posix_spawnattr_setpgroup(&attributes, 0));
            addAction(posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP));
        }

        std::vector<std::string> arguments;
        arguments.reserve(options_.arguments.size() + 1);
        arguments.push_back(options_.executable);
        arguments.insert(arguments.end(), options_.arguments.begin(), options_.arguments.end());
        std::vector<char*> argumentPointers = pointers(arguments);
        std::vector<std::string> environment = childEnvironment(options_.environment);
        std::vector<char*> environmentPointers = pointers(environment);

        pid_t spawnedPid = -1;
        if (setupError == 0) {
            setupError = posix_spawnp(&spawnedPid,
                                      options_.executable.c_str(),
                                      &actions,
                                      attributeResult == 0 ? &attributes : nullptr,
                                      argumentPointers.data(),
                                      environmentPointers.data());
        }
        if (attributeResult == 0) {
            static_cast<void>(posix_spawnattr_destroy(&attributes));
        }
        static_cast<void>(posix_spawn_file_actions_destroy(&actions));
        if (setupError != 0) {
            std::cerr << "codex-bridge: unable to spawn app-server: " << std::strerror(setupError) << '\n';
            return false;
        }

        childPid_ = spawnedPid;
        stdinPipe.closeRead();
        stdoutPipe.closeWrite();
        stderrPipe.closeWrite();

        stdinWriter_ = stdinPipe.releaseWriteAsSource(options_.maximumQueuedInputBytes, utils::Timeval({0, 0}));
        stdoutReader_ = stdoutPipe.releaseReadAsSink(core::pipe::PipeSink::DEFAULT_MAX_BYTES_PER_EVENT, utils::Timeval({0, 0}));
        stderrReader_ = stderrPipe.releaseReadAsSink(core::pipe::PipeSink::DEFAULT_MAX_BYTES_PER_EVENT, utils::Timeval({0, 0}));
        if (stdinWriter_ == nullptr || stdoutReader_ == nullptr || stderrReader_ == nullptr) {
            std::cerr << "codex-bridge: unable to attach app-server pipe receivers\n";
            stop();
            return false;
        }

        stdinWriter_->setOnError([this](int error) {
            transportClosed(std::string("stdin error: ") + std::strerror(error));
        });
        stdinWriter_->setOnClosed([this]() {
            stdinWriter_ = nullptr;
        });
        stdoutReader_->setOnData([this](const char* data, std::size_t length) {
            receiveStdout(data, length);
        });
        stdoutReader_->setOnEof([this]() {
            transportClosed("app-server stdout EOF");
        });
        stdoutReader_->setOnError([this](int error) {
            transportClosed(std::string("stdout error: ") + std::strerror(error));
        });
        stdoutReader_->setOnClosed([this]() {
            stdoutReader_ = nullptr;
        });
        stderrReader_->setOnData([this](const char* data, std::size_t length) {
            receiveStderr(data, length);
        });
        stderrReader_->setOnClosed([this]() {
            stderrReader_ = nullptr;
        });

        connected_ = true;
        if (!observeChild()) {
            std::cerr << "codex-bridge: unable to observe app-server process with pidfd\n";
            stop();
            return false;
        }
        std::clog << "codex-bridge: app-server spawned pid=" << childPid_ << '\n';
        bridge_.appServerConnected();
        return true;
    }

    void StdioAppServer::stop() {
        if (stopping_) {
            return;
        }
        stopping_ = true;
        detachChildObserver();
        const bool wasConnected = connected_;
        connected_ = false;
        if (stdinWriter_ != nullptr) {
            stdinWriter_->eof();
        }
        if (childPid_ > 0) {
            ChildExitObserver::terminateAndReap(childPid_);
            childPid_ = -1;
        }
        if (wasConnected) {
            bridge_.appServerDisconnected("app-server stopped");
        }
    }

    bool StdioAppServer::send(const nlohmann::json& message) {
        if (!connected_ || stdinWriter_ == nullptr) {
            return false;
        }
        try {
            const std::string frame = protocol::JsonLineFramer::encode(message, options_.maximumFrameBytes);
            if (!stdinWriter_->send(frame)) {
                std::clog << "codex-bridge: app-server stdin rejected frame bytes=" << frame.size()
                          << " queued=" << stdinWriter_->getQueuedBytes() << '\n';
                return false;
            }
            return true;
        } catch (const std::exception& exception) {
            std::clog << "codex-bridge: app-server frame rejected reason=" << exception.what() << '\n';
            return false;
        }
    }

    bool StdioAppServer::isConnected() const noexcept {
        return connected_;
    }

    pid_t StdioAppServer::pid() const noexcept {
        return childPid_;
    }

    void StdioAppServer::receiveStdout(const char* data, std::size_t length) {
        const bool accepted = stdoutFramer_.consume(
            std::string_view(data, length),
            [this](nlohmann::json message) {
                bridge_.receiveFromAppServer(message);
            },
            [](std::string error) {
                std::clog << "codex-bridge: invalid app-server frame reason=" << error << '\n';
            });
        if (!accepted) {
            transportClosed("invalid or oversized app-server JSONL frame");
        }
    }

    void StdioAppServer::receiveStderr(const char* data, std::size_t length) {
        stderrBuffer_.append(data, length);
        while (true) {
            const std::size_t newline = stderrBuffer_.find('\n');
            if (newline == std::string::npos) {
                if (stderrBuffer_.size() > 64 * 1024) {
                    stderrBuffer_.erase(0, stderrBuffer_.size() - 64 * 1024);
                }
                return;
            }
            std::clog << "codex-app-server: " << stderrBuffer_.substr(0, newline) << '\n';
            stderrBuffer_.erase(0, newline + 1);
        }
    }

    void StdioAppServer::transportClosed(std::string reason) {
        if (!connected_) {
            return;
        }
        connected_ = false;
        std::clog << "codex-bridge: app-server transport closed reason=" << reason << '\n';
        bridge_.appServerDisconnected(reason);
    }

    bool StdioAppServer::observeChild() {
        childObserver_ = ChildExitObserver::observe(
            childPid_,
            [this](ChildExitObserver* observer) {
                childExitReady(observer);
            },
            [this](ChildExitObserver* observer) {
                childObserverClosed(observer);
            });
        if (childObserver_ != nullptr) {
            std::clog << "codex-bridge: observing app-server process with pidfd\n";
            return true;
        }
        return false;
    }

    void StdioAppServer::childExitReady(ChildExitObserver* observer) {
        if (childObserver_ != observer) {
            return;
        }
        childObserver_ = nullptr;
        if (childPid_ <= 0) {
            return;
        }
        int status = 0;
        pid_t waited = -1;
        do {
            waited = ::waitpid(childPid_, &status, 0);
        } while (waited < 0 && errno == EINTR);
        if (waited != childPid_) {
            std::clog << "codex-bridge: unable to reap exited app-server process\n";
            return;
        }

        const int exitStatus = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : status;
        childPid_ = -1;
        const bool wasConnected = connected_;
        connected_ = false;
        if (wasConnected) {
            bridge_.appServerDisconnected("app-server process exited");
        }
        detachCallbacks();
        std::clog << "codex-bridge: app-server process exited status=" << exitStatus << '\n';
        if (!stopping_ && options_.onExit) {
            options_.onExit(exitStatus);
        }
    }

    void StdioAppServer::childObserverClosed(ChildExitObserver* observer) noexcept {
        if (childObserver_ == observer) {
            childObserver_ = nullptr;
        }
    }

    void StdioAppServer::detachChildObserver() noexcept {
        if (childObserver_ != nullptr) {
            ChildExitObserver* const observer = std::exchange(childObserver_, nullptr);
            observer->detach();
        }
    }

    void StdioAppServer::detachCallbacks() {
        if (stdinWriter_ != nullptr) {
            stdinWriter_->setOnError({});
            stdinWriter_->setOnClosed({});
            stdinWriter_->close();
            stdinWriter_ = nullptr;
        }
        if (stdoutReader_ != nullptr) {
            stdoutReader_->setOnData({});
            stdoutReader_->setOnEof({});
            stdoutReader_->setOnError({});
            stdoutReader_->setOnClosed({});
            stdoutReader_->close();
            stdoutReader_ = nullptr;
        }
        if (stderrReader_ != nullptr) {
            stderrReader_->setOnData({});
            stderrReader_->setOnClosed({});
            stderrReader_->close();
            stderrReader_ = nullptr;
        }
    }

} // namespace ai::openai::codex::provider
