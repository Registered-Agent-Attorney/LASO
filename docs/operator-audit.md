# Operator audit

LASO stores an append-only audit feed in PostgreSQL. Every LASO instance using
the same configured database schema can read the same records. Schema version
12 creates the audit table; migrations run during storage initialization.

## Read API

GET /api/v1/operator/audit?limit=50&before=123

The endpoint requires an authenticated operator or admin principal. It returns
the newest events first:

    {
      "events": [{
        "sequence": 124,
        "event_id": "...",
        "operation_id": "...",
        "actor": "operator-42",
        "role": "operator",
        "action": "maintenance.drain",
        "target_type": "instance",
        "target_id": "...",
        "outcome": "succeeded",
        "http_status": 200,
        "occurred_at": "2026-09-29T12:00:00.000Z"
      }],
      "next_before": null,
      "has_more": false
    }

Limit is an integer from 1 through 100 and defaults to 50. Before is an
exclusive positive sequence cursor. Use the last event sequence as the next
request's before value when has_more is true. Offset and after pagination are
not supported. Invalid, repeated, oversized, or unsupported parameters return
a stable 400 INVALID_REQUEST. Responses contain at most 100 events.

## Recorded operations

The API records operator/admin requests for maintenance transitions, session
closure, worker-job cancellation, worker request decisions, run cancellation
and resume, approval decisions, event-source enable/disable, schedule/trigger
changes, and pipeline creation/run requests. Each operation produces a
requested record and, when the result can be recorded, a succeeded, rejected,
or failed record sharing one operation ID. Target IDs are included only when
they match LASO's bounded identifier syntax. The instance ID is the target for
maintenance transitions.

The audit schema and response intentionally omit request bodies, prompts,
message content, comments, credentials, tokens, DSNs, environment values,
filesystem paths, provider-native session identifiers, exception text, and
arbitrary provider metadata. Failed attempts retain a bounded HTTP status and
fixed action name rather than a raw error message.

## Failure semantics

LASO writes the requested event before a privileged operation. If that write
fails, the operation is not run and the API returns 503 AUDIT_UNAVAILABLE.
The outcome write follows the operation in a separate transaction. If it fails,
the operation may already have taken effect; the request event remains without
a result. The API returns 503 AUDIT_UNAVAILABLE. Check the current resource
state and audit feed before retrying. An unresolved request is evidence of an
interrupted or unconfirmed result, not proof that the operation failed.

The feed is durable audit metadata, not a transactional outbox and not an
exactly-once side-effect mechanism. Direct database administrators can modify
database contents; immutability depends on deployment PostgreSQL roles and
backup controls. Keep the database role and backups restricted.

M10 maintenance also keeps a bounded local journal used for restart
reconciliation. The PostgreSQL feed is cluster-visible and complements that
journal; it does not make local desired maintenance state cluster-owned.
