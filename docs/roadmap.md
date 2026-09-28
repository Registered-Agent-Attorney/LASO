# LASO milestone roadmap

## M3.6 — artifact transport: closed

M3.6 validated bounded content-addressed artifact transport across separate
Linux owner and worker OS/process/network boundaries. The accepted scope covered
PostgreSQL coordination, worker claims and heartbeats, shared artifact
transport, worker-loss and interrupted-upload recovery, stale-completion
fencing, PostgreSQL interruption, owner restart, and hash/size/provenance
verification. See [the validation record](../VALIDATION.md) for the evidence.

The implementation guarantees at-least-once attempts with one authoritative
fenced completion. It does not guarantee exactly-once execution or exactly-once
external side effects.

## M4 — production distributed artifact infrastructure

M4 improves the artifact infrastructure used by supported remote agent/worker
workloads. It does not expand distributed execution to arbitrary side-effecting
tools, general remote shell commands, or cluster scheduling.

### M4.1 — optional S3-compatible shared artifact store

Status: closed. Physical acceptance, hosted CI, merge, and post-merge smoke passed.
Merged as `fc26ea6ad67a5365d17cf22a164dd1e59c0d2fef`.

Add an optional S3-compatible object-store backend so owners and workers can
access content-addressed artifacts directly from shared object storage. The
filesystem backend remains the default and must not require cloud SDKs,
accounts, or credentials. Validation uses an isolated disposable S3-compatible
service, never production cloud credentials.

Physical acceptance has passed for backend failure handling, separate-machine
owner/worker execution, stale fencing, owner recovery, PostgreSQL interruption,
and trusted and untrusted TLS. The implementation and regression evidence are
recorded in [VALIDATION.md](../VALIDATION.md). Remote garbage collection
remains disabled unless it can be proven namespace-scoped, reference-aware, and
safe under pagination and concurrent writers.

## M5 — durable agent sessions

M5 adds persistent session identity and input delivery for applications that
need to address one logical agent session from multiple clients. Session event
sequences are durable and replayable; external applications retain user and
session authorization policy.

### M5.1 — durable input journal and replay

Status: closed. Merged in PR #13. Its durable session and event contract remains
the foundation for the later PostgreSQL-only implementation.

Provide durable session creation, bounded idempotent input acceptance, ordered
per-session events, close semantics, event replay, and resumable SSE. Acceptance
requires identical retries to return the original turn, conflicting reuse of an
idempotency key to fail, concurrent submissions to receive one ordered sequence,
closed sessions to reject new inputs, and the journal to survive service restart.
PostgreSQL multi-instance acceptance also requires an event written through one
service instance to be replayed and streamed by another. PostgreSQL is required
for single-owner and multi-instance deployments.

This milestone stores accepted inputs; it does not claim they have run. PR #13
adds no provider continuation state, turn dispatcher, execution lease, or worker
recovery for sessions.

### M5.2 — durable sequential turn execution and provider continuation

Status: implementation merged in PR #14. The PostgreSQL regression suite
covers ordered dispatch, run binding, recovery, fencing, cancellation, and
continuation publication. M6 adds real Codex continuation across a LASO restart
for one supported provider path. The system does not promise exactly-once
provider side effects; distributed fault injection remains limited to the cases
listed in [the validation record](../VALIDATION.md).

The durable state machine, run-binding transaction, execution ownership,
continuation boundary, and close/cancellation races are specified in
[m5-2-design.md](m5-2-design.md).

Accepted turns connect to LASO runs and the existing worker recovery and fencing
model. Per-session ordering and ownership, turn states, cancellation and close
races, provider continuation persistence, and lifecycle events are implemented.
Provider state remains opaque to clients and is protected from logs and public
artifacts. See [the M5.2 design](m5-2-design.md) and
[the session API](sessions.md).

The suite covers duplicate submission, two-instance claim contention, worker
and process recovery boundaries, stale-fence completion, provider timeout, and
replay of committed transitions. LASO may make at-least-once provider attempts
with one authoritative fenced completion; it does not promise exactly-once
external side effects.

### M5.3 — SSE admission and replay

Status: closed. Merged in PR #15 and included in the PostgreSQL session stack
landed by PR #21. The event journal supports numeric `Last-Event-ID` replay,
bounded stream admission, sanitized HTTP 429 responses, and `Retry-After`.
LASO-Web reconnects and reloads canonical session state from LASO.

### M5.4 — PostgreSQL storage and session context

Status: closed. PR #21 merged the validated PostgreSQL-only storage,
capability-discovery, context-provenance, immutable-generation, and opt-in
context-reduction work. PostgreSQL is required; SQLite storage has been
removed. Capability discovery is available from merged LASO `main`. Context
reduction remains opt-in and uses immutable generation records and run-context
snapshots; it does not replace durable turn history.

## M6 — real provider execution in durable sessions

Status: closed. Merged as PR #22 at `f78c1a98cc89f36f34903be1cee4119fe078fa31`;
exact-head CI and real-provider/Web acceptance passed. The Codex worker is the
canonical first adapter because its supported app-server protocol provides
structured start/resume/turn operations and a documented continuation boundary.
Claude Code and OpenCode remain optional adapters and are not accepted as
durable-session providers by this milestone.

The acceptance path completed three real provider-backed turns through a LASO
session, verified provider continuation after LASO and LASO-Web restarts, and
observed the same authoritative transcript through two independent Web clients.
The real-provider run also crossed an immutable context-generation boundary.
Browser acceptance verified SSE replay from `Last-Event-ID`. Provider process
death, stale completion, and cancellation races are covered by deterministic
worker fixtures and the existing fencing tests. Browser acceptance used two Web
instances against one LASO owner. A separate real-provider test used two LASO
instances and resumed the session through B after A stopped between turns; it
does not claim that an in-flight Codex operation can be transferred safely. See
[the M6 contract and validation](m6-real-provider-sessions.md).

## M7 — release candidate closure

Status: in progress. This is a release-readiness and feature-freeze milestone,
not a product-feature milestone. It closes only after clean installation,
existing-state upgrade, PostgreSQL backup/restore, process and provider
recovery, browser/SSE acceptance, bounded load, security review, exact-main CI,
and final clean-room validation have evidence recorded in `VALIDATION.md`.
Release tags and artifacts must identify the exact validated source commits.
Any uncompleted acceptance gate remains an explicit release limitation; it is
not marked complete by documentation alone.
