/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_PROTOCOL_H
#define AI_OPENAI_CODEX_FRONTEND_PROTOCOL_H

#include "ai/openai/codex/Protocol.h"
#include "GeneratedProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace ai::openai::codex::frontend {

    inline constexpr std::string_view ProtocolIdentity = "snodec.codex-frontend";
    inline constexpr std::uint32_t ProtocolVersion = 1;
    inline constexpr std::array<std::uint32_t, 1> SupportedProtocolVersions{ProtocolVersion};
    // Hello advertises the newest closed capability-name vocabulary the
    // client can decode. An older peer ignores this additive field.
    inline constexpr std::uint32_t CapabilityVocabularyVersion = 1;

    // Alternate k-prefixed spellings are provided for consumers that use that
    // convention for public constants.
    inline constexpr std::string_view kProtocolIdentity = ProtocolIdentity;
    inline constexpr std::uint32_t kProtocolVersion = ProtocolVersion;
    inline constexpr auto kSupportedProtocolVersions = SupportedProtocolVersions;
    inline constexpr std::uint32_t kCapabilityVocabularyVersion = CapabilityVocabularyVersion;

    inline constexpr std::size_t DefaultJournalMaxEntries = 4096;
    inline constexpr std::size_t DefaultJournalMaxBytes = 8 * 1024 * 1024;
    inline constexpr std::size_t DefaultBatchMaxEvents = 64;
    inline constexpr std::size_t DefaultBatchMaxBytes = 256 * 1024;
    inline constexpr std::size_t DefaultMaxDirtyEntities = 4096;
    // A maximum Codex text input can require six JSON bytes per Unicode
    // scalar (for example U+0000 -> "\\u0000"), plus the command envelope.
    inline constexpr std::size_t DefaultFrontendMaximumInboundMessageBytes = 8U * 1024U * 1024U;
    inline constexpr std::size_t DefaultFrontendServiceMaxOutboundMessages = 512;

    // The public client accepts this much canonical decoded State.  A server
    // that can project a legitimate State of that size must also be able to
    // carry its unchunked Snapshot message; keep the contract centralized so
    // client admission, server message framing, and aggregate backpressure
    // cannot drift apart.
    inline constexpr std::size_t DefaultFrontendMaximumDecodedStateBytes = 128U * 1024U * 1024U;
    inline constexpr std::size_t DefaultFrontendProjectedSnapshotEnvelopeHeadroomBytes = 64U * 1024U;
    inline constexpr std::size_t DefaultFrontendMaximumProjectedSnapshotBytes =
        DefaultFrontendMaximumDecodedStateBytes + DefaultFrontendProjectedSnapshotEnvelopeHeadroomBytes;

    // A provider result is a subset of an accepted app-server response. The
    // frontend response adds its own bounded request/type envelope around that
    // result, so reserve explicit headroom beyond the provider framing limit.
    inline constexpr std::size_t DefaultFrontendServerMessageEnvelopeHeadroomBytes = 64U * 1024U;
    inline constexpr std::size_t DefaultFrontendMaximumProviderResponseBytes =
        MaximumAppServerFramedLineBytes + DefaultFrontendServerMessageEnvelopeHeadroomBytes;

    // Journal byte accounting measures compact event objects. Replaying them
    // adds an events-envelope and separators. At most one envelope is needed
    // per retained event, and a v1 envelope (including maximum-width sequence
    // numbers and its separator) is safely below 512 bytes. Reserve that
    // deliberately conservative worst case, plus space for welcome and
    // sync.complete, so a full default journal is replayable by the default
    // adapter instead of being rejected by its own backpressure boundary.
    inline constexpr std::size_t DefaultReplayEnvelopeHeadroomPerEntry = 512;
    inline constexpr std::size_t DefaultReplayControlHeadroomBytes = 64 * 1024;
    inline constexpr std::size_t DefaultFrontendMaximumReplayBytes =
        DefaultJournalMaxBytes + DefaultJournalMaxEntries * DefaultReplayEnvelopeHeadroomPerEntry +
        DefaultReplayControlHeadroomBytes;
    // One individual message may be either a maximum provider-derived result
    // or a maximum projected Snapshot.  The aggregate queue then reserves an
    // independent replay/control allowance in addition to that message.
    inline constexpr std::size_t DefaultFrontendMaximumServerMessageBytes =
        DefaultFrontendMaximumProjectedSnapshotBytes > DefaultFrontendMaximumProviderResponseBytes
            ? DefaultFrontendMaximumProjectedSnapshotBytes
            : DefaultFrontendMaximumProviderResponseBytes;
    inline constexpr std::size_t DefaultFrontendServiceMaxOutboundBytes =
        DefaultFrontendMaximumServerMessageBytes + DefaultFrontendMaximumReplayBytes;
    inline constexpr std::size_t DefaultFrontendServiceMaxMessagesPerDelivery = 64;

    static_assert(DefaultFrontendServiceMaxOutboundBytes >= DefaultFrontendMaximumProviderResponseBytes);
    static_assert(DefaultFrontendServiceMaxOutboundBytes >= DefaultFrontendMaximumReplayBytes);
    static_assert(DefaultFrontendMaximumServerMessageBytes >= DefaultFrontendMaximumProjectedSnapshotBytes);
    static_assert(DefaultFrontendServiceMaxOutboundBytes - DefaultFrontendMaximumServerMessageBytes >=
                  DefaultFrontendMaximumReplayBytes);

    inline constexpr std::size_t kDefaultJournalMaxEntries = DefaultJournalMaxEntries;
    inline constexpr std::size_t kDefaultJournalMaxBytes = DefaultJournalMaxBytes;
    inline constexpr std::size_t kDefaultBatchMaxEvents = DefaultBatchMaxEvents;
    inline constexpr std::size_t kDefaultBatchMaxBytes = DefaultBatchMaxBytes;
    inline constexpr std::size_t kDefaultMaxDirtyEntities = DefaultMaxDirtyEntities;
    inline constexpr std::size_t kDefaultFrontendMaximumInboundMessageBytes = DefaultFrontendMaximumInboundMessageBytes;
    inline constexpr std::size_t kDefaultFrontendServiceMaxOutboundMessages = DefaultFrontendServiceMaxOutboundMessages;
    inline constexpr std::size_t kDefaultFrontendMaximumDecodedStateBytes = DefaultFrontendMaximumDecodedStateBytes;
    inline constexpr std::size_t kDefaultFrontendProjectedSnapshotEnvelopeHeadroomBytes =
        DefaultFrontendProjectedSnapshotEnvelopeHeadroomBytes;
    inline constexpr std::size_t kDefaultFrontendMaximumProjectedSnapshotBytes = DefaultFrontendMaximumProjectedSnapshotBytes;
    inline constexpr std::size_t kDefaultFrontendServerMessageEnvelopeHeadroomBytes =
        DefaultFrontendServerMessageEnvelopeHeadroomBytes;
    inline constexpr std::size_t kDefaultFrontendMaximumProviderResponseBytes = DefaultFrontendMaximumProviderResponseBytes;
    inline constexpr std::size_t kDefaultFrontendMaximumServerMessageBytes = DefaultFrontendMaximumServerMessageBytes;
    inline constexpr std::size_t kDefaultReplayEnvelopeHeadroomPerEntry = DefaultReplayEnvelopeHeadroomPerEntry;
    inline constexpr std::size_t kDefaultReplayControlHeadroomBytes = DefaultReplayControlHeadroomBytes;
    inline constexpr std::size_t kDefaultFrontendMaximumReplayBytes = DefaultFrontendMaximumReplayBytes;
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
