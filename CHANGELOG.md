# Changelog

## Unreleased

- Add coordinated instance drain and maintenance control with admission and
  ownership-claim gating, readiness integration, bounded maintenance metrics,
  local actor-attributed transition history, and restart/rolling-operation
  guidance. Drain preserves normal lease renewal and stale-fence rejection; it
  does not claim zero-downtime operation. See `docs/maintenance.md`.
- Add `/api/v1/health/ready` with bounded PostgreSQL, schema, and runtime-drain
  status while preserving `/api/v1/health` as the compatibility liveness route.
- Add authorization-aware Prometheus text metrics for bounded API request
  counts/latency and session-SSE admission. Metric labels exclude paths,
  principals, workflow IDs, prompts, messages, and errors.
- Added bounded, metadata-only operator API views for service compatibility and
  capacity, runs, durable sessions and turns, NodeWork, worker jobs and
  requests, approvals, providers, plugins, artifacts, instances, and leases.
  Artifact integrity reports counts and status without paths or raw errors.
- Corrected current documentation to distinguish the RC2 release baseline from
  M8 development and to describe PostgreSQL-only support and current limits.

## 0.1.0-rc.2 (2026-09-28)

### Breaking changes

- PostgreSQL is required for all deployments, including single-owner mode.
  SQLite storage, `storage_backend`, and `db_path` were removed. Existing
  SQLite state is not imported; back it up and plan an application-specific
  export/import before upgrading. Startup detects the default `.laso/laso.db`
  file and refuses to continue until it is handled. See [storage upgrade
  guidance](docs/storage.md#upgrading-from-sqlite).

- Added an optional S3-compatible content-addressed artifact backend behind
  `LASO_ENABLE_S3`; the filesystem backend remains the default and cloud-free.
  Uploads and downloads stream through bounded temporary files with hash/size
  verification, immutable conditional publication, bounded request behavior,
  and AWS SDK credential-chain integration. Remote S3 garbage collection is
  intentionally unsupported. Physical owner/worker acceptance, fault recovery,
  stale-result fencing, and trusted/untrusted TLS validation have passed using
  disposable infrastructure. M4.1 merged after physical acceptance and green hosted CI; see
  `VALIDATION.md` for the sanitized evidence.

- Added durable agent sessions, idempotent ordered turns, sequential run
  execution, opaque provider continuation, event replay, and resumable SSE on
  PostgreSQL. Capability discovery, immutable context generations, run-context
  snapshots, and opt-in context reduction are part of the merged session API.
  LASO remains authoritative for session history and execution state; it does
  not promise exactly-once provider side effects. See `VALIDATION.md` and
  `docs/sessions.md` for the supported contract.

- Preserve expired PostgreSQL lease rows so fencing tokens remain monotonic, and
  mark a superseded running node attempt failed in the same fenced transaction
  that claims its replacement. Bound PostgreSQL connection startup with the
  configured pool acquisition deadline, including when a server accepts TCP but
  stalls during protocol startup. Regression tests cover token takeover,
  interrupted attempt history, and stalled startup.

- Added durable content-addressed artifact transport for distributed workspaces
  and returned results. Files stream through atomic filesystem objects, can be
  materialized through an authenticated object-only gateway when instances do
  not share a filesystem root, and retain run/NodeWork/attempt/worker/fence
  provenance. Added integrity and conservative GC operator commands plus large
  generated-file and gateway regression coverage. PostgreSQL stores metadata and
  references only; at-least-once attempts with one authoritative fenced
  completion remain the guarantee. The first artifact-specific publication did
  not yet have complete chaos evidence; that gap closed under M3.6 before this
  release. See `VALIDATION.md`.

- Closed M3.6 artifact-transport validation using separate Linux owner and
  worker operating-system/process/network boundaries. Normal distributed runs,
  upload/materialization worker-loss cases, stale completion fencing,
  PostgreSQL interruption, owner recovery, integrity verification, and the
  short-TTL lease regression passed. LASO provides at-least-once attempts with
  one authoritative fenced completion and does not claim exactly-once
  execution. See `VALIDATION.md` for the acceptance evidence.

- Added opt-in PostgreSQL node-work distribution for deterministic parallel
  branches. Durable fenced `NodeWork` records, bounded global/per-run node slots,
  crash takeover, stale-result rejection, retry attempt identity, cross-instance
  cancellation, and approval resume preserve the existing runtime path. Tools,
  providers, workers, subpipelines, approvals inside branch paths, and other
  host-local or side-effecting work remain run-owner controlled; no distributed
  exactly-once guarantee is claimed.

- Added opt-in PostgreSQL multi-instance execution. Multiple LASO service
  processes can claim whole queued runs using database-time leases, heartbeats,
  fencing tokens, durable service-instance state, and crash takeover. Distributed
  execution does not claim exactly-once external effects.

- Hardened the CLI schedule and trigger deletion results so successful deletes
  return an explicit stable result instead of depending on post-delete record
  lookup. Added an integration regression test and refreshed the release-
  readiness validation inventory; no version bump or production-readiness claim
  is implied.

- Added a bounded PostgreSQL connection pool and opaque service-instance identity.
  Added PostgreSQL lease, heartbeat, expiry, takeover, and fencing primitives with
  migration version 6 and contention/crash coverage. Multi-instance run
  ownership is an explicit PostgreSQL-only opt-in.

- Added an optional supervised Claude Code adapter using the documented
  structured `stream-json` CLI interface. Session identifiers, resume/follow-up,
  normalized results and usage, project-root checks, bounded process handling,
  and LASO-controlled permission/question requests are kept outside Core.
  The adapter is disabled by default; real provider validation remains gated on
  an installed authenticated Claude Code executable.

- Added the optional `laso-codex-worker` adapter. It uses the installed Codex
  app-server's structured JSON-RPC interface behind the supervised process
  transport, supports persistent thread start/resume, normalized results and
  token usage, bounded project roots, cancellation, and LASO-controlled
  command/file/permission approvals. It is disabled by default and does not
  add Codex-specific behavior to Core.

- Added durable, policy-controlled worker `approval`, `permission`, and
  `question` requests to the versioned process protocol. Added an opt-in
  supervised OpenCode 1.18.x adapter with session continuation, bounded
  structured result/usage normalization, explicit project-root checks, and
  conservative restart/cancellation behavior. Remote workers and distributed
  execution remain deferred.

- Added backend-neutral worker usage accounting for optional durations, token,
  tool/action, identity, cost-unit, and structured metadata fields. Added
  deterministic wall-time and per-run token/cost budgets; no billing or vendor
  pricing is included.
- Added the `WorkerTransport` seam while preserving the native in-process
  `WorkerAdapter` ABI path. Transport failures are distinct from worker job,
  budget, and cancellation outcomes. Added an opt-in supervised local process
  transport, bounded versioned NDJSON protocol, and deterministic reference
  worker host; remote transports remain future work.
- Fixed the Clang formatting gate and added a real PostgreSQL CI service job.
- Hardened SQLite/PostgreSQL adapter parity with shared conformance coverage,
  normalized invalid record handling, and a PostgreSQL-backed runtime/reopen test.
- Added optional JSON Schema contracts for node inputs and outputs.
- Added shared schema validation for explicit `validator` nodes, safe local `$ref`
  loading, bounded schema/payload resources, and structured validation errors.
- Hardened terminal-state recovery, concurrent branch policy and budget enforcement,
  plugin exception boundaries, paginated recovery scans, and adversarial regression
  coverage without changing the scheduler architecture.
- Added durable immutable `name@version` pipeline revisions and first-class
  `subpipeline` child runs with persisted parent/child relationships, direct payload
  passing, retry-distinct child attempts, approval recovery, recursion detection,
  depth limits, and API/CLI run inspection.
- Added durable UTC one-time, interval, and five-field cron schedules plus internal
  event triggers. Occurrence claims, misfire/overlap policies, restart recovery,
  bounded trigger delivery, event-depth protection, trigger-origin provenance, and
  schedule/trigger API and CLI operations use the normal runtime and storage paths.
- Added a generic durable external-worker adapter framework: ABI-v1 worker
  components, bounded submit/status/result/cancel callbacks, durable worker jobs,
  idempotent attempt identities, event-based completion, recovery reconciliation,
  cancellation/timeout handling, per-worker limits, API/CLI inspection, and an
  offline worker example. Vendor-specific adapters and distributed workers remain
  out of scope.

## Previous 0.1.0-rc.1 candidate (not tagged)

- Release-hardening pass: documented clean-clone builds, optional dependencies,
  configuration, capability boundaries, recovery semantics, migration policy,
  and the public threat model.
- Added safe operator inspection views for runs, durable node work, attempts,
  worker jobs, leases/fences, failures, and artifact integrity metadata.
- Added a credential-free deterministic PostgreSQL owner/worker example and
  documented installation and controlled recovery workflows.

## 0.1.0 — initial skeleton (historical baseline)

- Linux-first C++20 libraries and native CLI/server with CMake/Ninja configuration.
- Typed YAML pipelines, message/provenance envelopes and explicit run/attempt states.
- Bounded coroutine execution, retries, cooperative deadlines/cancellation, durable
  approval, branch/join checkpoints and registered subpipelines.
- SQLite state and event history (removed in the PostgreSQL-only release
  candidate), local artifacts, policy and identity boundaries.
- Versioned C plugin SDK, Linux `.so` loader and deterministic example tool.
- Offline GoogleTest/CTest suites, Ubuntu GCC/Clang and Debian CI definitions,
  sanitizer options, systemd and container examples, contributor documentation.
- Historical initial-baseline note; see `VALIDATION.md` for actual results.
