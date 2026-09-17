# Local worker process protocol

LASO supports a versioned, local-only worker-host protocol for adapters that
must not run inside the LASO server process. Version 1 uses newline-delimited
JSON (NDJSON) over the child's stdin and stdout. The newline is the frame
boundary; each request receives exactly one response, and unsolicited messages
are rejected by the host transport.

## Request

Every request is an object with the following fields:

```json
{
  "protocol_version": 1,
  "request_id": "req-1",
  "operation": "submit",
  "job_id": "worker-123",
  "external_job_id": "",
  "payload": {}
}
```

`operation` is one of `hello`, `submit`, `status`, `result`, `cancel`, or
`shutdown`. `job_id` is the durable LASO job identifier. `external_job_id` is
required for operations that address an already-submitted external job.
Payloads are structured JSON and are bounded by the transport.

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
64 KiB metadata, 16 artifact references, one outstanding request, and bounded
startup/request timeouts from configuration. Malformed, oversized, truncated,
unexpected, or version-incompatible frames are transport failures.

## Process boundary

LASO executes the configured absolute executable directly with an argument
vector; it never interpolates a shell command. The child receives an empty
environment by default, plus explicitly configured overrides and values named
by an environment allowlist. This is process isolation and lifecycle
supervision, not an OS/container sandbox. The child can affect LASO only by
returning protocol messages.
