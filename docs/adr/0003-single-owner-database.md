# ADR 0003: One LASO owner per database by default

The PostgreSQL-only opt-in `execution_mode: multi_instance` described in
`docs/distributed-execution.md` supersedes the former single-owner-only scope.
SQLite and the default PostgreSQL mode still follow this ADR.

## Context

Default scheduler and recovery semantics are local-service semantics, not a
distributed execution protocol.

## Decision

One LASO service owns a selected database. SQLite uses its process lease and
PostgreSQL uses a session-held advisory lock. A competing owner fails during
startup.

## Consequences

Claims and transactions are safe within the selected ownership model. Distributed
worker leasing and HA remain explicitly out of scope; opt-in PostgreSQL
multi-instance mode adds whole-run leases and fenced checkpoints only.

## Supersedes

Superseded for PostgreSQL multi-instance mode by the distributed execution
milestone; retained as the default ownership decision.
