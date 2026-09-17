# Validation record

Snapshot validated: 2026-09-17. The Windows development workstation was used for
source review and packaging. Native validation was performed over SSH in an isolated
directory on a remote Ubuntu 24.04.5 LTS (x86-64) host.

## Implemented

C++20 source, public headers, CMake targets, native C plugin SDK and examples,
SQLite persistence, runtime, API/CLI, policies, scheduling, event-source ingress,
and artifact interfaces, tests, systemd/Docker deployment files, documentation, and
Linux CI are present.

The test inventory contains **175 GoogleTest cases** plus **2 CTest entries** for CLI
validation and a process smoke/restart scenario, for **177 CTest entries**. Composition,
storage, event-ingress, and worker-adapter coverage includes
revision immutability, cross-boundary payload and schema behavior, child retry
identity, approval-compatible persistence, parallel children, recursion, depth,
run inspection, backend conformance, pagination, rollback, concurrent persistence,
PostgreSQL runtime/reopen behavior, event-source ABI/lifecycle, bounded host ingress,
source schema validation, durable external-event deduplication, source-state restart,
offline plugin-to-trigger-to-pipeline execution, durable worker-job lifecycle,
worker idempotency, concurrent duplicate submission, worker status/result/cancel
correlation, optional normalized usage, execution budgets, transport/job failure
classification, supervised process-worker framing, protocol mismatch, bounded
timeouts, cancellation, shutdown, process exit, and separate-process pipeline
execution, schema and policy enforcement at the worker boundary, late terminal
event rejection, and manager restart reconciliation.

## Statically reviewed

- All source paths named by CMake exist; source/header references, namespaces,
  definitions, dependency discovery, and target boundaries were reviewed.
- GCC/Clang flags, C++20 requirements, runtime output paths, and the Linux-only
  platform check were reviewed.
- ABI version/size checks, C exports, buffer ownership, library lifetime, and
  explicitly configured plugin discovery paths were reviewed.
- Transaction checkpoints, approval/restart handling, cancellation persistence,
  finite edge/loop/retry limits, join state, and subpipeline waits were reviewed.
- HTTP isolation and limits, loopback defaults, policy checks, error/log contents,
  and deployment configuration were reviewed.
- Event-source lifecycle isolation, thread-safe host emission, source identity checks,
  bounded ingress, durable external-event claims, callback shutdown behavior, and
  trigger provenance/depth integration were reviewed.
- Worker ABI prefix compatibility, bounded request/result handling, durable job
  idempotency, status/result/cancel correlation, callback serialization, recovery
  reconciliation, terminal-state protection, worker limits, and secret-safe job
  metadata were reviewed.
- Ubuntu GCC/Clang, Debian 13, PostgreSQL, clang-tidy, formatting, and ASan/UBSan
  CI jobs are defined in `.github/workflows/linux.yml`.

Static review found and corrected mismatched binding acceptance, fixture replacement
lengths, cancelled approval records, cancellation persistence, join resume state,
scheduler stop-before-start handling, completed-attempt recording on edge-budget
failure, a GoogleTest name lookup collision, Boost discovery under CMake 3.31, and
unclear failures for unavailable configured plugin directories.

The release-hardening audit additionally corrected terminal checkpoint immutability,
recovery pagination beyond one record page, concurrent-branch policy and shared step
and edge budgets, branch deadline inheritance, plugin callback exception containment,
and safe relative `$ref` resolution from the declaring schema document. Regression
tests cover these failure paths and schema diagnostics. Composition coverage also
includes nested A → B → C execution and child output-contract failure propagation.

## Tested on this development workstation

| Check | Result |
|---|---|
| Native clang-format | Not installed on the Windows workstation; the required Linux check is recorded below |
| CMake source paths | PASS: all referenced source files present |
| LASO include resolution and documentation links | PASS |
| Safe YAML/configuration document scan | PASS: 13 documents |
| Git whitespace check | PASS |
| Private terminology and obvious credential-pattern scan | PASS: no matches |

No Windows C++ compilation was attempted because LASO is intentionally Linux-only.

## Tested on a remote Ubuntu 24.04.5 host

| Check | Result |
|---|---|
| CMake 3.28.3 + Ninja configure | **PASS** |
| GCC 13.3.0 Debug build | **PASS** |
| Clang 18.1.3 Debug build | **PASS** |
| GCC Debug CTest suite | **PASS: 177/177 scheduled; 175 passed and 2 PostgreSQL cases skipped without a PostgreSQL DSN** |
| GCC Release CTest suite | **PASS: 177/177 scheduled; 175 passed and 2 PostgreSQL cases skipped without a PostgreSQL DSN** |
| Clang Debug CTest suite | **PASS: 177/177 scheduled; 175 passed and 2 PostgreSQL cases skipped without a PostgreSQL DSN** |
| Clang Release CTest suite | **PASS: 177/177 scheduled; 175 passed and 2 PostgreSQL cases skipped without a PostgreSQL DSN** |
| ASan + UBSan build and CTest, leak detection enabled | **PASS: 177/177 scheduled; 175 passed and 2 PostgreSQL cases skipped without a PostgreSQL DSN** |
| PostgreSQL-enabled GCC Debug CTest suite | **PASS: 177/177; all PostgreSQL cases executed against an isolated PostgreSQL 16 cluster** |
| clang-format `--dry-run --Werror` on Linux | **PASS** |
| clang-tidy 18 against the Clang compilation database | **PASS: exit 0; advisory warnings remain** |
| Debian 13 container | **Validated by the public PR workflow; post-fix result pending the public workflow** |
| Multi-stage Debian runtime image build | **PASS** |
| Runtime image health endpoint and unprivileged UID | **PASS: host-network health endpoint; image runs as `laso:laso`** |
| systemd unit syntax and dependency verification | **PASS** |
| CLI and process restart smoke tests | **PASS: CLI validation and process smoke executed; process smoke used a local extracted jq 1.7 because jq is not installed system-wide** |
| Offline shipped examples | **PASS: all shipped offline pipeline definitions validated; event-source plugin → durable event trigger → completed run and worker plugin → durable job → validated output exercised; optional local-openai not run** |
| HTTP health/run integration tests | **PASS** |
| valid, invalid, incompatible, and symlinked `.so` plugin tests | **PASS** |
| GPU inference | **Not part of this worker-hardening audit** |

clang-tidy reported advisory findings because `WarningsAsErrors` is intentionally
empty in the v0.1 baseline. They include enum-size suggestions, explicit handling
of ignored networking return values and exception boundaries, integer widening,
and small copy/allocation opportunities. The configured CI command exits zero.

## Requires further Linux validation

| Check | Status |
|---|---|
| Full systemd installation, privilege setup, and shutdown behavior | **PENDING** |
| GitHub Actions execution | **Pending the post-fix PR workflow** |
| Optional TSan execution | **BLOCKED ON HOST: GCC runtime aborted during test discovery with `unexpected memory mapping`** |

Use [the Linux validation procedure](docs/first-linux-validation.md) when validating
another distribution or deployment environment. Ubuntu and Debian results above
are actual executions; pending and blocked rows do not imply success. The TSan
failure occurred before LASO tests ran and must be repeated on a compatible kernel
and sanitizer runtime; global ASLR settings were not weakened to work around it.

## Worker-hardening branch validation

This branch was validated from upstream commit
`e8bff28d4f1cf2c2e93ece067a988a47edf8bd02` using GCC and Clang Debug/Release,
ASan/UBSan, and an isolated local PostgreSQL 16 cluster. SQLite CTest ran 177
tests (**175 passed, 2 PostgreSQL tests skipped**); PostgreSQL-enabled CTest
completed **177/177**; and the ASan/UBSan CTest ran 177 tests (**175 passed,
2 PostgreSQL tests skipped**) with leak detection enabled. The added
worker tests cover no/partial/full usage, persistence, wall/token/cost budgets,
transport-versus-job failures, and the supervised local process protocol. The
process-worker suite adds 10 focused tests, including a separate-process
pipeline and bounded child cleanup. SQLite and PostgreSQL server smoke tests both
passed health, version, Hello, and run inspection; PostgreSQL state remained
available after a server restart. The shipped worker-adapter example completed
successfully. No credentials or DSNs were written to tracked files.

The schema-contract tests additionally cover valid and invalid input/output,
registration-time missing or malformed schemas, safe local references, forbidden
remote references, traversal rejection, payload limits, explicit ValidatorNode
use, and concurrent cache access.

The worker-adapter tests additionally cover ABI-compatible plugin discovery and
health, normal `WorkerNode` execution, worker-boundary schemas and policy approval,
durable idempotency under sequential and concurrent submission, optional usage
metadata, budget acceptance/rejection and accumulation, transport-versus-job
failure classification, terminal late-event handling, and reconciliation after
manager restart. Worker job requests persist bounded metadata only; instructions
and payloads are not copied into job records.

The shared storage conformance tests run against SQLite on every default build and
against a real disposable PostgreSQL service when `LASO_TEST_POSTGRES_DSN` is set.
They cover immutable pipeline revisions, pagination, invalid-record rejection,
structured operational records, rollback boundaries, concurrent writes and claims,
durable schedules/triggers, occurrence/delivery deduplication, and restart/reopen
recovery. Scheduler tests cover UTC one-time/interval/cron behavior, misfire and
overlap policies, bounded capacity retry, event matching/depth/deduplication,
API/CLI surfaces, and normal-runtime launch provenance.
