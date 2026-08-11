#include "../LegacyFrontendService.h"

bool legacyServerOracleLinkProbe() {
    using Service = ai::openai::codex::frontend::oracle::FrontendService;
    using Member = ai::openai::codex::frontend::SequenceNumber (Service::*)() const noexcept;
    static volatile Member member = &Service::currentSequence;
    (void) member;
    return true;
}
