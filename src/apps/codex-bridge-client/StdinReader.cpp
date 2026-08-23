/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-bridge-client/StdinReader.h"

#include "core/EventReceiver.h"
#include "log/SemanticLogger.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace apps::codex_bridge_client {

    StdinReader::StdinReader(std::size_t maximumLineBytes,
                             LineHandler onLine,
                             CloseHandler onClose,
                             ErrorHandler onError,
                             int descriptor)
        : core::eventreceiver::ReadEventReceiver(
              "codex-bridge-client stdin",
              logger::LogScope{logger::LogOrigin::Application,
                               logger::LogBoundary::Application,
                               "app",
                               "codex-bridge-client",
                               logger::LogRole::Unknown,
                               {}},
              core::DescriptorEventReceiver::TIMEOUT::DISABLE)
        , maximumLineBytes_(maximumLineBytes)
        , onLine_(std::move(onLine))
        , onClose_(std::move(onClose))
        , onError_(std::move(onError))
        , descriptor_(descriptor) {
        originalFlags_ = ::fcntl(descriptor_, F_GETFL);
        if (originalFlags_ < 0) {
            throw std::system_error(errno, std::generic_category(), "failed to inspect stdin flags");
        }
        if ((originalFlags_ & O_NONBLOCK) == 0) {
            if (::fcntl(descriptor_, F_SETFL, originalFlags_ | O_NONBLOCK) < 0) {
                throw std::system_error(errno, std::generic_category(), "failed to make stdin nonblocking");
            }
            restoreOriginalFlags_ = true;
        }

        struct stat status {};
        if (::fstat(descriptor_, &status) != 0) {
            const int error = errno;
            restoreFlags();
            throw std::system_error(error, std::generic_category(), "failed to inspect stdin descriptor");
        }
        if (S_ISREG(status.st_mode)) {
            restoreFlags();
            throw std::runtime_error("regular-file stdin is unsupported; pipe commands to codex-bridge-client");
        }

        if (S_ISCHR(status.st_mode) && ::isatty(descriptor_) == 0) {
            active_ = true;
            deferredReader_ = std::make_shared<StdinReader*>(this);
            core::EventReceiver::atNextTick([weak = std::weak_ptr<StdinReader*>(deferredReader_)] {
                if (const auto reader = weak.lock(); reader && *reader != nullptr && (*reader)->active_) {
                    (*reader)->readEvent();
                }
            });
            return;
        }
        if (!enable(descriptor_)) {
            restoreFlags();
            throw std::runtime_error("failed to register stdin with the SNode.C event loop");
        }
        registered_ = true;
        active_ = true;
    }

    StdinReader::~StdinReader() {
        stop();
    }

    void StdinReader::stop() noexcept {
        active_ = false;
        invalidateDeferredRead();
        if (registered_) {
            registered_ = false;
            try {
                disable();
            } catch (...) {
            }
        }
        restoreFlags();
    }

    bool StdinReader::active() const noexcept {
        return active_;
    }

    void StdinReader::readEvent() {
        std::array<char, 4096> chunk{};
        while (active_) {
            const ssize_t count = ::read(descriptor_, chunk.data(), chunk.size());
            if (count > 0) {
                buffered_.append(chunk.data(), static_cast<std::size_t>(count));
                emitLines();
                if (active_ && buffered_.size() > maximumLineBytes_) {
                    fail("stdin command exceeds configured bridge frame limit");
                }
                continue;
            }
            if (count == 0) {
                finish();
                return;
            }
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            fail(std::string("stdin read failed: ") + std::strerror(errno));
            return;
        }
    }

    void StdinReader::unobservedEvent() {
        registered_ = false;
        stop();
    }

    void StdinReader::shutdownEvent([[maybe_unused]] const core::ShutdownContext& context) {
        stop();
    }

    void StdinReader::emitLines() {
        while (active_) {
            const std::size_t newline = buffered_.find('\n');
            if (newline == std::string::npos) {
                return;
            }
            const std::size_t lineBytes = newline > 0 && buffered_[newline - 1] == '\r' ? newline - 1 : newline;
            if (lineBytes > maximumLineBytes_) {
                fail("stdin command exceeds configured bridge frame limit");
                return;
            }
            std::string line = buffered_.substr(0, lineBytes);
            buffered_.erase(0, newline + 1);
            try {
                if (onLine_) {
                    onLine_(std::move(line));
                }
            } catch (...) {
                fail("stdin command callback failed");
            }
        }
    }

    void StdinReader::finish() noexcept {
        if (!active_) {
            return;
        }
        if (!buffered_.empty() && buffered_.size() <= maximumLineBytes_) {
            std::string line = std::move(buffered_);
            buffered_.clear();
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            try {
                if (onLine_) {
                    onLine_(std::move(line));
                }
            } catch (...) {
                fail("stdin command callback failed");
                return;
            }
        }
        stop();
        try {
            if (onClose_) {
                onClose_();
            }
        } catch (...) {
        }
    }

    void StdinReader::fail(std::string reason) noexcept {
        if (!active_) {
            return;
        }
        stop();
        try {
            if (onError_) {
                onError_(std::move(reason));
            }
        } catch (...) {
        }
    }

    void StdinReader::restoreFlags() noexcept {
        if (restoreOriginalFlags_) {
            static_cast<void>(::fcntl(descriptor_, F_SETFL, originalFlags_));
            restoreOriginalFlags_ = false;
        }
    }

    void StdinReader::invalidateDeferredRead() noexcept {
        if (deferredReader_) {
            *deferredReader_ = nullptr;
            deferredReader_.reset();
        }
    }

} // namespace apps::codex_bridge_client
