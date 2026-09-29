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

Status: closed. The durable execution implementation was merged in PR #14 and
the PostgreSQL storage/session integration landed in PR #21. Regression tests
cover ordered dispatch, run binding, recovery, fencing, cancellation, and
continuation publication. M6 proved one supported real-provider path with
Codex across a LASO restart. The system does not promise exactly-once provider
side effects, and in-flight Codex work is not transferred between LASO owners;
see [the validation record](../VALIDATION.md).

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

Status: closed for the `0.1.0-rc.2` release candidate. This was a release-
readiness and feature-freeze milestone, not a product-feature milestone.
Clean installation, upgrade from the prior merged main, PostgreSQL backup and
restore, process/provider recovery, real-provider sessions, browser/SSE
acceptance, bounded load, security review, exact-main CI, and clean-source
validation are recorded in [the validation record](../VALIDATION.md).
The release is a candidate for controlled pilot use, not a claim of general
production readiness. PostgreSQL downgrade rollback is not supported without
restoring a compatible database backup; TSan could not start in the validation
runtime; and the dedicated system-account filesystem sandbox was statically
checked but not exercised with a system-wide service. These boundaries remain
explicit release limitations.

## M8 — operator/admin and production boundary

M8 makes the shipped system easier to operate while keeping its trust and
recovery limits explicit. It is not a production-readiness declaration.

### M8.0 — repository truth and operator API support

Status: complete on public LASO main. Current documentation describes
PostgreSQL as the only supported database, records shipped RC2 work under the
`0.1.0-rc.2` changelog section, preserves dated validation history, and states
deployment, authentication, and plugin limits accurately. SQLite is retained
only as historical or migration context. The public Core also provides
bounded, metadata-safe operator views for the separate Web surface.

### M8.1 — separate LASO-Web operator/admin surface

Status: complete in the separate Go LASO-Web service. A distinct `/admin`
area provides sanitized, read-only operational metadata; normal chat remains
separate. Operator and admin principals may read the surface, while ordinary
users are denied. Admin mutations are withheld until LASO can durably attribute
the authenticated human principal; privileged mutation requests return a stable
`501` without reaching LASO. Remote administration fails closed unless a
deployment-owned trusted proxy supplies a trustworthy principal and role from a
configured direct peer. Explicit loopback development remains available, and
the Web application remains an API client and reverse-proxy. No durable Core
actor record or action audit is claimed. Durable actor propagation and full
OIDC/JWT integration remain follow-up work.

The operator contract excludes prompts, message bodies, provider-native
session IDs, credentials, DSNs, absolute sensitive paths, and arbitrary
provider metadata.

### M8.2 — native-plugin isolation design

Status: design complete on public LASO main; implementation remains future
work. See [plugin isolation design](designs/native-plugin-isolation.md). The
design evaluates reuse of the supervised process-worker boundary for worker
plugins and specifies protocol compatibility, lifetime, cancellation, crash
handling, bounded data, permission and environment allowlists,
workspace/filesystem access, resource limits, logging and redaction,
provenance and upgrades, plugin identity, and coexistence with the current C
ABI. Trusted native in-process plugins remain explicitly privileged. Process
separation alone is not described as sandboxing; an OS isolation boundary and
acceptance tests are required before making that claim.

M8 acceptance retains the PostgreSQL-only storage contract, the complete C++
regression matrix, durable fencing/idempotency/recovery behavior, existing
two-browser session coverage, and exact-head hosted validation. The alternate
TSan attempt stopped before test bodies with an unexpected-memory-mapping
runtime failure. Disposable user-manager systemd acceptance passed; the
dedicated system-account/system-wide sandbox remained unverified because
available host preflight required privileged system changes. No account or
system service was created. Neither TSan execution nor dedicated-account
sandbox acceptance is claimed.

## M9 — operational diagnostics and integration foundations

M9 follows the merged M8 operator/API work. The feature-gap audit found that
LASO-Web already has loopback-only runtime diagnostics and separate liveness /
readiness routes, while LASO Core exposed only process liveness and an
in-process SSE counter snapshot. M9.1 adds a Core readiness signal for
PostgreSQL/schema compatibility and runtime shutdown state, plus an authorization-aware,
low-cardinality Prometheus endpoint for API request latency/outcomes and
session-SSE admission. It does not claim provider, artifact backend, or worker
health through readiness, and it is not a complete tracing/metrics system.

Subsequent work should establish a versioned machine-readable API contract,
then advance the durable principal/audit boundary and out-of-process plugin
execution. Credentialed remote MCP and webhook integrations remain dependent on
a safe secret-reference contract and explicit network policy. Preserve the
distinction between a supervised child process and an OS-sandboxed plugin.

## M10 — coordinated drain and maintenance

The M10 development slice adds instance-scoped drain and maintenance control for safe rolling operations. Drain closes new submissions and ownership claims while valid owned work may finish and renew leases. PostgreSQL registry state prevents new cluster claims; the local desired state survives restart. Readiness reports non-ready while admission is closed, while liveness remains available. Operators can verify the derived drained state and `safe_to_stop` before using their service manager.

The transition record is a bounded, instance-local audit file; this is not a cluster-wide audit service. Forced termination remains governed by lease expiry and fencing and may cause at-least-once side effects. M10 does not claim zero downtime. See [coordinated drain](maintenance.md).
