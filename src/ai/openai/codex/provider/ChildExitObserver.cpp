/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/provider/ChildExitObserver.h"

#include "core/system/unistd.h"
#include "log/SemanticLogger.h"

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

namespace ai::openai::codex::provider {

    namespace {

        int openPidFd(pid_t pid) {
#if defined(__linux__) && defined(SYS_pidfd_open)
            return static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
#else
            static_cast<void>(pid);
            errno = ENOSYS;
            return -1;
#endif
        }

        int relocateAboveStandardDescriptors(int descriptor) {
            if (descriptor < 0 || descriptor > STDERR_FILENO) {
                return descriptor;
            }
            const int relocated = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            const int savedErrno = errno;
            core::system::close(descriptor);
            errno = savedErrno;
            return relocated;
        }

    } // namespace

    ChildExitObserver* ChildExitObserver::observe(pid_t pid, Callback onExit, Callback onClosed) {
        const int pidfd = relocateAboveStandardDescriptors(openPidFd(pid));
        if (pidfd < 0) {
            return nullptr;
        }
        try {
            return new ChildExitObserver(pidfd, std::move(onExit), std::move(onClosed));
        } catch (...) {
            core::system::close(pidfd);
            return nullptr;
        }
    }

    void ChildExitObserver::terminateAndReap(pid_t pid) noexcept {
        if (pid <= 0) {
            return;
        }

        static_cast<void>(::kill(-pid, SIGTERM));
        const int pidfd = relocateAboveStandardDescriptors(openPidFd(pid));
        bool exited = false;
        if (pidfd >= 0) {
            pollfd descriptor{.fd = pidfd, .events = POLLIN, .revents = 0};
            const int result = ::poll(&descriptor, 1, 1000);
            exited = result > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0;
            core::system::close(pidfd);
        }
        if (!exited) {
            static_cast<void>(::kill(-pid, SIGKILL));
        }

        int status = 0;
        pid_t waited = -1;
        do {
            waited = ::waitpid(pid, &status, 0);
        } while (waited < 0 && errno == EINTR);
    }

    ChildExitObserver::ChildExitObserver(int pidfd, Callback onExit, Callback onClosed)
        : core::eventreceiver::ReadEventReceiver("codex app-server pidfd",
                                                 logger::LogScope{logger::LogOrigin::Application,
                                                                  logger::LogBoundary::Application,
                                                                  "codex.provider",
                                                                  "app-server",
                                                                  logger::LogRole::Unknown,
                                                                  {}},
                                                 TIMEOUT::DISABLE)
        , onExit_(std::move(onExit))
        , onClosed_(std::move(onClosed)) {
        if (!ReadEventReceiver::enable(pidfd)) {
            throw std::runtime_error("unable to register app-server pidfd");
        }
    }

    ChildExitObserver::~ChildExitObserver() {
        core::system::close(getRegisteredFd());
    }

    void ChildExitObserver::detach() noexcept {
        onExit_ = {};
        onClosed_ = {};
        if (ReadEventReceiver::isEnabled()) {
            ReadEventReceiver::disable();
        }
    }

    void ChildExitObserver::readEvent() {
        Callback callback = std::move(onExit_);
        ReadEventReceiver::disable();
        if (callback) {
            callback(this);
        }
    }

    void ChildExitObserver::unobservedEvent() {
        Callback callback = std::move(onClosed_);
        if (callback) {
            callback(this);
        }
        delete this;
    }

    void ChildExitObserver::shutdownEvent(const core::ShutdownContext&) {
        onExit_ = {};
        Callback callback = std::move(onClosed_);
        if (callback) {
            callback(this);
        }
        if (ReadEventReceiver::isEnabled()) {
            ReadEventReceiver::disable();
        }
    }

} // namespace ai::openai::codex::provider
