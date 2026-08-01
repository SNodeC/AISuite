# Final A1b ABI transition

The stable Codex A1 typed protocol surface remains complete at 339 Complete,
zero Partial, zero NotImplemented, and 48 NotApplicable identities. Final A1b
changes no registry row, descriptor, wire codec, variant order, lifecycle, or
protocol behavior.

## Frozen compatibility removal

Final A1b removes only the approved deferred compatibility layer:

- the legacy `InitializeResult`, `getInitializeResult()`, and
  `decodeInitializeResult(...)` projection path; use
  `typed::InitializeResponse`, `getInitializeResponse()`, and
  `decodeInitializeResponse(...)`;
- `ThreadStartOptions`, `ThreadResumeOptions`, `ThreadListOptions`,
  `ThreadReadOptions`, their four conversion helpers, `ThreadResultHandler`,
  and the five deprecated Threads overloads; use `ThreadStartParams`,
  `ThreadResumeParams`, `ThreadListParams`, and `ThreadReadParams` with their
  operation-specific result handlers;
- `TurnInterruptResult`, `TurnStartOptions`, the two conversion helpers,
  `TurnResultHandler`, `InterruptResultHandler`, and the two deprecated Turns
  overloads; use `TurnStartParams`, `TurnInterruptParams`,
  `TurnStartResultHandler`, and `UnitResultHandler`;
- `WorkspaceWriteSandboxPolicy::fromLegacy(...)`; construct
  `WorkspaceWriteSandboxPolicy` directly with `AbsolutePathBuf` roots;
- the inconsistent `OptionalNullable(bool, std::optional<T>)` constructor; use
  `omitted()`, `explicitNull()`, or `withValue(...)`; and
- the four optional string `decodingError` fields on `UnknownItem`,
  `UnknownResponseItem`, `UnknownEvent`, and `UnknownServerRequest`; use their
  structured `DecodeDiagnostic` and preserved raw JSON.

Nothing outside that frozen list is removed. In particular, the grouped
`client.typed()` path and deprecated direct Threads, Turns, Events, and Requests
accessors remain available. `ClientInfo` construction, item and turn-input alias
families, reverse-request projections, application Event projections, context
fields, implicit path conversions, raw protocol access, unknown alternatives,
and stable protocol-deprecated identities remain for the separate A1.5 review.

## SOVERSION 2

The shared `AISUITE_CODEX_SOVERSION` setting changes from 1 to 2 for all three
libraries:

- `libaisuite-openai-codex.so.2`;
- `libaisuite-openai-codex-backend.so.2`; and
- `libaisuite-openai-codex-frontend.so.2`.

No `.so.1` compatibility library is installed or packaged. Project version,
output names, imported targets (`AISuite::OpenAICodex`,
`AISuite::OpenAICodexBackend`, and `AISuite::OpenAICodexFrontend`), public
include paths, and package components remain unchanged. Raw and unknown-value
forward compatibility remains supported.

Final A1b is the ABI transition only. A1.5 remains responsible for the later
application façade and vocabulary decisions.
