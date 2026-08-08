/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "apps/codex-backend-client/StdinReader.h"
#include "core/EventReceiver.h"
#include "core/SNodeC.h"
#include "core/timer/Timer.h"
#include "support/TestResult.h"
#include "utils/Timeval.h"

#include <cerrno>
#include <csignal>
#include <exception>
#include <fcntl.h>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace apps::codex_backend_client {

    struct StdinReaderTestAccess {
        static void readNow(StdinReader& reader) {
            reader.readEvent();
        }
    };

} // namespace apps::codex_backend_client

namespace {

    class Pipe final {
    public:
        Pipe() {
            int descriptors[2] = {-1, -1};
            if (::pipe(descriptors) == 0) {
                readFd = descriptors[0];
                writeFd = descriptors[1];
            }
        }

        Pipe(const Pipe&) = delete;
        Pipe(Pipe&&) = delete;

        Pipe& operator=(const Pipe&) = delete;
        Pipe& operator=(Pipe&&) = delete;

        ~Pipe() {
            if (readFd >= 0) {
                static_cast<void>(::close(readFd));
            }
            if (writeFd >= 0) {
                static_cast<void>(::close(writeFd));
            }
        }

        [[nodiscard]] bool valid() const noexcept {
            return readFd >= 0 && writeFd >= 0;
        }

        [[nodiscard]] bool writeAll(std::string_view data) const noexcept {
            std::size_t offset = 0;
            while (offset < data.size()) {
                const ssize_t count = ::write(writeFd, data.data() + offset, data.size() - offset);
                if (count > 0) {
                    offset += static_cast<std::size_t>(count);
                } else if (count < 0 && errno == EINTR) {
                    continue;
                } else {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool closeWriter() noexcept {
            if (writeFd < 0) {
                return true;
            }
            const int descriptor = std::exchange(writeFd, -1);
            return ::close(descriptor) == 0;
        }

        int readFd = -1;

    private:
        int writeFd = -1;
    };

    [[nodiscard]] bool callbacksMatch(const std::vector<std::string>& actual, std::initializer_list<std::string_view> expected) {
        if (actual.size() != expected.size()) {
            return false;
        }

        auto actualIterator = actual.begin();
        for (const std::string_view expectedEntry : expected) {
            if (*actualIterator != expectedEntry) {
                return false;
            }
            ++actualIterator;
        }
        return true;
    }

} // namespace

int main(int argc, char* argv[]) {
    tests::support::TestResult testResult;
    if (tests::support::shouldSkipRootWithoutSNodeCGroup()) {
        tests::support::printRootWithoutSNodeCGroupSkipMessage("CodexBackendClientStdinReaderTest");
        return tests::support::cTestSkipReturnCode;
    }

    core::SNodeC::init(argc, argv);

    Pipe blockingPipe;
    Pipe nonblockingPipe;
    Pipe shutdownPipe;
    Pipe eagainPipe;
    Pipe signalPipe;
    const bool pipesCreated =
        blockingPipe.valid() && nonblockingPipe.valid() && shutdownPipe.valid() && eagainPipe.valid() && signalPipe.valid();
    testResult.expectTrue(pipesCreated, "stdin reader test pipes are created");
    if (!pipesCreated) {
        core::SNodeC::free();
        return testResult.processResult();
    }

    const int blockingOriginalFlags = ::fcntl(blockingPipe.readFd, F_GETFL);
    const int nonblockingInitialFlags = ::fcntl(nonblockingPipe.readFd, F_GETFL);
    const int shutdownOriginalFlags = ::fcntl(shutdownPipe.readFd, F_GETFL);
    const bool flagsInspected = blockingOriginalFlags >= 0 && nonblockingInitialFlags >= 0 && shutdownOriginalFlags >= 0;
    testResult.expectTrue(flagsInspected, "initial read descriptor flags are available");
    if (!flagsInspected) {
        core::SNodeC::free();
        return testResult.processResult();
    }

    const int nonblockingOriginalFlags = nonblockingInitialFlags | O_NONBLOCK;
    const bool preexistingNonblockingSet = ::fcntl(nonblockingPipe.readFd, F_SETFL, nonblockingOriginalFlags) == 0;
    testResult.expectTrue(preexistingNonblockingSet, "second pipe begins with O_NONBLOCK already set");
    if (!preexistingNonblockingSet) {
        core::SNodeC::free();
        return testResult.processResult();
    }

    const std::string blockingInput = "first\nsecond\r\nfinal-without-newline";
    const bool inputWritten = blockingPipe.writeAll(blockingInput);
    const bool blockingWriterClosed = blockingPipe.closeWriter();
    const bool nonblockingWriterClosed = nonblockingPipe.closeWriter();
    testResult.expectTrue(inputWritten, "multiple lines are written to the real pipe");
    testResult.expectTrue(blockingWriterClosed && nonblockingWriterClosed, "pipe writers close to produce EOF");

    std::vector<std::string> blockingCallbacks;
    std::vector<std::string> errors;
    int eofCount = 0;
    int blockingFlagsInsideEof = -1;
    int nonblockingFlagsInsideEof = -1;
    bool stopScheduled = false;
    bool watchdogExpired = false;

    const auto scheduleStopAfterBothEofs = [&]() {
        ++eofCount;
        if (eofCount == 2 && !stopScheduled) {
            stopScheduled = true;
            core::EventReceiver::atNextTick([]() {
                static_cast<void>(::kill(::getpid(), SIGINT));
            });
        }
    };

    std::unique_ptr<apps::codex_backend_client::StdinReader> blockingReader;
    std::unique_ptr<apps::codex_backend_client::StdinReader> nonblockingReader;
    std::unique_ptr<apps::codex_backend_client::StdinReader> shutdownReader;
    std::unique_ptr<apps::codex_backend_client::StdinReader> eagainReader;
    std::unique_ptr<apps::codex_backend_client::StdinReader> signalReader;
    std::size_t eagainLineCount = 0;
    std::size_t eagainEofCount = 0;
    std::size_t eagainErrorCount = 0;
    std::size_t shutdownNotificationCount = 0;

    try {
        blockingReader = std::make_unique<apps::codex_backend_client::StdinReader>(
            [&blockingCallbacks](std::string line) {
                blockingCallbacks.emplace_back("line:" + std::move(line));
            },
            [&]() {
                blockingFlagsInsideEof = ::fcntl(blockingPipe.readFd, F_GETFL);
                blockingCallbacks.emplace_back("eof");
                scheduleStopAfterBothEofs();
            },
            [&errors](std::string error) {
                errors.push_back(std::move(error));
                core::SNodeC::stop();
            },
            blockingPipe.readFd);

        nonblockingReader = std::make_unique<apps::codex_backend_client::StdinReader>(
            [](std::string) {
            },
            [&]() {
                nonblockingFlagsInsideEof = ::fcntl(nonblockingPipe.readFd, F_GETFL);
                scheduleStopAfterBothEofs();
            },
            [&errors](std::string error) {
                errors.push_back(std::move(error));
                core::SNodeC::stop();
            },
            nonblockingPipe.readFd);

        shutdownReader = std::make_unique<apps::codex_backend_client::StdinReader>(
            [](std::string) {
            },
            []() {
            },
            [&errors](std::string error) {
                errors.push_back(std::move(error));
                core::SNodeC::stop();
            },
            shutdownPipe.readFd);

        eagainReader = std::make_unique<apps::codex_backend_client::StdinReader>(
            [&](std::string) {
                ++eagainLineCount;
            },
            [&]() {
                ++eagainEofCount;
            },
            [&](std::string) {
                ++eagainErrorCount;
            },
            eagainPipe.readFd);

        signalReader = std::make_unique<apps::codex_backend_client::StdinReader>(
            [](std::string) {
            },
            []() {
            },
            [&errors](std::string error) {
                errors.push_back(std::move(error));
            },
            signalPipe.readFd,
            [&]() {
                ++shutdownNotificationCount;
            });
    } catch (const std::exception& exception) {
        errors.emplace_back(exception.what());
    }

    testResult.expectTrue(blockingReader != nullptr && nonblockingReader != nullptr && shutdownReader != nullptr &&
                              eagainReader != nullptr && signalReader != nullptr,
                          "stdin readers register all real pipe descriptors");
    if (!blockingReader || !nonblockingReader || !shutdownReader || !eagainReader || !signalReader) {
        blockingReader.reset();
        nonblockingReader.reset();
        shutdownReader.reset();
        eagainReader.reset();
        signalReader.reset();
        core::SNodeC::free();
        return testResult.processResult();
    }

    const int blockingFlagsWhileActive = ::fcntl(blockingPipe.readFd, F_GETFL);
    const int nonblockingFlagsWhileActive = ::fcntl(nonblockingPipe.readFd, F_GETFL);
    testResult.expectTrue(blockingFlagsWhileActive == (blockingOriginalFlags | O_NONBLOCK),
                          "stdin reader temporarily enables O_NONBLOCK on a blocking pipe");
    testResult.expectTrue(nonblockingFlagsWhileActive == nonblockingOriginalFlags,
                          "stdin reader preserves a preexisting O_NONBLOCK flag while active");

    const int shutdownFlagsWhileActive = ::fcntl(shutdownPipe.readFd, F_GETFL);
    shutdownReader->stop();
    const int shutdownFlagsAfterStop = ::fcntl(shutdownPipe.readFd, F_GETFL);
    testResult.expectTrue(shutdownFlagsWhileActive == (shutdownOriginalFlags | O_NONBLOCK),
                          "stdin reader makes a still-open shutdown pipe nonblocking while active");
    testResult.expectTrue(!shutdownReader->active() && shutdownFlagsAfterStop == shutdownOriginalFlags,
                          "explicit stdin shutdown restores descriptor flags without waiting for EOF");

    // The writer deliberately remains open and has produced no bytes. A
    // direct nonblocking drain therefore returns EAGAIN, not EOF.
    apps::codex_backend_client::StdinReaderTestAccess::readNow(*eagainReader);
    testResult.expectTrue(eagainReader->active() && eagainLineCount == 0 && eagainEofCount == 0 && eagainErrorCount == 0,
                          "stdin EAGAIN leaves the reader active and does not synthesize EOF or failure");
    eagainReader->stop();

    core::timer::Timer watchdog = core::timer::Timer::singleshotTimer(
        [&]() {
            watchdogExpired = true;
            core::SNodeC::stop();
        },
        utils::Timeval({2, 0}));

    const int eventLoopResult = core::SNodeC::start(utils::Timeval({3, 0}));

    testResult.expectTrue(!watchdogExpired, "both pipe EOF callbacks complete before the safety watchdog");
    testResult.expectEqual(-SIGINT, eventLoopResult, "stdin reader test event loop preserves the framework signal result");
    testResult.expectTrue(errors.empty(), "reading the pipes produces no stdin errors");
    testResult.expectEqual(2, eofCount, "EOF is reported exactly once for each pipe");
    testResult.expectTrue(callbacksMatch(blockingCallbacks, {"line:first", "line:second", "line:final-without-newline", "eof"}),
                          "multiple lines and the final unterminated line precede EOF in order");
    testResult.expectTrue(!blockingReader->active() && !nonblockingReader->active(), "stdin readers become inactive at EOF");
    testResult.expectTrue(blockingFlagsInsideEof == blockingOriginalFlags,
                          "blocking descriptor flags are restored exactly before its EOF callback");
    testResult.expectTrue(nonblockingFlagsInsideEof == nonblockingOriginalFlags,
                          "preexisting O_NONBLOCK is preserved exactly inside its EOF callback");
    testResult.expectEqual(std::size_t{1},
                           shutdownNotificationCount,
                           "the registered stdin receiver publishes one intentional local-shutdown notification before signal teardown");
    testResult.expectTrue(!signalReader->active(), "signal shutdown disables stdin without synthesizing EOF");

    blockingReader.reset();
    nonblockingReader.reset();
    shutdownReader.reset();
    eagainReader.reset();
    signalReader.reset();

    const int blockingFlagsAfterDestruction = ::fcntl(blockingPipe.readFd, F_GETFL);
    const int nonblockingFlagsAfterDestruction = ::fcntl(nonblockingPipe.readFd, F_GETFL);
    testResult.expectTrue(blockingFlagsAfterDestruction == blockingOriginalFlags,
                          "blocking descriptor flags remain restored exactly after reader destruction");
    testResult.expectTrue(nonblockingFlagsAfterDestruction == nonblockingOriginalFlags,
                          "preexisting O_NONBLOCK remains unchanged after reader destruction");

    core::SNodeC::free();
    return testResult.processResult();
}
