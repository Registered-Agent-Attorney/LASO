# ADR 0003: One LASO owner per database

## Context

Current scheduler and recovery semantics are local-service semantics, not a
distributed execution protocol.

## Decision

One LASO service owns a selected database. SQLite uses its process lease and
PostgreSQL uses a session-held advisory lock. A competing owner fails during
startup.

## Consequences

Claims and transactions are safe within the selected ownership model, while
distributed scheduling, worker leasing, fencing, and HA remain explicitly out
of scope.

## Supersedes

None; this records the current architecture.
