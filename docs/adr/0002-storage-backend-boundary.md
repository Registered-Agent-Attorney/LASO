# ADR 0002: Storage is backend-neutral

> Superseded for backend support by [ADR 0009](0009-postgres-only-storage.md). This records the former SQLite plus optional PostgreSQL policy; the generic `Storage` abstraction remains.

## Context

At acceptance, LASO supported embedded SQLite and an optional PostgreSQL
deployment without duplicating runtime persistence semantics.

## Decision

Runtime code uses the backend-neutral `Storage` record/claim contract. SQLite
and PostgreSQL own their drivers, SQL, schema setup, transactions, and locking
behind that boundary.

## Consequences

At acceptance, SQLite was the default embedded backend. Backend conformance
tests defined shared behavior; backend-specific tests covered driver and
deployment details.

## Supersedes

None; this recorded the architecture at the time of acceptance.

> **Status: superseded** by ADR 0009. This records the former SQLite plus optional PostgreSQL policy. The generic `Storage` boundary remains; PostgreSQL is now the sole shipped implementation.
