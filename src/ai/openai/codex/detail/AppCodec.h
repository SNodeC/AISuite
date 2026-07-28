/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_APPCODEC_H
#define AI_OPENAI_CODEX_DETAIL_APPCODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/Apps.h"

#include <optional>
#include <string>
#include <string_view>

namespace ai::openai::codex::detail {

    std::optional<Json> encodeAppsListParams(const typed::AppsListParams& value, std::string& error) noexcept;

    std::optional<typed::AppsListResponse> decodeAppsListResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::AppListUpdatedNotification> decodeAppListUpdatedNotification(const Notification& notification,
                                                                                      std::string& error) noexcept;

    // Shared App-domain leaves used by plugin catalog responses.
    std::optional<typed::AppSummary> decodeAppSummary(const Json& value, std::string& error, std::string_view fieldPath = "$") noexcept;
    std::optional<typed::AppTemplateSummary>
    decodeAppTemplateSummary(const Json& value, std::string& error, std::string_view fieldPath = "$") noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_APPCODEC_H
