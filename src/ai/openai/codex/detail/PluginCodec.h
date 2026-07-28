/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_DETAIL_PLUGINCODEC_H
#define AI_OPENAI_CODEX_DETAIL_PLUGINCODEC_H

#include "ai/openai/codex/Protocol.h"
#include "ai/openai/codex/typed/Plugins.h"

#include <optional>
#include <string>

namespace ai::openai::codex::detail {

    std::optional<Json> encodePluginInstallParams(const typed::PluginInstallParams& value, std::string& error) noexcept;
    std::optional<Json> encodePluginShareCheckoutParams(const typed::PluginShareCheckoutParams& value, std::string& error) noexcept;
    std::optional<Json> encodePluginShareDeleteParams(const typed::PluginShareDeleteParams& value, std::string& error) noexcept;
    std::optional<Json> encodePluginShareSaveParams(const typed::PluginShareSaveParams& value, std::string& error) noexcept;
    std::optional<Json> encodePluginShareUpdateTargetsParams(const typed::PluginShareUpdateTargetsParams& value,
                                                             std::string& error) noexcept;
    std::optional<Json> encodePluginSkillReadParams(const typed::PluginSkillReadParams& value, std::string& error) noexcept;
    std::optional<Json> encodePluginUninstallParams(const typed::PluginUninstallParams& value, std::string& error) noexcept;

    std::optional<typed::PluginInstallResponse> decodePluginInstallResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::PluginShareCheckoutResponse> decodePluginShareCheckoutResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::PluginShareSaveResponse> decodePluginShareSaveResponse(const Json& value, std::string& error) noexcept;
    std::optional<typed::PluginShareUpdateTargetsResponse> decodePluginShareUpdateTargetsResponse(const Json& value,
                                                                                                  std::string& error) noexcept;
    std::optional<typed::PluginSkillReadResponse> decodePluginSkillReadResponse(const Json& value, std::string& error) noexcept;

} // namespace ai::openai::codex::detail

#endif // AI_OPENAI_CODEX_DETAIL_PLUGINCODEC_H
