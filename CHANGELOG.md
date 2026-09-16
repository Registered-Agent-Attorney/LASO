# Changelog

## Unreleased

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
