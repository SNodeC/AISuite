/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef APPS_CODEX_BRIDGE_CLIENT_STDINREADER_H
#define APPS_CODEX_BRIDGE_CLIENT_STDINREADER_H

#include "core/eventreceiver/ReadEventReceiver.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>

namespace apps::codex_bridge_client {

    class StdinReader final : private core::eventreceiver::ReadEventReceiver {
    public:
        using LineHandler = std::function<void(std::string)>;
        using CloseHandler = std::function<void()>;
        using ErrorHandler = std::function<void(std::string)>;

        StdinReader(std::size_t maximumLineBytes,
                    LineHandler onLine,
                    CloseHandler onClose,
                    ErrorHandler onError = {},
                    int descriptor = 0);
        ~StdinReader() override;

        StdinReader(const StdinReader&) = delete;
        StdinReader& operator=(const StdinReader&) = delete;

        void stop() noexcept;
        bool active() const noexcept;

    private:
        void readEvent() override;
        void unobservedEvent() override;
        void shutdownEvent(const core::ShutdownContext& context) override;

        void emitLines();
        void finish() noexcept;
        void fail(std::string reason) noexcept;
        void restoreFlags() noexcept;
        void invalidateDeferredRead() noexcept;

        std::size_t maximumLineBytes_;
        LineHandler onLine_;
        CloseHandler onClose_;
        ErrorHandler onError_;
        int descriptor_;
        int originalFlags_ = -1;
        bool restoreOriginalFlags_ = false;
        bool active_ = false;
        bool registered_ = false;
        std::shared_ptr<StdinReader*> deferredReader_;
        std::string buffered_;
    };

} // namespace apps::codex_bridge_client

#endif
