# Validation record

Snapshot validated: 2026-09-12. The Windows development workstation was used for
source review and packaging. Native validation was performed over SSH in an isolated
directory on a remote Ubuntu 24.04.5 LTS (x86-64) host.

## Implemented

C++20 source, public headers, CMake targets, native C plugin SDK and examples,
SQLite persistence, runtime, API/CLI, policies, scheduling and artifact interfaces,
tests, systemd/Docker deployment files, documentation, and Linux CI are present.

The test inventory contains **73 GoogleTest cases** plus **2 CTest entries** for CLI
validation and a process smoke/restart scenario.

## Statically reviewed

- All 24 source paths named by CMake exist; source/header references, namespaces,
  definitions, dependency discovery, and target boundaries were reviewed.
- GCC/Clang flags, C++20 requirements, runtime output paths, and the Linux-only
  platform check were reviewed.
- ABI version/size checks, C exports, buffer ownership, library lifetime, and
  explicitly configured plugin discovery paths were reviewed.
- Transaction checkpoints, approval/restart handling, cancellation persistence,
  finite edge/loop/retry limits, join state, and subpipeline waits were reviewed.
- HTTP isolation and limits, loopback defaults, policy checks, error/log contents,
  and deployment configuration were reviewed.
- Ubuntu GCC/Clang, Debian 13, clang-tidy, formatting, and ASan/UBSan CI jobs are
  defined in `.github/workflows/linux.yml`.

Static review found and corrected mismatched binding acceptance, fixture replacement
lengths, cancelled approval records, cancellation persistence, join resume state,
scheduler stop-before-start handling, completed-attempt recording on edge-budget
failure, a GoogleTest name lookup collision, and Boost discovery under CMake 3.31.

## Tested on this development workstation

| Check | Result |
|---|---|
| clang-format 18.1.8, `--dry-run --Werror` | PASS: 48 C/C++ files |
| CMake source paths | PASS: 24 referenced source files present |
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
| GCC CTest suite | **PASS: 75/75** |
| Clang CTest suite | **PASS: 75/75** |
| ASan + UBSan build and CTest, leak detection enabled | **PASS: 75/75** |
| clang-format `--dry-run --Werror` on Linux | **PASS** |
| clang-tidy 18 against the Clang compilation database | **PASS: exit 0; advisory warnings remain** |
| Debian 13 container, GCC 14.2 Debug build | **PASS** |
| Debian 13 container CTest suite | **PASS: 75/75** |
| Multi-stage Debian runtime image build | **PASS** |
| Runtime image health endpoint and unprivileged UID | **PASS: host-network health endpoint; image runs as `laso:laso`** |
| systemd unit syntax and dependency verification | **PASS** |
| CLI and process restart smoke tests | **PASS** |
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
| GitHub Actions execution | **PENDING: workflow configured but not published from this workspace** |
| Optional TSan execution | **BLOCKED ON HOST: GCC runtime aborted during test discovery with `unexpected memory mapping`** |

Use [the Linux validation procedure](docs/first-linux-validation.md) when validating
another distribution or deployment environment. Ubuntu and Debian results above
are actual executions; pending and blocked rows do not imply success. The TSan
failure occurred before LASO tests ran and must be repeated on a compatible kernel
and sanitizer runtime; global ASLR settings were not weakened to work around it.
