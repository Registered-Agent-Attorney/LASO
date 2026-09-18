# Changelog

## Unreleased

- Added a bounded PostgreSQL connection pool and opaque service-instance identity.
  Added PostgreSQL lease, heartbeat, expiry, takeover, and fencing primitives with
  migration version 6 and contention/crash coverage. SQLite and default
  single-owner service behavior remain unchanged; distributed scheduling is not
  enabled.

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

## 0.2.0 — Framework Milestone 1 (in progress)

- Native model-provider components are available through the stable C plugin ABI.
- Added an offline deterministic provider plugin and end-to-end AgentNode coverage.
- Corrected Linux validation status claims after recorded Ubuntu, Debian, CI, and
  sanitizer validation.

## 0.1.0 — initial skeleton (unreleased)

- Linux-first C++20 libraries and native CLI/server with CMake/Ninja configuration.
- Typed YAML pipelines, message/provenance envelopes and explicit run/attempt states.
- Bounded coroutine execution, retries, cooperative deadlines/cancellation, durable
  approval, branch/join checkpoints and registered subpipelines.
- SQLite state and event history, local artifacts, policy and identity boundaries.
- Versioned C plugin SDK, Linux `.so` loader and deterministic example tool.
- Offline GoogleTest/CTest suites, Ubuntu GCC/Clang and Debian CI definitions,
  sanitizer options, systemd and container examples, contributor documentation.
- Historical initial-baseline note; see `VALIDATION.md` for actual results.
