#include "../LegacyFrontendClient.h"

bool legacyClientOracleLinkProbe() {
    using Client = ai::openai::codex::frontend::legacy_client::Client;
    using Member = bool (Client::*)() const noexcept;
    static volatile Member member = &Client::isOpen;
    (void) member;
    return true;
}
