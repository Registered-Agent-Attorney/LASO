# Validation record

Snapshot validated: 2026-09-16. The Windows development workstation was used for
source review and packaging. Native validation was performed over SSH in an isolated
directory on a remote Ubuntu 24.04.5 LTS (x86-64) host.

## Implemented

C++20 source, public headers, CMake targets, native C plugin SDK and examples,
SQLite persistence, runtime, API/CLI, policies, scheduling and artifact interfaces,
tests, systemd/Docker deployment files, documentation, and Linux CI are present.

The test inventory contains **125 GoogleTest cases** plus **2 CTest entries** for CLI
validation and a process smoke/restart scenario, for **127 CTest entries**. Composition and
storage coverage includes
revision immutability, cross-boundary payload and schema behavior, child retry
identity, approval-compatible persistence, parallel children, recursion, depth,
run inspection, backend conformance, pagination, rollback, concurrent persistence,
and PostgreSQL runtime/reopen behavior.

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
| clang-format 18.1.8, `--dry-run --Werror` | PASS: 48 C/C++ files |
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
| GCC Debug CTest suite | **PASS: 127/127** |
| GCC Release CTest suite | **PASS: 127/127** |
| Clang Debug CTest suite | **PASS: 127/127** |
| Clang Release CTest suite | **PASS: 127/127** |
| ASan + UBSan build and CTest, leak detection enabled | **PASS: 127/127** |
| PostgreSQL-enabled GCC Debug CTest suite | **PASS: 127/127; PostgreSQL tests executed against isolated PostgreSQL 16** |
| clang-format `--dry-run --Werror` on Linux | **PASS** |
| clang-tidy 18 against the Clang compilation database | **PASS: exit 0; advisory warnings remain** |
| Debian 13 container, GCC 14.2 Debug build | **PASS** |
| Debian 13 container CTest suite | **PASS: 127/127 in the public Debian 13 container job; Docker was unavailable on server1300 for a duplicate local run** |
| Multi-stage Debian runtime image build | **PASS** |
| Runtime image health endpoint and unprivileged UID | **PASS: host-network health endpoint; image runs as `laso:laso`** |
| systemd unit syntax and dependency verification | **PASS** |
| CLI and process restart smoke tests | **PASS** |
| Offline shipped examples | **PASS: deterministic, schema, approval, plugin, and composition examples; optional local-openai not run** |
| HTTP health/run integration tests | **PASS** |
| valid, invalid, incompatible, and symlinked `.so` plugin tests | **PASS** |
| Loopback-only local model provider and GPU inference | **PASS: completed LASO agent run with GPU memory allocation and active utilization observed** |

clang-tidy reported advisory findings because `WarningsAsErrors` is intentionally
empty in the v0.1 baseline. They include enum-size suggestions, explicit handling
of ignored networking return values and exception boundaries, integer widening,
and small copy/allocation opportunities. The configured CI command exits zero.

## Requires further Linux validation

| Check | Status |
|---|---|
| Full systemd installation, privilege setup, and shutdown behavior | **PENDING** |
| GitHub Actions execution | **PASS: run `35135035917` completed all Ubuntu GCC/Clang, Debian, ASan/UBSan, and PostgreSQL jobs successfully** |
| Optional TSan execution | **BLOCKED ON HOST: GCC runtime aborted during test discovery with `unexpected memory mapping`** |

Use [the Linux validation procedure](docs/first-linux-validation.md) when validating
another distribution or deployment environment. Ubuntu and Debian results above
are actual executions; pending and blocked rows do not imply success. The TSan
failure occurred before LASO tests ran and must be repeated on a compatible kernel
and sanitizer runtime; global ASLR settings were not weakened to work around it.

The schema-contract tests additionally cover valid and invalid input/output,
registration-time missing or malformed schemas, safe local references, forbidden
remote references, traversal rejection, payload limits, explicit ValidatorNode
use, and concurrent cache access.

The shared storage conformance tests run against SQLite on every default build and
against a real disposable PostgreSQL service when `LASO_TEST_POSTGRES_DSN` is set.
They cover immutable pipeline revisions, pagination, invalid-record rejection,
structured operational records, rollback boundaries, concurrent writes, and
restart/reopen recovery.
