# Storage backends

LASO application and runtime code depend on the backend-neutral `Storage` interface.
SQLite is the default local backend. PostgreSQL is optional and is enabled only by
building with `-DLASO_ENABLE_POSTGRES=ON` and selecting `storage_backend: postgres`.
The PostgreSQL build requires `libpqxx-dev` and `libpq-dev`; the default build does
not require PostgreSQL headers or libraries.

## Contract

`Storage::commit` accepts a checkpoint batch and commits every record atomically or
none of them. Records are upserts by `(RecordKind, id)`; updates retain their
original insertion sequence so list order is stable. Pipeline records are immutable:
the same ID and exact body is idempotent, while a different body returns
`ErrorCode::Conflict`. Empty record IDs, unknown record kinds, invalid JSON, and
oversized bodies are rejected consistently by both adapters.

`get` returns the JSON body or `ErrorCode::NotFound`. `list` optionally filters by
`run_id`, orders by the durable insertion sequence, and supports bounded limit/offset
pagination. The application stores typed runs, attempts, messages, approvals,
artifacts, events, event-source state, external-event claims, worker jobs, schedules,
triggers, schedule occurrences, and trigger deliveries as JSON records; the storage layer
does not duplicate those domain objects into backend-specific tables.

`claim` is an atomic insert-only operation for a schedule occurrence, trigger
delivery, external event identity, or worker-job idempotency identity. It returns true only for the first claim of
an ID and never overwrites the winning body. For external events, the associated
normal Event record is inserted in the same transaction, so a successful claim
cannot expose a dedupe record without its event. This is a durable deduplication
boundary, not a distributed exactly-once guarantee.

## SQLite

SQLite is opened with full mutex mode and each adapter serializes operations with a
mutex. Schema initialization is transactional, uses `PRAGMA user_version`, enables
WAL/full synchronization and foreign keys, and retains the existing local database
format. `Service` acquires a filesystem process lease before opening the database.

## PostgreSQL

PostgreSQL uses one bounded connection per `PostgresStorage`, protected by a mutex;
transactions are scoped to individual operations and checkpoint batches. Startup
creates the configured validated schema and applies idempotent version-1 through
version-4 migrations in a transaction. Version 3 adds event-source state and
external-event claim records; version 4 adds durable worker jobs. A session-held advisory lock prevents two LASO services from
owning the same database at once. Schema identifiers are validated before being
quoted; table names come only from the internal `RecordKind` mapping and values use
parameterized queries. DSNs and raw driver diagnostics are not returned to API
callers or written to LASO logs.

The public CI workflow starts an isolated PostgreSQL 16 service with disposable
test credentials. The same storage conformance tests run against SQLite and
PostgreSQL when `LASO_TEST_POSTGRES_DSN` is configured, and a runtime/reopen test
also verifies normal pipeline and child-run persistence on PostgreSQL.

The backend choice does not change pipeline revision immutability, checkpoint
atomicity, event ordering, approvals, recovery, artifacts, cancellation, or
parent/child persistence, schedule occurrence claims, or trigger delivery deduplication.
PostgreSQL is not a distributed worker or registry service; it is an optional
storage adapter for one LASO service owner. Scheduler processes use the same
service ownership coordination as the rest of LASO.
