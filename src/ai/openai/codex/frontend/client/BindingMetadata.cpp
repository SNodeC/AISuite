#include "ai/openai/codex/frontend/client/GeneratedBindings.h"

namespace ai::openai::codex::frontend::client::generated {

    static_assert(AllBindings.size() == frontend::generated::MethodCount);
    static_assert(NativeBindingCount == 7);
    static_assert(ProviderBindingCount == 86);
    static_assert(ReverseBindingCount == 12);
    static_assert(RequestsBindingCount == ReverseBindingCount);
    static_assert(bindingMetadata(frontend::generated::MethodId::ControllerAcquire)->method ==
                  frontend::generated::MethodId::ControllerAcquire);
    static_assert(bindingMetadata(frontend::generated::MethodId::ThreadStart)->method ==
                  frontend::generated::MethodId::ThreadStart);
    static_assert(bindingMetadata(frontend::generated::MethodId::PermissionsApprovalRespond)->method ==
                  frontend::generated::MethodId::PermissionsApprovalRespond);
    static_assert(bindingIsSensitive(frontend::generated::MethodId::AuthenticationRespond));
    static_assert(bindingIsSensitive(frontend::generated::MethodId::AccountLoginStart));
    static_assert(!bindingIsSensitive(frontend::generated::MethodId::ThreadRead));

} // namespace ai::openai::codex::frontend::client::generated
