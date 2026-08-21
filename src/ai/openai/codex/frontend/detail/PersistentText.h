/*
 * SNode.C - A Slim Toolkit for Network Communication
 * Copyright (C) Volker Christian <me@vchrist.at>
 *
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#ifndef AI_OPENAI_CODEX_FRONTEND_DETAIL_PERSISTENTTEXT_H
#define AI_OPENAI_CODEX_FRONTEND_DETAIL_PERSISTENTTEXT_H

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ai::openai::codex::frontend::detail {

    // Immutable byte-range views over an append-only, fixed-chunk store.
    // Publishing a new view copies only the incoming delta; older views keep
    // their end offset and therefore remain semantically immutable even while
    // the shared store receives later chunks.
    class PersistentText {
    public:
        static constexpr std::size_t ChunkBytes = 16 * 1024;

        PersistentText()
            : storage(std::make_shared<Storage>()) {
        }

        [[nodiscard]] static PersistentText from(std::string_view value) {
            PersistentText result;
            result.appendToStorage(value);
            result.endOffset = value.size();
            result.escapedPayloadBytes = escapedBytes(value);
            return result;
        }

        [[nodiscard]] std::size_t size() const noexcept {
            return endOffset - beginOffset;
        }

        [[nodiscard]] bool empty() const noexcept {
            return size() == 0;
        }

        // Compact nlohmann JSON string bytes, including surrounding quotes.
        [[nodiscard]] std::size_t jsonStringBytes() const noexcept {
            return escapedPayloadBytes + 2;
        }

        [[nodiscard]] std::optional<unsigned char> byteAt(std::size_t offset) const noexcept {
            if (offset >= size()) {
                return std::nullopt;
            }
            try {
                std::lock_guard lock(storage->mutex);
                const std::size_t absolute = beginOffset + offset;
                const std::size_t chunk = absolute / ChunkBytes;
                const std::size_t inChunk = absolute % ChunkBytes;
                if (chunk >= storage->chunks.size() || inChunk >= storage->chunks[chunk].size()) {
                    return std::nullopt;
                }
                return static_cast<unsigned char>(storage->chunks[chunk][inChunk]);
            } catch (...) {
                return std::nullopt;
            }
        }

        // Returns a new immutable view.  A branch from anything other than
        // the store's current tail is detached once; the normal publication
        // path is tail-linear and copies only `delta`.
        [[nodiscard]] std::optional<PersistentText> appended(std::size_t discardPrefixBytes,
                                                              std::string_view delta) const noexcept {
            if (discardPrefixBytes > size() || delta.size() > std::numeric_limits<std::size_t>::max() -
                                                                    (size() - discardPrefixBytes)) {
                return std::nullopt;
            }
            try {
                PersistentText result = *this;
                std::size_t discardedEscapedBytes = 0;
                bool detached = false;
                {
                    std::lock_guard lock(storage->mutex);
                    const std::size_t retainedBytes = size() - discardPrefixBytes;
                    const std::size_t nextBegin = beginOffset + discardPrefixBytes;
                    const std::size_t compactionThreshold = std::max(ChunkBytes, retainedBytes);
                    // A non-tail branch must detach before appending. A rolling
                    // view also re-roots after at most one retained window (or
                    // one fixed chunk) of dead prefix, so bounded command output cannot
                    // retain the command's unbounded lifetime output.
                    if (endOffset != storage->totalBytes ||
                        (discardPrefixBytes != 0 && nextBegin >= compactionThreshold)) {
                        const std::string retained = materializeRangeLocked(nextBegin, endOffset);
                        result = PersistentText::from(retained);
                        detached = true;
                    } else {
                        discardedEscapedBytes = escapedRangeLocked(beginOffset, beginOffset + discardPrefixBytes);
                        result.beginOffset += discardPrefixBytes;
                        result.escapedPayloadBytes -= discardedEscapedBytes;
                        // Tail validation and append must be one critical
                        // section. Otherwise two derivations from the same
                        // immutable tail can both pass validation and append
                        // into each other's result range.
                        result.appendToStorageLocked(delta);
                    }
                }
                if (detached) {
                    result.appendToStorage(delta);
                }
                result.endOffset += delta.size();
                result.escapedPayloadBytes += escapedBytes(delta);
                return result;
            } catch (...) {
                return std::nullopt;
            }
        }

        [[nodiscard]] std::string materialize() const {
            std::lock_guard lock(storage->mutex);
            return materializeLocked();
        }

        [[nodiscard]] std::size_t backingBytesForTesting() const noexcept {
            try {
                std::lock_guard lock(storage->mutex);
                return storage->totalBytes;
            } catch (...) {
                return std::numeric_limits<std::size_t>::max();
            }
        }

    private:
        struct Storage {
            mutable std::mutex mutex;
            std::vector<std::string> chunks;
            std::size_t totalBytes = 0;
        };

        void appendToStorage(std::string_view value) {
            std::lock_guard lock(storage->mutex);
            appendToStorageLocked(value);
        }

        void appendToStorageLocked(std::string_view value) {
            while (!value.empty()) {
                if (storage->chunks.empty() || storage->chunks.back().size() == ChunkBytes) {
                    storage->chunks.emplace_back();
                    storage->chunks.back().reserve(ChunkBytes);
                }
                std::string& chunk = storage->chunks.back();
                const std::size_t copied = std::min(ChunkBytes - chunk.size(), value.size());
                chunk.append(value.data(), copied);
                value.remove_prefix(copied);
                storage->totalBytes += copied;
            }
        }

        [[nodiscard]] std::string materializeLocked() const {
            return materializeRangeLocked(beginOffset, endOffset);
        }

        [[nodiscard]] std::string materializeRangeLocked(std::size_t first, std::size_t last) const {
            std::string result;
            result.reserve(last - first);
            std::size_t absolute = first;
            while (absolute < last) {
                const std::size_t chunkIndex = absolute / ChunkBytes;
                const std::size_t inChunk = absolute % ChunkBytes;
                const std::string& chunk = storage->chunks.at(chunkIndex);
                const std::size_t copied = std::min(last - absolute, chunk.size() - inChunk);
                result.append(chunk.data() + static_cast<std::ptrdiff_t>(inChunk), copied);
                absolute += copied;
            }
            return result;
        }

        [[nodiscard]] static std::size_t escapedBytes(std::string_view value) noexcept {
            std::size_t result = 0;
            for (const unsigned char byte : value) {
                if (byte == '"' || byte == '\\' || byte == '\b' || byte == '\f' || byte == '\n' || byte == '\r' || byte == '\t') {
                    result += 2;
                } else if (byte < 0x20U) {
                    result += 6;
                } else {
                    ++result;
                }
            }
            return result;
        }

        [[nodiscard]] std::size_t escapedRangeLocked(std::size_t first, std::size_t last) const noexcept {
            std::size_t result = 0;
            std::size_t absolute = first;
            while (absolute < last) {
                const std::size_t chunkIndex = absolute / ChunkBytes;
                const std::size_t inChunk = absolute % ChunkBytes;
                if (chunkIndex >= storage->chunks.size()) {
                    return std::numeric_limits<std::size_t>::max();
                }
                const std::string& chunk = storage->chunks[chunkIndex];
                const std::size_t copied = std::min(last - absolute, chunk.size() - inChunk);
                result += escapedBytes(std::string_view(chunk).substr(inChunk, copied));
                absolute += copied;
            }
            return result;
        }

        std::shared_ptr<Storage> storage;
        std::size_t beginOffset = 0;
        std::size_t endOffset = 0;
        std::size_t escapedPayloadBytes = 0;
    };

} // namespace ai::openai::codex::frontend::detail

#endif
