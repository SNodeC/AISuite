/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_RUNTIMEPLATFORMCODEC_H
#define AI_OPENAI_CODEX_DETAIL_RUNTIMEPLATFORMCODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/Events.h"

#include <optional>
#include <string>

namespace ai::openai::codex::detail {

    std::optional<typed::DeprecationNoticeNotification> decodeDeprecationNoticeNotification(const Notification& notification,
                                                                                            std::string& error) noexcept;
    std::optional<typed::ProcessExitedNotification> decodeProcessExitedNotification(const Notification& notification,
                                                                                    std::string& error) noexcept;
    std::optional<typed::ProcessOutputDeltaNotification> decodeProcessOutputDeltaNotification(const Notification& notification,
                                                                                              std::string& error) noexcept;
    std::optional<typed::RemoteControlStatusChangedNotification>
    decodeRemoteControlStatusChangedNotification(const Notification& notification, std::string& error) noexcept;
    std::optional<typed::ServerRequestResolvedNotification> decodeServerRequestResolvedNotification(const Notification& notification,
                                                                                                    std::string& error) noexcept;
    std::optional<typed::WarningNotification> decodeWarningNotification(const Notification& notification, std::string& error) noexcept;
    std::optional<typed::WindowsWorldWritableWarningNotification>
    decodeWindowsWorldWritableWarningNotification(const Notification& notification, std::string& error) noexcept;
    std::optional<typed::WindowsSandboxSetupCompletedNotification>
    decodeWindowsSandboxSetupCompletedNotification(const Notification& notification, std::string& error) noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_RUNTIMEPLATFORMCODEC_H
