# Final A1a protocol completion

The stable Codex A1 typed protocol surface is complete. Final A1a completes
exactly the Common/A1.0 identities `initialize`, `initialized`, and `error` for
the vendored Codex App Server 0.144.6 protocol.

## Initialization

`typed::InitializeParams` contains required canonical client information and
three-state optional-nullable capabilities. Client title and the root
capabilities value preserve omission, explicit null, and a value.
`InitializeCapabilities` preserves omitted, false, and true independently for
`experimentalApi`, `mcpServerOpenaiFormElicitation`, and
`requestAttestation`; `optOutNotificationMethods` preserves omission, null, an
empty array, and a populated array. All three open objects retain future raw
properties, while known typed fields remain authoritative when encoded.

Existing `ClientInfo` constructors remain source-compatible and map their
title to a present string with capabilities omitted. Applications that need
the complete schema pass `typed::InitializeParams` to the corresponding
`stdio::Client` constructor. The handshake remains automatic and asynchronous.

The initialize response retains all four required values:

- strong `AbsolutePath` `codexHome`;
- `platformFamily`;
- `platformOs`;
- `userAgent`.

`getInitializeResponse()` exposes the canonical value and retains the complete
raw response object. A malformed result does not transition the client to
Ready. Final A1b subsequently removed the legacy `InitializeResult` projection,
leaving `decodeInitializeResponse(...)` as the single decoder.

After a matching valid response, the client enqueues exactly:

```json
{"method":"initialized"}
```

There is no `params` member. Ready follows successful enqueue, and queued
pre-ready traffic is then delivered in order.

## Canonical error notification

The canonical stable value is:

```cpp
struct ErrorNotification {
    TurnError error;
    ThreadId threadId;
    TurnId turnId;
    bool willRetry;
    Json raw;
    std::vector<DecodeDiagnostic> diagnostics;
};
```

It is `CanonicalServerNotification` alternative 67, making that variant size
68. The application-facing `Event` remains size 69: `TurnErrorEvent` remains
alternative 44, `UnknownEvent` remains 45, and every A1.4c alternative remains
at 61 through 68. `TurnErrorEvent::canonical` exposes the complete canonical
notification without changing its existing fields or backend/frontend
projection.

## Current state and boundary

The production registry is 339 Complete / 0 Partial / 0 NotImplemented / 48
NotApplicable. InventoryOnly remains unchanged. Final A1b leaves that protocol
state untouched while moving the Codex libraries to SOVERSION 2 and removing
only its frozen compatibility list. Final A1a introduced no façade redesign and
no second transport, pending map, event loop, notification path, or handshake
state machine.
