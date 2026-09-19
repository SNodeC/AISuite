/* SPDX-License-Identifier: LGPL-3.0-or-later OR MIT */
#ifndef AISUITE_CODEX_MODEL_PROVIDERPROFILE_H
#define AISUITE_CODEX_MODEL_PROVIDERPROFILE_H
#include "ai/model/Provider.h"
#include "ai/openai/codex/provider/StdioAppServer.h"
namespace ai::openai::codex::model {
    // Owns the temporary, credential-free catalog for an AISuite provider. Codex
    // remains the conversation authority; this object stores no inference state.
    class ProviderProfile {
    public:
        ProviderProfile(const ai::model::ProviderInfo& provider, std::uint64_t contextWindow);
        ~ProviderProfile();
        ProviderProfile(const ProviderProfile&) = delete;
        ProviderProfile& operator=(const ProviderProfile&) = delete;
        void configure(provider::StdioAppServerOptions& options,
                       const std::string& model,
                       const std::string& baseUrl,
                       const std::string& localToken) const;
        const std::string& catalogPath() const noexcept;
        static nlohmann::json catalog(const ai::model::ProviderInfo& provider, std::uint64_t contextWindow);

    private:
        ai::model::ProviderInfo provider_;
        std::string path_;
    };
} // namespace ai::openai::codex::model
#endif
