# Validation record

## Latest systemd deployment validation (2026-09-22)

This section records the deployment-hardening work from the current source tree;
the older dated matrices below are retained as historical snapshots. Validation
used Linux x86-64, systemd 255, GCC 13.3.0, CMake 3.28.3, and Ninja. Debug,
Release, sanitizer, and PostgreSQL builds all configured, built, and staged
installed binaries, headers, example configuration, documentation, and the
generated systemd unit under isolated install prefixes. `systemd-analyze verify`
accepted the generated unit. The unit embeds the configured install prefix and
does not refer to the source/build tree.

| Check | Result |
|---|---|
| GCC Debug CTest (SQLite default) | **PASS: 202 scheduled; 198 passed, 4 skipped** (three PostgreSQL-only tests and the PostgreSQL backend-not-built guard) |
| GCC Release CTest (SQLite default) | **PASS: 202 scheduled; 198 passed, 4 skipped** (same expected PostgreSQL-disabled cases) |
| ASan + UBSan CTest (SQLite default, leak detection enabled) | **PASS: 202 scheduled; 198 passed, 4 skipped** (same expected PostgreSQL-disabled cases) |
| PostgreSQL-enabled GCC Debug CTest | **PASS serial run: 220 scheduled; 219 passed, 1 skipped** (cross-machine M3 acceptance requires a separate remote setup) |
| PostgreSQL CTest parallel diagnostic run | **Not supported with the shared single-owner test database:** overlapping test processes were rejected by the intentional PostgreSQL database ownership lock (`PostgreSQL database is owned by another LASO process`). Two of the four initial parallel failures reproduced with this explicit cause; the full serial suite passed. Parallel result is not claimed as passing.** |
| Unit installation and syntax | **PASS:** `cmake --install` into isolated prefixes; expected executables/config/docs/unit present; `systemd-analyze verify` passed |
| Native systemd user-service acceptance, SQLite | **PASS:** health, durable approval recovery across SIGTERM and SIGKILL restart, exactly-once completion event, SIGINT, repeated lifecycle, configuration failures, worker-child cleanup |
| Native systemd user-service acceptance, PostgreSQL | **PASS:** same lifecycle and durable recovery checks against a unique temporary schema, removed after service shutdown |
| Dedicated system account and system-unit sandbox | **BLOCKED:** this environment has no `laso` account/group and no available privilege to create them or install/start the system manager unit. Rootless user units do not validate `User=`, `Group=`, `StateDirectory=`, or system-unit filesystem restrictions. |
| Child-process/orphan check | **PASS:** acceptance observed worker-host termination on restart/stop and no LASO/reference worker processes remained afterward |

The acceptance procedure is `tests/acceptance/systemd-lifecycle.sh`. Run
`--preflight <installed-unit>` for non-mutating systemd/unit checks; run
`--user <install-prefix> <source-tree>` for the rootless user-service lifecycle.
For PostgreSQL, set `LASO_SYSTEMD_ACCEPTANCE_POSTGRES_DSN` to a disposable test
database DSN and use `--user-postgres`; the harness creates and drops only its
unique test schema. These modes do not install users/groups or alter system
services. A privileged Linux validation environment must still install the
unit and verify the dedicated service identity and actual system-manager
filesystem sandbox before the full systemd deployment gap can be considered
closed.

Snapshot validated: 2026-09-18. Source review and packaging were performed in a
development environment. Native validation used an isolated Ubuntu 24.04.5 LTS
(x86-64) environment.

## Implemented

C++20 source, public headers, CMake targets, native C plugin SDK and examples,
SQLite persistence, runtime, API/CLI, policies, scheduling, event-source ingress,
and artifact interfaces, tests, systemd/Docker deployment files, documentation, and
Linux CI are present.

The current default build registers **182 GoogleTest cases** plus **3 CTest entries**
for CLI validation, process smoke/restart, and the SQLite multi-instance guard, for
**185 CTest entries**. The PostgreSQL-enabled build registers **198 GoogleTest
cases** plus the same **3 CTest entries**, for **201 CTest entries**. Composition,
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

## Tested in the development environment

| Check | Result |
|---|---|
| Native clang-format | **PASS: clang-format-18 check executed on Linux; no violations** |
| CMake source paths | PASS: all referenced source files present |
| LASO include resolution and documentation links | PASS |
| Safe YAML/configuration document scan | PASS: 13 documents |
| Git whitespace check | PASS |
| Private terminology and obvious credential-pattern scan | PASS: no matches |

No non-Linux C++ compilation was attempted because LASO is intentionally Linux-only.

## Tested on Ubuntu 24.04.5 x86-64

| Check | Result |
|---|---|
| CMake 3.28.3 + Ninja configure | **PASS** |
| GCC 13.3.0 Debug build | **PASS** |
| Clang 18.1.3 Debug build | **PASS** |
| GCC Debug CTest suite | **PASS: 201/201; all PostgreSQL and distributed-execution tests ran against isolated PostgreSQL 16** |
| GCC Release CTest suite | **PASS: 201/201; all PostgreSQL and distributed-execution tests ran against isolated PostgreSQL 16** |
| Clang Debug CTest suite | **PASS: 201/201; all PostgreSQL and distributed-execution tests ran against isolated PostgreSQL 16** |
| Clang Release CTest suite | **PASS: 201/201; all PostgreSQL and distributed-execution tests ran against isolated PostgreSQL 16** |
| ASan + UBSan build and CTest, leak detection enabled | **PASS: 185/185 scheduled; 182 passed and 3 expected PostgreSQL-disabled cases skipped** |
| PostgreSQL-enabled GCC Debug CTest suite | **PASS: 201/201; all PostgreSQL storage, coordination, and distributed-execution tests ran against isolated PostgreSQL 16** |
| clang-format `--dry-run --Werror` on Linux | **PASS** |
| clang-tidy 18 against the Clang compilation database | **PASS: exit 0; advisory warnings remain** |
| Debian 13 container | **PASS: public workflow 35380101673** |
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
| Dedicated-account systemd installation and system-unit sandbox | **BLOCKED:** rootless lifecycle coverage passed; privileged system-manager install, account ownership, and `StateDirectory` sandbox remain unvalidated (details above) |
| GitHub Actions execution | **PASS: public workflow 35380101673; GCC, Clang, Debian, ASan/UBSan, formatting, clang-tidy, and PostgreSQL jobs succeeded** |
| Optional TSan execution | **BLOCKED ON HOST: GCC runtime aborted during test discovery with `unexpected memory mapping`** |

Use [the Linux validation procedure](docs/first-linux-validation.md) when validating
another distribution or deployment environment. Ubuntu and Debian results above
are actual executions; pending and blocked rows do not imply success. The TSan
failure occurred before LASO tests ran and must be repeated on a compatible kernel
and sanitizer runtime; global ASLR settings were not weakened to work around it.

## Distributed execution milestone validation

The Milestone 2 implementation was validated from the current source tree on an
isolated Linux x86-64 host. The PostgreSQL-enabled matrix used a disposable
PostgreSQL 16 instance; the default and sanitizer matrices kept PostgreSQL
disabled and verified that the SQLite build remains independent of libpq.

- SQLite/default GCC Debug: **185/185 scheduled; 182 passed and 3 expected
  PostgreSQL-disabled cases skipped**.
- PostgreSQL GCC Debug and Release: **201/201 passed**.
- PostgreSQL Clang Debug and Release: **201/201 passed**.
- ASan + UBSan: **185/185 scheduled; 182 passed and 3 expected
  PostgreSQL-disabled cases skipped** with leak detection enabled.
- The focused distributed tests cover run-owner takeover, durable `NodeWork`
  claiming, fencing, retries with distinct attempt IDs, cancellation, approval
  resume, process-crash recovery, and SQLite multi-instance rejection.
- Storage failure handling is fail-closed: lease renewal and authoritative
  node mutations stop when coordination/storage errors prevent proof of current
  ownership. Connection failure is bounded and redacted by the PostgreSQL pool;
  a live database-partition test remains an operational exercise rather than a
  claim of exactly-once execution.
- `clang-format-18 --dry-run --Werror` completed successfully after formatting
  six distributed-execution source/test files. The repository clang-tidy command
  completed with exit 0; its remaining output is advisory, non-LASO-owned or
  intentionally non-fatal baseline guidance.

## Historical validation snapshots

The following sections preserve earlier milestone evidence and are intentionally
historical. Their older test counts and branch names are not the current release-
readiness result above.

## Worker-hardening branch validation

This branch was validated in an isolated Linux x86-64 environment from upstream
commit `c9887eabd585a414789d6c43514e1ee3a221ca8b` using GCC Debug, an isolated
local PostgreSQL 16 cluster, and serial Ninja builds. SQLite CTest ran **179
tests: 175 passed and 4 skipped** (the PostgreSQL cases and gated real OpenCode
cases); PostgreSQL-enabled CTest completed **179/179** (the two gated real
OpenCode cases skipped); and ASan/UBSan CTest ran **179 tests: 175 passed and 4
skipped** with leak detection enabled. The added worker tests cover durable
approval/permission/question requests, policy decisions, idempotent replay,
cancellation, strict protocol bounds, OpenCode session continuation after
adapter restart, explicit project-root rejection, and normalized results and
usage. The real OpenCode test used the installed adapter and a disposable
synthetic workspace; it did not require a paid provider for the test suite.
OpenCode `1.18.29` emitted a real `permission.asked` event. LASO persisted the
request, resolved it through the worker-request API, and the OpenCode turn,
WorkerNode, and enclosing pipeline completed with the expected synthetic result.
The OpenCode question path remains not directly validated.

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

## Current optional Codex-adapter validation

The final local Codex matrix for this branch is: default SQLite Debug CTest
**180 total, 178 passed, 2 PostgreSQL cases skipped**; PostgreSQL-enabled Debug
CTest **188/188 passed**; and ASan/UBSan CTest **188 total, 186 passed, 2
PostgreSQL cases skipped**. The PostgreSQL and sanitizer runs both included the
opt-in real Codex fixture test.

This branch adds seven deterministic and one separately gated Codex adapter test
to the existing suite. The default build remains Codex-free with 180 CTest
entries. With `-DLASO_BUILD_CODEX_ADAPTER=ON`, the fixture-only suite contains
187 entries and the full optional registration contains 188 entries including
the gated real-installation test. The seven deterministic tests use a local
app-server-shaped fixture and cover structured startup, session follow-up,
adapter restart/resume, project-root rejection, LASO permission forwarding and
denial, question forwarding, and malformed-protocol failure. They do not require
an account or network access.

The real installation test is separately gated with
`LASO_RUN_REAL_CODEX=1`, uses a temporary fixture project, and is not counted as
passing unless it is explicitly run. It exercises the installed Codex
app-server, session capture, a bounded file edit, follow-up, and adapter
restart/resume. Its result must be recorded from the actual run; a skipped
gated test is not a pass. The adapter targets the structured app-server
interface observed in Codex CLI 0.154.0. The explicit real test passed in
19.08 seconds, including a temporary fixture edit, follow-up, and
adapter restart/resume. It is not part of the default or fixture-only totals.

## Claude worker validation snapshot

This snapshot covers the optional Claude Code adapter. The adapter is disabled
in the default build and the real-provider test is gated; no Claude executable
was available in the validation environment.

| Configuration | Result |
|---|---|
| Claude-enabled Debug | **PASS: 189 scheduled; 186 passed and 3 expected cases skipped (2 PostgreSQL, 1 real Claude)** |
| PostgreSQL + Claude Debug | **PASS: 189 scheduled; 188 passed and 1 real-Claude case skipped; PostgreSQL cases executed** |
| Claude-enabled ASan/UBSan | **PASS: 189 scheduled; 186 passed and 3 expected cases skipped (2 PostgreSQL, 1 real Claude)** |
| Claude deterministic adapter tests | **PASS: 8/8 executed; session resume, project/symlink boundaries, interactions, failures, cancellation truthfulness, and bounded process cleanup** |
| Real Claude Code integration | **SKIPPED: no installed Claude Code executable; no provider credentials were changed** |

The deterministic fixture emits Claude-shaped structured `stream-json` system,
assistant, result, and control messages without a network or account. The
vendor-specific permission/question path remains version-dependent because the
headless Claude CLI does not guarantee that every interaction is exposed to a
custom stream host. No real Claude session or provider transcript was used in
this validation.

## PostgreSQL coordination validation snapshot

The PostgreSQL-enabled build scheduled **194/194 CTest entries** against an
isolated PostgreSQL 16 service. Coordination coverage includes bounded pool
acquisition and replacement, opaque instance IDs, renewal, owner binding,
expiry takeover, fencing rejection, concurrent takeover, and process-exit
recovery. The distributed execution integration also exercises two LASO service
instances sharing PostgreSQL, a durable queued run, a single owner claim, a
multi-instance subpipeline with `max_runs: 1`, and normal runtime completion.
Multi-instance mode is not used by SQLite.

## Real-agent Phase 1.1 runtime-hardening acceptance (2026-09-19)

The manual acceptance harness in `tests/acceptance/` uses a disposable synthetic
C++ repository and the normal LASO HTTP API, worker adapters, durable SQLite
state, and process-worker isolation. Its pipeline runs Codex and OpenCode in
parallel with a deterministic validator, joins the branches deterministically,
waits for a reviewer approval checkpoint, and then builds and tests the
resulting workspace. The harness also includes an allowed-root failure case and
an in-flight cancellation case. It is intentionally opt-in because it invokes
real locally installed agent executables.

Phase 1.1 hardens the provider lifecycle at the framework and adapter
boundaries. Process-worker invocations now own an exact process group, use
bounded graceful-then-forced teardown for timeout/cancellation, and expose a
pending-submission cancellation hook. The Codex and OpenCode adapter hosts
also use parent-death cleanup; OpenCode explicitly stops its server before
acknowledging shutdown. Worker-node deadlines are propagated to process
transport requests and timeout results remain distinguishable from ordinary
provider failures.

The completed acceptance run exercised real Codex and OpenCode workers, two
successful repeated pipeline runs, parallel filesystem edits, durable worker
attempts, deterministic joining, reviewer approval, controlled orchestrator
termination and restart, recovery from the persisted checkpoint, and the
synthetic project's build and CTest suite. The final harness result was
`REAL_AGENT_PHASE1_OK` with `successful_repeated_runs=2`,
`cancellation=acknowledged`, and `build_and_tests=pass`.

The prior real OpenCode gate left three provider-server child processes. After
the lifecycle fix, the normal, failure, timeout, cancellation, and shutdown
regressions found zero LASO-owned provider descendants/listeners after
completion. Cleanup after a hard orchestrator kill is best-effort when a
provider daemonizes or escapes its owned process group; this is documented as
a future reaper/lease boundary rather than an unconditional guarantee.

Cancellation is durable and terminal-state protected. If cancellation wins,
LASO records the request and acknowledgement and commits `Cancelled` (or
`TimedOut` for an acknowledged deadline). If provider completion wins first,
the completed result remains authoritative even if a cancellation request was
also recorded. A late completion cannot rewrite a terminal cancelled or
completed job. The regression suite also covers cancellation during pending
submission and provider completion racing acknowledgement.

The real-provider gates passed for Codex (1/1) and OpenCode (2/2). Claude was
not invoked because no Claude executable was safely available. The failure
boundary used an out-of-allowed-root workspace and retained the detailed
failure on the durable worker job without exposing it through the redacted run
summary.

| Validation | Result |
|---|---|
| GCC Debug with Codex/OpenCode adapters | **PASS: 200 scheduled; 193 passed and 7 expected cases skipped** |
| GCC Release with Codex/OpenCode adapters | **PASS: 200 scheduled; 193 passed and 7 expected cases skipped** |
| ASan/UBSan Debug | **PASS: 189 scheduled; 186 passed and 3 PostgreSQL-related cases skipped** |
| Real Codex adapter gate | **PASS: 1/1** |
| Real OpenCode adapter gate | **PASS: 2/2** |
| Real Claude adapter | **SKIPPED: executable unavailable; no credentials changed** |
| PostgreSQL-backed validation | **BLOCKED: PostgreSQL server/tools unavailable on the execution host** |
| clang-format / clang-tidy | **BLOCKED: tools unavailable on the execution host** |

The harness documents the remaining Milestone 3 distributed-execution needs:
capability advertisement, remote task claims with lease expiry and fencing,
provider-aware remote cancellation handles, workspace/artifact transport,
retry and duplicate-completion idempotency, ownership-loss handling, and
explicit semantics that do not claim exactly-once execution across process or
network failure. The Phase 1.1 lifecycle hooks provide a local foundation but
do not by themselves make remote provider execution safe.

## Distributed Execution Milestone 3 implementation status (2026-09-19)

This milestone adds the first opt-in remote-agent foundation without broadening
distributed execution to arbitrary tools or side effects. Supported worker
branches now use the existing durable `NodeWork` record with persisted worker
ID/capability requirements. Multi-instance services advertise a bounded,
identity-free capability document and refresh it with their coordination
heartbeat. Claim selection checks local worker health and capability before
acquiring the node lease. Existing database-time lease and fencing predicates
remain authoritative; workers may physically continue after lease loss, but a
stale completion cannot commit.

Terminal `NodeWork` records now accept equivalent replay safely while rejecting
conflicting terminal rewrites. Cancelled parent runs permit a replacement
instance to claim still-running branch work for fenced cancellation cleanup,
including when the original worker disappeared. These semantics remain
at-least-once and do not claim exactly-once execution.

The first bounded workspace transport is an inline manifest of relative paths,
byte content, sizes, and SHA-256 hashes. It rejects traversal, absolute paths,
duplicate paths, symlinks, oversized files/workspaces, and integrity failures.
The remote worker stages it below its local LASO data directory per
run/work/attempt. Provider `project_dir` values are ephemeral and are removed
from distributed durable result metadata; validated output manifests are carried
through the deterministic join. Larger artifact/object-store transport and
remote provider-control protocols remain follow-on work.

## Historical M3 PostgreSQL validation pass before timeout closure

The current pass used a disposable PostgreSQL 16.15 instance bound to Linux
loopback only. LASO was compiled with PostgreSQL, Codex, and OpenCode support;
the coordination database tests ran against PostgreSQL rather than SQLite.
Credentials were supplied only through the test environment and are not part of
the repository.

| Validation | Result |
|---|---|
| SQLite/default regression matrix | **PASS: 219 total; 198 passed, 0 failed, 21 expected skips** |
| GCC Release regression matrix without PostgreSQL | **PASS: 220 total; 198 passed, 0 failed, 22 expected skips** |
| ASan/UBSan regression matrix | **PASS: 200 total; 196 passed, 0 failed, 4 expected skips** |
| PostgreSQL storage/coordination/distributed focus | **PASS: 42/42** |
| PostgreSQL-enabled full CTest excluding the real acceptance harness | **PASS: 215 passed, 4 expected provider-gate skips** |
| PostgreSQL-enabled full CTest including the real acceptance harness | **PARTIAL: 215 passed, 4 expected skips, 1 real-acceptance failure** |
| Real Codex adapter gate | **PASS: 1/1** |
| Real OpenCode adapter gate | **PASS: 3/3** |
| Real distributed basic runs | **PARTIAL: 5 attempted, 2 completed, 3 Codex-timeout runs failed/aborted and cleaned up** |

The 42 PostgreSQL-focused tests cover migrations/backend initialization,
concurrent claims, lease renewal and expiry, stale completion fencing,
equivalent and conflicting duplicate completion, cancellation recovery,
owner/process recovery, and SQLite multi-instance rejection. The two completed
real runs used separate owner and worker LASO instances sharing PostgreSQL on
the validation host; both real Codex and OpenCode work executed on the worker
instance and returned through the durable join.

The remaining real-acceptance failure is an operational boundary, not a green
result hidden by the harness: the Codex provider exceeded its configured
distributed request window, leaving the run paused with an unfinished NodeWork
while the OpenCode branch had completed. The harness terminated its owned
instances and left zero provider processes/listeners. A work-computer-owner to
Linux-worker run, live stale-worker/database-interruption chaos, and real remote
cancellation remain unvalidated in this pass. The implementation retains
at-least-once semantics and one authoritative fenced completion; it does not
claim exactly-once execution. SQLite remains explicitly single-instance.

## M3 closure validation pass (2026-09-21)

The timeout/reconciliation defect was fixed and then revalidated against the
real PostgreSQL-backed runtime. The provider transport had been converting a
quiet but valid provider into a false timeout because its polling interval was
limited to 60 seconds even when the overall request deadline was longer. The
transport now waits against the actual remaining deadline, bounded only by the
platform integer limit. A quiet-provider regression now runs past that interval
and completes successfully.

The runtime also reconciles terminal worker attempts from durable state. A
terminal success, provider failure, timeout, cancellation, or fenced ownership
loss is mapped to its current `NodeWork`; retry policy is evaluated and the
logical work is committed as completed, retryable, failed, cancelled, or
superseded. Recovery does not depend on an in-memory callback or a surviving
owner process. Temporary PostgreSQL storage failures are deferred while a
currently valid lease remains authoritative; they do not become false terminal
worker failures. Pending cancellation and synchronous submission races now
wait for the durable terminal cancellation state before a transport failure can
be persisted.

The final disposable PostgreSQL environment was PostgreSQL 16.15, bound to
loopback only and reached from the work-computer owner through a loopback SSH
forward. The build used libpq 16.15 and libpqxx 7.8.1. The database used a
dedicated synthetic test role/database and was removed after validation; no
production or LAN/public database endpoint was used.

The true cross-machine gate ran the owner on the authoritative work computer
and the worker/provider on a separate Linux test workstation. Codex execution
was observed on the worker side, with no owner-local provider fallback. Three
complete Codex runs passed consecutively. The owner verified remote artifact
identity, attempt/fence provenance, relative paths, sizes, SHA-256 hashes, and
content before rebuilding and testing the reconstructed synthetic project.

The required cross-machine chaos gates also passed: remote cancellation,
worker death and lease recovery, a live stale worker after lease expiry,
owner death and recovery, and bounded worker-side database interruption. The
stale-worker gate produced `STALE_RESULT_REJECTED`; no stale worker committed
authoritative state. Remote cancellation reached authoritative `Cancelled`.
The provider termination acknowledgement was false with a recorded pending
cancellation error, so the acceptance did not claim termination confirmation
that was not observed. Late completion remained non-authoritative.

| Validation | Result |
|---|---|
| PostgreSQL GCC Debug full CTest, excluding opt-in acceptance harness | **PASS: 223/223; 4 expected skips** |
| PostgreSQL GCC Release full CTest, excluding opt-in acceptance harness | **PASS: 223/223; 4 expected skips** |
| ASan + UBSan SQLite-only CTest | **PASS: 206/206; 8 expected skips** |
| PostgreSQL focused distributed/storage/coordination coverage | **PASS: 42/42** |
| Quiet-provider overall-deadline regression | **PASS: 1/1** |
| Pending-submission cancellation race repetition | **PASS: 20/20** |
| PostgreSQL schema upgrade v7 to current v8 | **PASS in Debug and Release** |
| Complete cross-machine Codex runs | **PASS: 3/3** |
| Cross-machine cancellation | **PASS: 2/2; termination acknowledgement accurately unconfirmed** |
| Cross-machine worker death, stale worker, owner death, DB interruption | **PASS: 1/1 each** |
| Cross-machine owner-side artifact rebuild and tests | **PASS for all 3 complete runs** |
| Cross-machine OpenCode | **SKIPPED: executable unavailable on the Linux test host** |
| Real Claude | **SKIPPED: executable unavailable; no credentials changed** |
| Clang, clang-format, clang-tidy, TSAN | **SKIPPED: unavailable or environment-blocked** |

The final cleanup inspection found zero LASO-owned provider processes,
provider listeners, and acceptance staging workspaces. The acceptance harness
cleanup is scoped to exact LASO-owned process trees and uses bounded escalation;
it does not kill by provider name. PostgreSQL and the SSH forwarding resources
were removed after the run.

This pass preserves the distributed execution contract: attempts may execute
at least once and may be duplicated after failure or lease expiry, while only a
valid current fence can commit one authoritative result. LASO does not claim
exactly-once execution. SQLite remains explicitly single-instance, and remote
execution remains limited to the supported agent/worker path.

## M3.6 artifact transport status — partially validated

Artifact transport is implemented and substantially validated. Artifact-specific
cross-machine chaos testing remains in progress. The results below are the
existing acceptance evidence; they are not a claim that the remaining chaos
scenarios passed.

| Artifact validation | Result |
|---|---|
| 64 MiB object-backed cross-machine transfer | **PASS** |
| Approximate materialization peak RSS for the 64 MiB object | **~13 MiB** |
| Deterministic cross-machine object-backed runs | **PASS: 3/3** |
| OpenCode 1.18.29 object-backed runs | **PASS: 3/3** |
| Clean-clone SQLite artifact workflow, including generated 64 MiB put/download/hash verification, listing, integrity, and GC dry-run | **PASS** |
| Clean-clone PostgreSQL distributed artifact example | **PASS: `state=Completed`, `remote_artifact=owner_verified`** |
| PostgreSQL Debug matrix | **PASS: 233 total; 228 passed, 0 failed, 5 expected skips** |
| Focused PostgreSQL matrix | **PASS: 44/44** |
| Artifact-focused tests | **PASS: 6/6** |
| SQLite Debug matrix | **PASS: 202 total; 198 passed, 0 failed, 4 expected skips** |
| Release matrix | **PASS: 202 total; 198 passed, 0 failed, 4 expected skips** |
| ASan/UBSan equivalent matrix | **PASS; expected PostgreSQL skips; no actionable sanitizer failures reported** |

The following artifact-specific acceptance work remains open:

- [ ] Worker death after artifact materialization
- [ ] Worker death during output upload
- [ ] Worker death after publication/pre-completion
- [ ] Network-separated stale artifact completion
- [ ] Artifact-stage PostgreSQL interruption
- [ ] Full owner-restart artifact recovery
- [ ] Diagnose remote worker health/heartbeat stall

In the latest closure attempt, the remote worker stopped advancing its health/
heartbeat and did not claim queued work, so the requested artifact chaos barriers
were not reached. The root cause is not yet established; it has not been
classified as a LASO, network, or environment defect. These cases remain
follow-up validation, not passed tests. The earlier distributed-runtime
cancellation, lease, owner recovery, and database interruption results above
refer to that runtime acceptance scope and do not substitute for these
artifact-specific scenarios.

The artifact gateway currently uses a deployment-local bearer token. SQLite
remains single-instance; S3 storage and arbitrary remote side-effecting tools
are not implemented. LASO provides at-least-once attempt semantics with one
authoritative fenced completion, not exactly-once execution.
