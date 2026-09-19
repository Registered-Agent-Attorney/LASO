# ADR 0007: Prepare coordination primitives for opt-in distributed execution

This ADR records the coordination primitives that preceded the distributed
execution milestone. The single-owner defaults remain authoritative, while the
later opt-in `execution_mode: multi_instance` uses these primitives for whole-run
ownership and deterministic branch `NodeWork` claims. It does not provide
distributed worker leasing or exactly-once side effects.

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
remain in force. `single_owner` remains the default. PostgreSQL can explicitly
enable multi-instance whole-run ownership after the version-7 instance registry
and fenced-checkpoint work; SQLite remains single-instance.

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

The historical statement that multi-instance configuration is rejected is
superseded by the opt-in distributed execution milestone. ADR 0003's single-owner
default remains unchanged.

## Future work

WorkerJob ownership and distributed event delivery remain future work. Pipeline-run
ownership uses these primitives only in explicit multi-instance mode, with fenced
writes and end-to-end recovery coverage.
