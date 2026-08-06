/*
 * SPDX-License-Identifier: LGPL-3.0-or-later OR MIT
 */

#include "ai/openai/codex/frontend/client/ProjectionFingerprint.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace ai::openai::codex::frontend::client {
    namespace {
        template <typename T, typename Identity>
        frontend::Json canonicalSet(const std::vector<T>& values, Identity identity) {
            std::vector<std::string> strings;
            strings.reserve(values.size());
            for (const T value : values) {
                strings.emplace_back(identity(value));
            }
            std::sort(strings.begin(), strings.end());
            strings.erase(std::unique(strings.begin(), strings.end()), strings.end());
            frontend::Json canonical = frontend::Json::array();
            for (std::string& value : strings) {
                canonical.push_back(std::move(value));
            }
            return canonical;
        }

        template <typename T, typename Identity>
        frontend::Json canonicalOptionalSet(const std::optional<std::vector<T>>& values, Identity identity) {
            frontend::Json canonical = frontend::Json::object();
            if (!values) {
                canonical["present"] = false;
                return canonical;
            }
            canonical["present"] = true;
            canonical["values"] = canonicalSet(*values, identity);
            return canonical;
        }

        template <typename Enum>
        std::string invalidIdentity(Enum value) {
            return "invalid:" + std::to_string(static_cast<std::underlying_type_t<Enum>>(value));
        }
    } // namespace

    std::string projectionFingerprint(const ProjectionFingerprintInput& input) {
        const auto capabilityIdentity = [](frontend::FrontendCapability capability) {
            const std::string_view identity = frontend::toString(capability);
            return identity.empty() ? invalidIdentity(capability) : std::string(identity);
        };
        const auto methodIdentity = [](frontend::generated::MethodId method) {
            const std::string_view identity = frontend::generated::methodString(method);
            return identity.empty() ? invalidIdentity(method) : std::string(identity);
        };
        const auto scopeIdentity = [](frontend::FrontendScope scope) {
            const std::string_view identity = frontend::toString(scope);
            return identity.empty() ? invalidIdentity(scope) : std::string(identity);
        };

        frontend::Json continuityKey = frontend::Json::object();
        continuityKey["present"] = input.continuityKey.has_value();
        if (input.continuityKey) {
            continuityKey["value"] = *input.continuityKey;
        }

        frontend::Json projectionMetadata = frontend::Json::object();
        projectionMetadata["present"] = input.explicitProjectionMetadata.has_value();
        if (input.explicitProjectionMetadata) {
            projectionMetadata["value"] = *input.explicitProjectionMetadata;
        }

        frontend::Json canonical = frontend::Json::object();
        canonical["format"] = "snodec.codex-frontend.projection-fingerprint.v1";
        canonical["requestedRepresentationCapabilities"] = canonicalSet(input.requestedRepresentationCapabilities, capabilityIdentity);
        canonical["selectedRepresentationCapabilities"] = canonicalSet(input.selectedRepresentationCapabilities, capabilityIdentity);
        canonical["continuityKey"] = std::move(continuityKey);
        canonical["permittedScopes"] = canonicalOptionalSet(input.permittedScopes, scopeIdentity);
        canonical["permittedMethods"] = canonicalOptionalSet(input.permittedMethods, methodIdentity);
        canonical["availableMethods"] = canonicalOptionalSet(input.availableMethods, methodIdentity);
        canonical["explicitProjectionMetadata"] = std::move(projectionMetadata);
        return canonical.dump();
    }

} // namespace ai::openai::codex::frontend::client
