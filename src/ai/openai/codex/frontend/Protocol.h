/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_PROTOCOL_H
#define AI_OPENAI_CODEX_FRONTEND_PROTOCOL_H

#include "GeneratedProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ai::openai::codex::frontend {

    inline constexpr std::string_view ProtocolIdentity = "snodec.codex-frontend";
    inline constexpr std::uint32_t ProtocolVersion = 1;
    inline constexpr std::array<std::uint32_t, 1> SupportedProtocolVersions{ProtocolVersion};

    // Alternate k-prefixed spellings are provided for consumers that use that
    // convention for public constants.
    inline constexpr std::string_view kProtocolIdentity = ProtocolIdentity;
    inline constexpr std::uint32_t kProtocolVersion = ProtocolVersion;
    inline constexpr auto kSupportedProtocolVersions = SupportedProtocolVersions;

    inline constexpr std::size_t DefaultJournalMaxEntries = 4096;
    inline constexpr std::size_t DefaultJournalMaxBytes = 8 * 1024 * 1024;
    inline constexpr std::size_t DefaultBatchMaxEvents = 64;
    inline constexpr std::size_t DefaultBatchMaxBytes = 256 * 1024;
    inline constexpr std::size_t DefaultMaxDirtyEntities = 4096;
    inline constexpr std::size_t DefaultFrontendMaximumInboundMessageBytes = 1024U * 1024U;
    inline constexpr std::size_t DefaultFrontendServiceMaxOutboundMessages = 512;

    // Journal byte accounting measures compact event objects. Replaying them
    // adds an events-envelope and separators. At most one envelope is needed
    // per retained event, and a v1 envelope (including maximum-width sequence
    // numbers and its separator) is safely below 512 bytes. Reserve that
    // deliberately conservative worst case, plus space for welcome and
    // sync.complete, so a full default journal is replayable by the default
    // adapter instead of being rejected by its own backpressure boundary.
    inline constexpr std::size_t DefaultReplayEnvelopeHeadroomPerEntry = 512;
    inline constexpr std::size_t DefaultReplayControlHeadroomBytes = 64 * 1024;
    inline constexpr std::size_t DefaultFrontendServiceMaxOutboundBytes = 11 * 1024 * 1024;
    inline constexpr std::size_t DefaultFrontendServiceMaxMessagesPerDelivery = 64;

    static_assert(DefaultFrontendServiceMaxOutboundBytes >= DefaultJournalMaxBytes +
                                                                DefaultJournalMaxEntries * DefaultReplayEnvelopeHeadroomPerEntry +
                                                                DefaultReplayControlHeadroomBytes);

    inline constexpr std::size_t kDefaultJournalMaxEntries = DefaultJournalMaxEntries;
    inline constexpr std::size_t kDefaultJournalMaxBytes = DefaultJournalMaxBytes;
    inline constexpr std::size_t kDefaultBatchMaxEvents = DefaultBatchMaxEvents;
    inline constexpr std::size_t kDefaultBatchMaxBytes = DefaultBatchMaxBytes;
    inline constexpr std::size_t kDefaultMaxDirtyEntities = DefaultMaxDirtyEntities;
    inline constexpr std::size_t kDefaultFrontendMaximumInboundMessageBytes = DefaultFrontendMaximumInboundMessageBytes;
    inline constexpr std::size_t kDefaultFrontendServiceMaxOutboundMessages = DefaultFrontendServiceMaxOutboundMessages;
    inline constexpr std::size_t kDefaultReplayEnvelopeHeadroomPerEntry = DefaultReplayEnvelopeHeadroomPerEntry;
    inline constexpr std::size_t kDefaultReplayControlHeadroomBytes = DefaultReplayControlHeadroomBytes;
    inline constexpr std::size_t kDefaultFrontendServiceMaxOutboundBytes = DefaultFrontendServiceMaxOutboundBytes;
    inline constexpr std::size_t kDefaultFrontendServiceMaxMessagesPerDelivery = DefaultFrontendServiceMaxMessagesPerDelivery;

    namespace kind {
        inline constexpr std::string_view Hello = "hello";
        inline constexpr std::string_view Welcome = "welcome";
        inline constexpr std::string_view SyncComplete = "sync.complete";
        inline constexpr std::string_view Command = "command";
        inline constexpr std::string_view Response = "response";
        inline constexpr std::string_view Snapshot = "snapshot";
        inline constexpr std::string_view Events = "events";
        inline constexpr std::string_view ProtocolError = "protocol.error";
    } // namespace kind

} // namespace ai::openai::codex::frontend

#endif // AI_OPENAI_CODEX_FRONTEND_PROTOCOL_H
