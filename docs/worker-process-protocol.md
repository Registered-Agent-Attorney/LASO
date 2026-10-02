# Local worker process protocol

LASO supports a versioned, local-only worker-host protocol for adapters that
must not run inside the LASO server process. Version 1 uses newline-delimited
JSON (NDJSON) over the child's stdin and stdout. The newline is the frame
boundary; each request receives exactly one response, and unsolicited messages
are rejected by the host transport. A worker may send one correlated
`worker_request` while LASO is waiting for a response; the host answers it with
one `worker_response` before continuing the original operation. The Codex
adapter may also send the single fixed `worker_tool_call` described below. No
other unsolicited message type is accepted.

## Request

Every request is an object with the following fields:

```json
{
  "protocol_version": 1,
  "request_id": "req-1",
  "operation": "submit",
  "job_id": "worker-123",
  "external_job_id": "",
  "payload": {"timeout_ms": 30000}
}
```

`operation` is one of `hello`, `submit`, `status`, `result`, `cancel`, or
`shutdown`. `job_id` is the durable LASO job identifier. `external_job_id` is
required for operations that address an already-submitted external job.
Payloads are structured JSON and are bounded by the transport.

The `submit` payload may also contain `durable_session`,
`durable_session_id`, `continuation`, and `session_context`. These optional
fields are used only when LASO executes an agent worker inside a durable
session. `durable_session_id` is LASO's canonical session identifier and is
provided so worker-interaction records can refer to the LASO session without
exposing provider-native identifiers. `continuation` has provider ID, adapter
version, and an opaque state string; `session_context` is the immutable
provider-neutral context snapshot selected when LASO bound the run. A
continuation response uses the same three fields. Workers must not publish
continuation through the result or metadata fields. LASO stores it privately
and returns it only to the adapter on a later authoritative turn.

`timeout_ms` is an optional positive integer execution budget in milliseconds.
Core sends the remaining run budget capped by the process worker's configured
request timeout. Workers should stop or discard the result after that budget
expires. Omission or zero means the worker uses its own bounded default; the
transport request itself remains bounded independently by Core.

## Worker-originated requests

The request channel is deliberately narrower than RPC. Supported request types
are `approval`, `permission`, and `question`:

```json
{
  "protocol_version": 1,
  "message_type": "worker_request",
  "request_id": "request-17",
  "worker_job_id": "worker-123",
  "worker_id": "example",
  "external_job_id": "child-123",
  "session_id": "session-1",
  "request_type": "permission",
  "title": "Run local test",
  "summary": "The worker requests permission to run a command",
  "payload": {"resource": "project.tests", "command": "..."},
  "created_at": "2026-09-17T12:00:00Z",
  "deadline": "",
  "risk": "medium",
  "category": "tool"
}
```

LASO evaluates permission/approval requests through its configured policy and
otherwise stores a durable pending decision. Questions require a human answer.
The response is:

```json
{"protocol_version":1,"message_type":"worker_response",
 "request_id":"request-17","decision":"approved","payload":{},"reason":"..."}
```

Durable interaction states are `pending`, `approved`, `denied`, `answered`,
`cancelled`, and `expired`. Duplicate request IDs replay the existing decision
only when the job and request type match. Restart never auto-approves a pending
request, and cancelling the owning job cancels its pending requests. The
configured interaction timeout is bounded; a timeout becomes `expired`.
Workers cannot call arbitrary LASO methods or submit pipelines through this
channel.

### Fixed Codex dynamic tool

The Codex adapter may forward one app-server dynamic tool request using a
separate message type:

```json
{"protocol_version":1,"message_type":"worker_tool_call",
 "request_id":"call-1","worker_job_id":"worker-123","worker_id":"agent-one",
 "external_job_id":"codex:thread-1","session_id":"thread-1",
 "turn_id":"turn-1","deadline":"","namespace":"laso",
 "tool":"browser_status","arguments":{}}
```

Core accepts only `namespace: "laso"`, `tool: "browser_status"`, and an empty
object for `arguments`. It maps the request to the configured
`windows_computer` worker and `browser.status` capability, applies worker
policy, persists a child job linked to the parent Codex job, and returns only
that child result. Other names, arguments, and worker targets fail closed. The
correlated response is:

```json
{"protocol_version":1,"message_type":"worker_tool_response",
 "request_id":"call-1","success":true,
 "result":{"window_count":1,"browser_status":{"browser_visible":true,
 "active_browser_visible":true}},"error":""}
```

Parent cancellation or deadline expiry requests cancellation of the linked
Computer child. This channel does not expose generic worker dispatch or
command execution.

`Unknown` means the adapter cannot establish the job's outcome. Core persists
that state and does not resubmit the same durable job. If Core restarts while
an asynchronous submit has no recorded external job ID, it also records
`Unknown` rather than guessing that submission never reached the child.

## Response

```json
{
  "protocol_version": 1,
  "request_id": "req-1",
  "ok": true,
  "state": "Completed",
  "external_job_id": "child-123",
  "payload": {"ok": true},
  "metadata": {},
  "continuation": null,
  "artifacts": [],
  "usage": {"executor": "reference"},
  "error": ""
}
```

`ok: false` is a transport/protocol error and is not a worker-declared job
failure. A response with `ok: true` and `state: Failed` is a worker-declared
job failure. `state` uses LASO's normalized worker states. `usage` is optional
and uses the backend-neutral `WorkerUsage` fields. Artifact entries are
metadata/reference objects only; the protocol does not authorize arbitrary
filesystem paths.

The transport enforces a 1 MiB maximum frame, 64 KiB maximum captured stderr,
64 KiB metadata, 16 artifact references, one outstanding request, 4 KiB
interaction text fields, a 512-byte interaction identity, 64 KiB interaction
payload, and bounded startup/request/interaction timeouts from configuration.
Malformed, oversized, truncated, unexpected, or version-incompatible frames are
transport failures.

## Process boundary

LASO executes the configured absolute executable directly with an argument
vector; it never interpolates a shell command. The child receives an empty
environment by default, plus explicitly configured overrides and values named
by an environment allowlist. Explicit overrides win when a name is present in
both sources. Configuration rejects NUL characters, duplicate allowlist names,
and oversized values; the total child environment is bounded to 64 KiB at
launch. This is process isolation and lifecycle supervision, not an
OS/container sandbox. The child can affect LASO only by returning protocol
messages.
