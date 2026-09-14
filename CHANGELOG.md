# Changelog

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
