# LASO implementation report

Current development starts from `v0.1.0-rc.2`; untagged M8 commits retain that
release version until a later candidate is published. The RC2 validation record
describes the release source at public `main` SHA
`568edd2c9934ad10553c822af977113227379931`. Exact scope and dated historical
evidence are in [VALIDATION.md](VALIDATION.md). RC2 is a release candidate for
controlled evaluation, not a general production-readiness declaration.

## Current implementation

LASO is a Linux-first C++20 workflow orchestration framework. Applications
provide their own pipeline definitions and integrations. The API and CLI use
shared application services; the runtime owns pipeline execution, policies,
durable state transitions, scheduling, recovery, and worker coordination.

PostgreSQL is the only supported database implementation and is required for
single-owner and multi-instance deployments. The storage adapter applies
versioned migrations and uses bounded connection pools. SQLite storage,
`storage_backend`, and `db_path` are not shipped. Existing SQLite state is not
imported automatically; the migration and backup requirements are in
[storage.md](docs/storage.md#upgrading-from-sqlite).

The framework includes bounded coroutine execution, approvals, immutable
pipeline revisions and child runs, schedules and event triggers, durable agent
sessions and sequential turns, replayable events/SSE, context provenance,
recovery inspection, and opt-in multi-instance run and deterministic `NodeWork`
claiming with leases and fencing. Worker attempts are at least once; external
provider side effects are not guaranteed exactly once.

The local filesystem is the default content-addressed artifact store. An
optional S3-compatible backend is built only when enabled. Artifact metadata
retains integrity and run/worker provenance; remote S3 garbage collection is
unsupported. Provider adapters are optional supervised executables. The
Codex app-server adapter is the validated real-provider durable-session path;
other provider adapters remain optional.

## Deployment and trust boundaries

The C++ plugin SDK uses a versioned C ABI. Native plugins are loaded in-process
with `dlopen` and are privileged code; LASO does not sandbox them. Supervised
process workers provide a lifecycle and transport boundary, not an OS sandbox.
See the [plugin ABI](docs/plugin-abi.md), [worker process protocol](docs/worker-process-protocol.md),
and [security threat model](docs/security-threat-model.md).

The shipped HTTP API defaults to unauthenticated loopback access. LASO Core
defines an `IdentityProvider` interface but ships only the local-development
identity; it does not include a user authentication platform. Remote API access
must be protected by a deployment-owned authentication and authorization
boundary. The API avoids returning provider-native continuation identifiers and
provides safe operator inspections that omit prompts, message payloads, and
absolute artifact locations. M8 adds paged operator endpoints for diagnostics,
run/session/worker state, approvals, integrations, artifacts, instances, and
leases; artifact integrity results omit filesystem locations and raw errors.
Current untagged development also has a cheap PostgreSQL/schema/runtime
readiness probe and an authorization-aware Prometheus endpoint for bounded API
response/latency and session-SSE admission metrics. These M9 additions are not
part of the published RC2 tag. See [readiness and metrics](docs/observability.md)
for scope and security boundaries.

The PostgreSQL schema migrates forward at startup. Binary rollback across an
incompatible schema requires restoration of a compatible database backup.
Deployment examples cover an unprivileged systemd service and a Debian
container; the RC2 dedicated-account system-unit filesystem sandbox was not
rerun specifically for this release candidate.

## Validation status

- Clean-source RC2 validation passed 272 LASO tests, GCC/Clang build and
  regression coverage, Go static analysis and race tests for the paired Web
  client, 11/11 frontend tests, and 7/7 PostgreSQL browser-acceptance scenarios.
- Exact public `main` Actions passed all 8 jobs. The private Go Web repository's
  exact `main` passed its 4 Go jobs and PostgreSQL browser-acceptance workflow.
- PostgreSQL backup/restore and durable-session recovery passed. The actual
  Codex app-server completed three durable turns across LASO restart and a
  further turn after database restore; provider-native IDs and session values
  are excluded from this report.
- The TSan binary built, but the runtime failed before test discovery with
  `unexpected memory mapping`; no test body ran under TSan and no TSan pass is
  claimed. ASan/UBSan and Go race validation passed.
- RC2 systemd validation covered the installed unit and user-service lifecycle.
  The dedicated-account system-unit filesystem sandbox was not rerun
  specifically for RC2. Earlier system-account evidence remains dated in
  `VALIDATION.md`.

## Current limitations

LASO does not provide a model-serving service, persistent secret store,
built-in authentication platform, native-plugin sandbox, full tracing or a
complete service metrics suite, general remote shell, cluster scheduler, or
exactly-once external side-effect guarantee. Multi-instance
execution is opt-in and constrained by the supported worker and deterministic
branch contracts. PostgreSQL schema rollback requires a compatible backup when
the prior binary cannot read the migrated schema. These limits and their
validation boundaries are not evidence of production readiness.

SQLite references in dated sections of [VALIDATION.md](VALIDATION.md) describe
historical builds only. Current installation, runtime, and deployment support
is PostgreSQL-only. M8 operator API additions are not part of the published
RC2 tag until a later release candidate includes them.
