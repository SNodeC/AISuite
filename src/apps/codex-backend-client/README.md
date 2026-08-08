# Codex backend reference client

`codex-backend-client` is the reference terminal application for
`AISuite::OpenAICodexFrontendClient`. The SDK owns Hello, authentication
placement, request IDs, response correlation, synchronization, replay cursors,
sparse sequence handling, typed state, and operation completion. The
application owns command parsing, presentation, workflows, and physical
SNode.C transports. It never connects to the Codex App Server directly.

Start the backend in one terminal and the client in another:

```sh
codex-backend
codex-backend-client
```

The client connects to
`$XDG_RUNTIME_DIR/snodec-codex-backend.sock` when `XDG_RUNTIME_DIR` is set and
nonempty. Otherwise it uses
`/tmp/snodec-codex-backend-<numeric-uid>.sock`.

Unix is the only transport enabled by default. Its ordinary SNode.C
remote-address option overrides that default:

```sh
codex-backend-client codex-backend-client-unix remote \
  --sun-path /run/user/1000/my-codex-backend.sock
```

The executable also composes disabled-by-default named SNode.C clients for
IPv4/IPv6 JSONL, IPv4/IPv6 TLS JSONL, RFCOMM JSONL, RFCOMM TLS JSONL,
WebSocket, and WSS when those features are compiled. Enable and configure
exactly one named outgoing instance through SNode.C's native configuration.
WebSocket and WSS use SNode.C framing and request the exact `codex`
subprotocol. The SDK has no transport registry.

Remote transports require a protected bearer-token file:

```sh
install -m 600 /dev/null "$XDG_CONFIG_HOME/aisuite/codex.token"
# Write the token without placing it in process arguments or configuration dumps.
codex-backend-client --bearer-token-file "$XDG_CONFIG_HOME/aisuite/codex.token" \
  codex-backend-client-ipv4 --disabled=false remote --host 127.0.0.1
```

The file must satisfy the same owner, regular-file, no-symlink, and permission
checks as the backend reference authentication policy. Verified local Unix use
may use `NoCredential` and a continuity key derived from the effective UID.
The SDK itself neither knows a token-file path nor persists credentials.

The SDK sends Hello automatically only after the physical transport reports
connected. Interactive commands are:

```text
help
quit
reconnect
snapshot
replay <sequence>
acquire
release
threads
start [--cwd <path>] [--model <model>]
      [--model-provider <provider>]
      [--approval-policy <policy>]
      [--sandbox-mode <mode>]
      [--ephemeral]
resume <thread-id>
       [--cwd <path>] [--model <model>]
       [--model-provider <provider>]
       [--approval-policy <policy>]
       [--sandbox-mode <mode>]
new [thread-start-options] -- <prompt>
new <prompt>
read <thread-id>
turn <thread-id> <prompt>
interrupt <thread-id> <turn-id>
raw <json>
watch on
watch off
```

The application distinguishes command, physical-connection, and process
lifetimes. Unknown/malformed local input, a pre-acceptance command-local SDK
rejection, or a normal failed command response ends only that input command. A
transport/send rejection remains connection-level; temporary pending-capacity
and active-synchronization conditions are bounded deferrals. A command-local
failure does not stop
stdin, close a valid transport, or terminate the event loop. A protocol/state
failure or transport loss still closes that physical attachment and fails its
accepted operations exactly once, but the interactive application remains
running in `Disconnected` with stale retained State.

While Disconnected, `help`, `watch`, `reconnect`, and `quit` remain available.
Remote commands are rejected locally with reconnect guidance and are not saved
for later surprise execution. `reconnect` creates one new physical attempt
using the selected configured transport and the same SDK Client. Repeated
attempts are explicit: there is no automatic reconnect, no command retry, and
no automatic controller reacquisition. `reconnect` does not overlap an active
connection attempt or disrupt an already Ready connection.

Each physical attempt carries one immutable generation. Native factories
capture that generation, while WebSocket/WSS upgrade callbacks additionally
bind it to the exact originating socket connection. Late HTTP or subprotocol
callbacks from a retired connection cannot claim or retire a later attempt.

The application-owned command queue has finite configurable limits:

```sh
codex-backend-client \
  --maximum-queued-commands 256 \
  --maximum-queued-command-bytes 16777216
```

Those are the defaults. Zero means zero queue capacity. The byte limit covers
retained command-owned UTF-8 input; checked arithmetic rejects the newest
overflowing command without evicting older entries. Connecting and
Synchronizing may queue remote input, and Ready may temporarily queue a first
submission blocked by pending capacity or active synchronization. Disconnected
never queues remote input. One queued compound `new` counts as one command.

Normal commands use SDK submissions. `raw` accepts only a known one-of-105
generated command, validates it through generated schema authority, discards
caller request IDs, and uses normal SDK correlation. It cannot send Hello,
unknown methods, or raw App Server messages. `watch` is local: disabling it
suppresses event-batch presentation but does not change backend state or
synchronization.

## Thread lifecycle

Every connection begins as an observer. The client never acquires controller
ownership automatically. Run `acquire` before `start`, `resume`, `new`, or
`turn`; otherwise the backend reports its normal `permission_denied` response.
`release` gives up that ownership.

To create a thread and submit its first turn explicitly, use the thread ID
reported by `start`:

```text
acquire
start --cwd /home/voc/projects/snode.c
turn <returned-thread-id> Review the repository.
```

`start` maps directly to Frontend Protocol v1 `thread.start`. Its options are
optional and remain unset when omitted, allowing Codex defaults to apply. A
successful human-readable response identifies the returned thread ID.

To continue an existing persisted thread:

```text
acquire
threads
resume <thread-id>
turn <thread-id> Continue the previous task.
```

`threads` can report a persisted thread whose completeness is `notLoaded`.
`resume` maps directly to `thread.resume` and loads that thread into the running
Codex App Server before a later `turn` command. It accepts the same overrides as
`start` except `--ephemeral`, which is not part of `thread.resume`.

> `read` retrieves thread data but does not resume or load the thread into the
> running Codex App Server. Use `resume <thread-id>` before starting a turn on a
> persisted `notLoaded` thread.

For the common create-and-prompt workflow, `new` performs both client-side
steps:

```text
acquire
new --cwd /home/voc/projects/snode.c -- Review the repository.
```

With no thread-start options, the separator may be omitted:

```text
acquire
new Explain the current repository architecture.
```

When options are present, `--` ends option parsing and everything after it is
the prompt, including words that begin with `--`. `new` is implemented only by
`codex-backend-client`: it sends `thread.start`, waits for that request's
successful typed result, carries its validated `ThreadId`, and then sends
typed `turn.start` with one text input containing the complete prompt. It does
not add a backend command or Frontend Protocol method, and it does not
establish implicit current-thread state.

If thread creation succeeds but the initial turn cannot be submitted or later
fails, the diagnostic preserves the created thread ID and reports the compound
operation as failed. The successful thread creation is not rolled back and the
turn is not retried. One explicit active workflow is separate from later queued
`new` commands, so a failure clears only the active workflow and later input
continues in order. A failure before thread creation likewise ends only that
workflow.

Human mode reports when the connection is waiting for its initial
synchronization and when commands are ready. Commands entered during that
handshake are acknowledged as queued and remain behind the `sync.complete`
barrier.

After Ready, the backend may send a bounded live Snapshot barrier when one
atomic event occurrence cannot fit a batch. The SDK applies it as one
authoritative State replacement without leaving Ready, canceling pending
commands, or fabricating another synchronization completion. Expanded
snapshots and `threadList.updated` events carry real thread-list page,
completeness, cursor, and freshness state, so the CLI no longer reports that
metadata as unknown after expanded synchronization.

Expanded entity events preserve exact canonical identity. In the original
live failure, a `threads` response contained 25 distinct IDs but all 25
`thread.upserted` lines named one retained tail thread. Projection had missed
`data.thread.id` and substituted an unrelated last entity. The corrected
stream carries every returned page ID exactly once with its own title/preview,
preserves unrelated retained threads, and emits one compact
`threadList.updated`. Equivalent exact-ID rules cover turns, items, processes,
filesystem watches, fuzzy searches, activities, and the triggering notice
occurrence; an unresolved identity selects bounded Snapshot fallback rather
than another entity.

`thread/deleted` maps to `thread.removed`. Exactly five accumulated-content
notifications map to `item.content.updated`: agent-message delta,
command-execution output delta, file-change output delta, reasoning-summary
text delta, and reasoning text delta. Other item lifecycle, progress, plan, and
summary-part notifications map to `item.upserted`.

Human-readable output is the default. It gives concise stage summaries for
started and resumed threads and for both stages of `new`, rather than dumping
complete snapshots.

`--json` writes each decoded server message as one compact protocol JSON object
on stdout; connection notices, local command errors, and other diagnostics
remain on stderr so stdout can be consumed by scripts. For `new`, stdout
contains the complete real responses and events for its underlying
`thread.start` and `turn.start` requests. There is no synthetic `new` protocol
message and the original protocol messages are not hidden or rewritten.

Stdin and the Unix socket are both integrated with the SNode.C event loop.
Terminal and pipe input is nonblocking and line-buffered; regular-file stdin is
rejected because POSIX cannot make those reads nonblocking (pipe the file's
contents instead). Socket JSONL framing tolerates both fragmented records and
several records in one read.

Piped commands are retained by the CLI until the SDK becomes Ready after the
initial `sync.complete`, then submitted once in input order. EOF enters a
deterministic accumulating drain using SDK pending-operation counts and
callbacks: local parse errors and ordinary failed responses mark the final
status but do not stop later queued commands. The client waits until the queue,
accepted operations, active `new`, and explicit snapshot/replay synchronization
are all terminal before disconnecting and exiting. All-success input exits
zero; any line/command failure exits nonzero after the complete drain. If the
connection is lost, unsubmitted entries are explicitly accounted as failed and
discarded, so drain finishes nonzero without waiting for manual reconnect. No
accepted request is retried. An explicit interactive `quit` remains an
immediate successful shutdown even after earlier interactive command failures.

The first concrete typed SDK or transport failure is retained and presented;
a later socket detach does not overwrite it with a generic unexpected-close
message. Human diagnostics remain on the diagnostic stream and JSON mode keeps
stdout protocol-only. `quit`, Ctrl-C, and the SNode.C application-shutdown
signal are classified as intentional before transport detachment. Remote EOF
while the application is otherwise running remains an error. Nonblocking
terminal input continues to distinguish `EAGAIN` from real `read() == 0` EOF.
The registered stdin receiver publishes the framework shutdown notification
before any pending connect or WebSocket-upgrade failure callback, covering the
short interval before a transport context exists without synthesizing EOF.

For example, this waits for the handshake and both command responses before
returning:

```sh
printf 'acquire\nthreads\n' | codex-backend-client --json
```

This piped convenience workflow likewise waits for the generated thread ID to
be handed from `thread.start` to `turn.start` before returning:

```sh
printf '%s\n' \
  'acquire' \
  'new --cwd /home/voc/projects/snode.c -- Review the repository.' \
  | codex-backend-client --json
```
