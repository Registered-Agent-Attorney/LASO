# Capability matrix

The matrix describes the supported contract in this release candidate. A
successful local unit test does not imply support in every backend mode.

| Capability | SQLite | PostgreSQL single instance | PostgreSQL multi-instance |
|---|---|---|---|
| Deterministic pipelines | Supported | Supported | Supported |
| Real agents/providers | Local process only | Local process only | Supported through eligible worker workloads |
| Durable approvals | Supported | Supported | Supported for the owner-controlled run |
| Crash/restart recovery | Local durable history | Local durable history | Owner and worker recovery with leases/fencing |
| Distributed `NodeWork` | Not supported | Available but single-instance | Supported with capability-filtered claims |
| Remote agent workers | Not supported | Not required | Supported for bounded worker workloads |
| Worker leasing/fencing | Not applicable | Available but single-instance | Supported; stale completion is rejected |
| Workspace manifest transport | Not supported | Available for local validation | Supported with bounded manifests and SHA-256 validation |
| Large artifact transport | Not supported | Not supported | Not supported in RC1 |
| Arbitrary remote tools | Not supported | Not supported | Not supported |

LASO provides at-least-once attempt semantics with one authoritative fenced
completion. It does not claim exactly-once execution. Remote side effects,
general remote shell access, cluster scheduling, and large-object transfer are
outside this release candidate.
