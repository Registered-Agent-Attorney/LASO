# ADR 0007: Prepare coordination primitives without enabling distributed execution

## Context

LASO currently assumes one service process owns a selected database. Removing that
protection without a durable ownership protocol would permit duplicate scheduling
and ambiguous recovery. PostgreSQL also needs bounded concurrent database access
as coordination work grows, while SQLite must remain the default embedded backend.

## Decision

PostgreSQL storage uses a bounded RAII connection pool. Each running service gets a
fresh opaque UUID instance identity that is independent of host, user, network, and
hardware identifiers. PostgreSQL migration version 6 adds a coordination lease
table keyed by resource. Atomic acquisition uses database time and advances a
monotonic fencing token on every takeover. Renewals require the current owner and
token before expiry; release and inspection are token-aware; protected operations
can reject stale tokens.

The existing SQLite file lease and PostgreSQL session-held advisory owner lock
remain in force. `single_owner` remains the default and the only production service
mode. The experimental multi-instance configuration is rejected; this ADR adds
primitives and tests, not distributed scheduler, run, worker, or event ownership.

## Consequences

The pool bounds PostgreSQL connections and provides acquisition timeout,
replacement, and utilization diagnostics without exposing credentials. Lease
contention, heartbeat renewal, expiry takeover, stale-token rejection, process
exit, and concurrent takeover are testable safety properties.

The following invariants are explicit:

- at most one active owner holds a resource for a fencing generation;
- each successful takeover receives a greater fencing token;
- stale owners cannot pass the current-token check;
- lease expiry does not prove that old external side effects stopped;
- distributed exactly-once execution is not claimed.

## Supersedes

None. This extends the single-owner decision in ADR 0003 without changing it.

## Future work

Later work may use these primitives for scheduler claims, pipeline-run ownership,
WorkerJob ownership, event delivery, and crash takeover. Each adoption must add
fenced writes and end-to-end recovery tests before enabling multi-instance mode.
