# P3 live transport and authentication evidence

This report records direct live acceptance performed on 2026-08-13 against
AISuite commit `61d6db0daaf6dde6653ae5f534ec1ce8b799ea6c` on branch
`codex/p3-frontend-cutover`. It supplements the focused automated P3 evidence
with observations from the real `codex-backend`, `codex-backend-client`, and
locally installed `codex app-server` processes.

The worktree was clean before and after testing. No production source,
configuration, generated authority, test, SNode.C dependency, or installed
file was modified during the investigation.

## Test environment and method

- Host platform: Linux with IPv4 and IPv6 loopback connectivity.
- Bluetooth: one active `hci0` controller at `44:38:E8:4D:38:C9`; no second or
  virtual HCI controller was available.
- Accepted SNode.C source inspected: commit
  `6ae8fafcd50052a9d86932b0be721ef39cce7a44` on SNode.C `master`.
- A private temporary directory with mode `0700` held test credentials.
- Bearer-token files used mode `0600`.
- A temporary CA and server certificate were generated with OpenSSL. The
  server certificate contained SAN entries for `localhost`, `127.0.0.1`, and
  `::1`.
- TLS/WSS positive tests explicitly configured the generated CA and SNI
  `localhost`; certificate acceptance was not bypassed.
- Each successful frontend composition had to reach physical connection,
  `Welcome`, `Snapshot`, `sync.complete`, and a successful `threads` response.
- All isolated test listeners and their `codex app-server` children were
  stopped after use. Temporary tokens, private keys, certificates, and probe
  output were removed. Pre-existing user processes were left untouched.

Representative client selection used the production CLI hierarchy. For
example, an IPv6 WebSocket client was selected with:

```text
codex-backend-client \
  --bearer-token-file <protected-token-file> \
  codex-backend-client-unix --disabled=true \
  codex-backend-client-websocket-ipv6 --disabled=false \
  remote --host <host> --port <port>
```

TLS and WSS variants additionally used:

```text
tls --ca-cert <generated-ca-certificate> --sni localhost
```

## Live transport matrix

| Transport composition | Result | Evidence and qualification |
|---|---|---|
| Unix JSONL | PASS | Verified same-user local trust, Welcome, Snapshot, synchronization, and `threads` all succeeded without a bearer token. |
| IPv4 JSONL | PASS | Correct bearer authenticated; synchronization and `threads` succeeded. |
| IPv6 JSONL | PASS | Correct bearer authenticated over `::1`; synchronization and `threads` succeeded. |
| IPv4 TLS JSONL | PASS | Generated CA validation and SNI succeeded; synchronization and `threads` succeeded. |
| IPv6 TLS JSONL | PASS | Generated CA validation and SNI succeeded over `::1`; synchronization and `threads` succeeded. |
| RFCOMM JSONL | ENVIRONMENT BLOCKED | The server listener bound successfully to `44:38:E8:4D:38:C9`, channel 23. Linux rejected a client connection back to the same sole adapter with `EHOSTUNREACH` (`No route to host`). |
| RFCOMM TLS JSONL | ENVIRONMENT BLOCKED | The TLS listener bound successfully to the same adapter, channel 24. The physical connection was rejected with the same `EHOSTUNREACH` before TLS could begin. |
| WebSocket IPv4 | PASS | HTTP upgrade selected subprotocol `codex`; synchronization and `threads` succeeded. |
| WebSocket IPv6 | CONDITIONAL PASS | `localhost` and an explicit bracketed HTTP Host authority succeeded. The production default with raw `--host ::1` failed because it emitted an invalid unbracketed HTTP Host authority. |
| WSS IPv4 | PASS | Generated CA validation, WebSocket upgrade, synchronization, and `threads` succeeded. |
| WSS IPv6 | CONDITIONAL PASS | `localhost` and an explicit bracketed HTTP Host authority succeeded with CA validation. Raw/default `::1` has the same Host-authority defect as plaintext WebSocket IPv6. |

The two RFCOMM compositions were therefore proven to configure and bind, but
could not receive an end-to-end result on this host. A second physical or
virtual Bluetooth controller is required to distinguish the remote endpoint;
no TCP substitute can validate RFCOMM semantics.

## Authentication and security matrix

The following scenarios produced the expected result:

| Scenario | Result | Observed behavior |
|---|---|---|
| Verified Unix same-user trust, no bearer | PASS | Connection synchronized and `threads` succeeded. |
| Verified Unix trust with an incorrect supplied bearer | PASS | Verified local identity took precedence; synchronization succeeded. |
| Unix verified trust disabled, no bearer | PASS | Server returned `authentication_required` and closed the connection. |
| Unix verified trust disabled, correct bearer | PASS | Bearer authentication and synchronization succeeded. |
| Explicit insecure Unix local-trust override | PASS | Connection succeeded without a bearer and the backend emitted the required prominent warning. |
| Remote client with no bearer configuration | PASS | Client credential preparation failed before Hello; no session was admitted. |
| Remote client with wrong bearer | PASS | Server returned `authentication_failed`; no Welcome was issued. |
| Correct bearer after a failed attempt | PASS | A subsequent connection synchronized normally. |
| Failed-authentication rate limiting | PASS | The first three wrong bearers returned `authentication_failed`; the fourth normalized-peer attempt returned `rate_limited` under the default three-per-60-second policy. |
| Bearer file grants group/other access | PASS | Backend rejected startup with `bearer-token file grants group or other access`. |
| Bearer file is a symbolic link | PASS | Backend rejected the file before starting. |
| Explicitly trusted generated CA | PASS | TLS and WSS handshakes succeeded. |
| Explicit unrelated CA | PASS | TLS handshake failed with certificate verification failure; no frontend synchronization occurred. |
| Non-loopback plaintext bind, default policy | PASS | Backend rejected the bind. |
| Non-loopback plaintext bind with explicit insecure-remote override and bearer | PASS | Listener and authenticated client succeeded. |
| WebSocket `Authorization` credential channel | PASS | HTTP 400 `websocket_credential_channel_rejected`. |
| WebSocket `Proxy-Authorization` credential channel | PASS | HTTP 400 `websocket_credential_channel_rejected`. |
| WebSocket `Cookie` credential channel | PASS | HTTP 400 `websocket_credential_channel_rejected`. |
| WebSocket query credential channel | PASS | HTTP 400 `websocket_credential_channel_rejected`. |
| WebSocket unapproved Origin | PASS | HTTP 403 `origin_rejected`. |
| WebSocket same-origin Origin | PASS | HTTP 101 Switching Protocols. |
| WebSocket explicitly allowlisted Origin | PASS | HTTP 101 Switching Protocols. |
| Wrong bearer after WebSocket upgrade | PASS | Frontend Protocol returned `authentication_failed`. |
| Correct bearer after WebSocket authentication failure | PASS | A subsequent upgrade and synchronization succeeded. |

### TLS verification caveat

The accepted SNode.C TLS implementation enables peer verification when a CA
file, CA directory, or default CA store is configured. With none of those
configured, its current behavior does not request peer verification even
though `--ca-cert-accept-unknown` defaults to false. Deployment configurations
that require authenticated TLS must therefore configure an explicit CA source.
The positive tests above did so; the negative test used an explicit unrelated
CA and observed the expected verification failure.

## IPv6 WebSocket investigation

The raw/default IPv6 WebSocket failure was isolated to HTTP authority
formatting. It is not an IPv6 TCP, WebSocket framing, static subprotocol,
Origin-policy, bearer-authentication, or TLS failure.

### Exact wire result

With the production client configured as `remote --host ::1 --port 45876`, TCP
connected and the SNode.C HTTP client sent:

```http
GET /frontend HTTP/1.1
Host: ::1:45876
```

The AISuite server returned:

```http
HTTP/1.1 400 Bad Request

invalid_websocket_upgrade
```

Using the same IPv6 socket destination with the existing public HTTP policy
override:

```text
http --host '[::1]:45876'
```

sent the valid authority:

```http
Host: [::1]:45876
```

and received HTTP 101 Switching Protocols. A raw HTTP probe with the bracketed
Host also received 101. Connecting to the same listener using `localhost`
sent `Host: localhost:45876` and likewise received 101.

### Source trace

SNode.C's HTTP client derives its default Host field from:

```cpp
socketConnection->getConfig()->Remote::getSocketAddress().toString(false)
```

The SNode.C IPv6 `SocketAddress::toString(false)` representation is currently
`host + ':' + port`; its own unit tests expect `::1:8080`. That generic display
representation is ambiguous when reused as an HTTP authority, where an IPv6
literal must be bracketed.

AISuite's static-composition WebSocket client repeats the same default in
`src/apps/codex-backend-client/FrontendWebSocketClient.h`. The generated Host
therefore becomes `::1:45876`.

The SNode.C server parser accepts that nonempty Host header. AISuite then builds
the request origin from the Host in `FrontendWebApplication.cpp` and validates
it through `normalizeWebOrigin()` in `FrontendWebSecurity.cpp`. The strict
parser accepts bracketed IPv6 and rejects an unbracketed colon-containing host,
returning HTTP 400. This fail-closed server behavior is correct.

The existing `CodexBackendClientWebSocketAcceptanceTest` already documents the
SNode.C limitation and explicitly sets `[IPv6]:port` for its IPv6 cases. Both
focused tests passed:

```text
CodexBackendClientWebSocketIpv6AcceptanceTest  Passed  0.22 sec
CodexBackendClientWssIpv6AcceptanceTest        Passed  1.31 sec
```

That override means the test verifies the transport once supplied a valid
Host, but does not exercise the production CLI's default Host derivation.

### Ownership and prospective repair boundary

The reusable root defect belongs to SNode.C's HTTP Host/authority generation:
a generic socket display string is being used without IPv6 authority
bracketing. Changing generic `SocketAddress::toString()` is not necessarily the
right repair because its representation is used outside HTTP and is explicitly
covered by SNode.C tests. A dedicated HTTP-authority formatter would avoid that
coupling.

AISuite also owns a local manifestation because its custom WebSocket HTTP
client duplicates the SNode.C default. A correction to SNode.C's standard HTTP
client alone would not change the copied AISuite line; AISuite would need to
consume the corrected helper or apply equivalent IPv6-aware formatting.
AISuite's server validation should not be weakened.

No matching open SNode.C issue or pull request was found during this
investigation. This document records evidence only and does not implement a
repair.

## Final state

- Nine non-Bluetooth transport compositions completed live end-to-end.
- Both RFCOMM listeners bound, but end-to-end connection was blocked by the
  single-adapter environment.
- All exercised authentication and WebSocket admission scenarios matched their
  expected security behavior.
- The IPv6 WebSocket default-host failure has an exact wire-level and
  source-level explanation.
- Test processes and ephemeral credentials were removed.
- Repository source and build configuration remained unchanged.
