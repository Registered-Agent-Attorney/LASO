# Persistent agent sessions

LASO sessions provide a stable orchestration identity, a durable ordered input log,
retry-safe input acceptance, and a durable event journal. The database assigns a
per-session sequence while holding the session row lock. A repeated
`idempotency_key` with the same input returns the original turn; reusing that key
for different input is a conflict. SQLite serializes writes within its documented
single-instance boundary. PostgreSQL uses a row lock, so independent service
instances share the same ordering and replay history.

Sessions are generic runtime primitives. LASO does not define users, accounts,
authorization policy, conversation membership, presence, or UI. An external
application authenticates its clients, decides which clients may address a
session, and renders the events. For example:

```text
external client A ----\
                       > logical LASO session -> backend agent context
external client B ----/
```

Both clients can independently read `GET /api/v1/sessions/{id}/events?after=N`
or subscribe to `GET /api/v1/sessions/{id}/events/stream`. SSE event IDs are
per-session sequence numbers and are accepted as `Last-Event-ID` on reconnect.
The stream polls the durable database journal; it is a delivery mechanism only.
A client can always replay after its last observed sequence. PostgreSQL polling
also makes events written through another LASO instance visible without sticky
sessions or a process-local notification bus.
Each LASO HTTP process accepts at most 32 concurrent session streams; excess
connections receive HTTP 429. The existing HTTP server also has a total
connection limit. External applications can fan out to their clients above
LASO when they need more concurrent viewers.

## API sketch

```http
POST /api/v1/sessions
{"pipeline_id":"review@1"}

POST /api/v1/sessions/{id}/turns
{"idempotency_key":"client-request-42","input":{"task":"inspect authentication"}}

GET /api/v1/sessions/{id}/events?after=0&limit=50
GET /api/v1/sessions/{id}/events/stream
POST /api/v1/sessions/{id}/close
```

Turn inputs are durably accepted and ordered before a success response. The
current session API is the persistence and replay substrate; it does not yet
schedule those inputs as sequential pipeline runs or bind them to a provider's
continuation handle. Provider continuation remains opaque and must not be
placed in API responses. A follow-up can connect the durable turn queue to the
existing worker recovery and fencing path without changing the session event
cursor contract.
