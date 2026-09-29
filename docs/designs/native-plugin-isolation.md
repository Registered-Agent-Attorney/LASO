# Native plugin isolation design

Status: design only. This document does not claim that process workers or native
plugins are sandboxed. The current C ABI loads code into the LASO process with
`dlopen`; a plugin constructor or callback can use every privilege available to
LASO. Continue to load only deployment-trusted native plugins.

## Goal and scope

Move plugins that are not trusted as LASO-process code behind an operating
system process boundary while preserving the existing C ABI for explicitly
trusted, high-performance plugins. Keep orchestration, durable state, policy,
leases, fencing, approvals, and recovery in LASO. A plugin host may perform only
the component operations granted to its configured plugin identity.

The first implementation should target worker components. `ProcessWorkerTransport`
already implements the `WorkerTransport` lifecycle and job operations, and the
local worker protocol already has a versioned hello, submit, status, result,
cancel, shutdown, bounded NDJSON frames, correlated human-interaction messages,
and explicit child supervision. This is a close fit for worker plugins.

That transport is not a drop-in replacement for tool, model, or event ABI
components. Those APIs have different synchronous-call, generation, and event
registration contracts. Reuse the process launcher, framing, redaction, and
supervision primitives, then add separately versioned type-specific operations
only where their semantics are clear. Do not map arbitrary tool invocation to
the worker job protocol or move orchestration into a plugin host.

## Identity and configuration

Each isolated plugin needs a stable LASO-assigned plugin ID, component kind,
configured executable, configured version, and executable digest. The
deployment configuration, not plugin-supplied metadata, binds those values to
the process. The hello response must repeat the assigned ID, component kind,
protocol version, and supported capabilities; mismatches fail startup.

Configuration names the executable by absolute path and carries a narrow
argument vector, explicit environment allowlist, resource profile, and granted
LASO capabilities. It must not use a shell, PATH lookup, inherited environment,
plugin-selected executable, or plugin-provided filesystem path. Native library
discovery remains a separate, explicit trusted mode.

## Protocol and bounded data

Start with worker protocol v1 for worker components. Negotiate the protocol
version and operation set during hello. Reject unknown required capabilities,
duplicate or stale request IDs, unsolicited messages, malformed frames, and
responses that do not match the outstanding request. Additive fields may be
ignored only when the negotiated protocol declares them optional. A future
incompatible change requires a new protocol version and a tested compatibility
matrix; it must not silently reinterpret the C ABI.

Retain hard limits for frame bytes, JSON nesting/field counts, stderr capture,
artifact references, interaction text and payload, metadata, concurrent requests,
startup, operation, and cancellation deadlines. Apply separate input and output
budgets. Do not send prompts or message bodies to operator logs. Transfer
artifacts by validated content IDs and bounded artifact operations rather than
arbitrary host paths. Every limit must have a negative test at its boundary.

## Lifetime, cancellation, and failures

The LASO service owns each child process and its process group. Start it only
after configuration validation and stop it before its transport and callbacks
are destroyed. A worker process is reused only according to its declared
concurrency contract; the default is one in-flight protocol operation per
process. Restarting LASO reopens the configured executable and reconstructs
worker health from durable LASO job state; it does not trust an in-memory child
handle as durable state.

Cancellation first sends the protocol cancel operation and waits only for a
configured grace interval. If the owned child does not exit, terminate its
process group, wait a bounded interval, then force-kill only that owned group.
Shutdown, startup timeout, malformed protocol, and service restart follow the
same child ownership rules. A crash becomes a bounded transport failure and a
durable failed/reconcilable job transition through the existing
`WorkerManager`; it must not bypass lease checks, fencing tokens, idempotency,
or terminal-state immutability. Provider side effects remain at least once.

## Permissions, environment, filesystem, and resources

The protocol grants operations, not ambient LASO authority. A plugin receives
only its configured worker ID, job context, deadlines, approved interaction
channel, and artifact references. Secrets are absent by default. Environment
variables require an explicit per-plugin allowlist and values come from the
deployment secret provider; they are never copied from the LASO process
environment wholesale.

The initial contract grants no direct host filesystem, workspace, device, or
network access. If a plugin needs workspace input, LASO creates a per-job
staging view from a validated manifest and exposes only that view through an
explicit transport capability. Any eventual filesystem access must be
read-only or confined to an owned scratch directory and tested against traversal
and symlink attacks. Network access is denied by default and requires a
documented per-plugin egress policy.

Process separation alone is not an OS sandbox. Before calling a mode sandboxed,
the implementation must enforce and test a concrete Linux boundary, such as a
dedicated unprivileged account plus systemd/cgroup controls, private mount and
network namespaces where supported, read-only system files, a private temporary
directory, `NoNewPrivileges`, syscall policy, and CPU, memory, process, file,
and I/O limits. Host preflight must fail closed when the required controls are
unavailable. It must not weaken host-wide security settings.

## Logging, provenance, and upgrades

Keep stderr capture bounded and redact credentials, environment values,
provider-native IDs, prompt-like values, and absolute sensitive paths before
logs or operator views. Record only plugin ID/version/digest, protocol version,
job ID, attempt, timestamps, normalized health/failure state, and termination
reason in operator summaries. Retain detailed diagnostics only in a deployment
controlled channel with explicit access policy.

Pin every job attempt to the configured plugin ID, digest, and protocol version.
Do not hot-unload or replace a live process. Upgrades quiesce new assignments,
drain or cancel existing work using normal LASO policy, start the new executable,
verify its handshake, and then enable assignments. On startup failure, keep the
plugin disabled and preserve durable jobs for operator reconciliation. Rollback
uses the prior executable digest and the existing at-least-once recovery rules;
it does not roll back PostgreSQL schema or durable state.

## Existing C ABI compatibility

Keep ABI v1 source and binary compatibility for trusted native mode. Do not
change callback ownership, descriptor layout, event lifetime, or plugin
registration semantics to implement process isolation. Existing native plugins
remain explicitly privileged and are not made safer by passing the same JSON
messages through an in-process wrapper. Migration is a deployment choice per
plugin: retain trusted `dlopen`, adopt the process worker protocol for a worker,
or wait for a reviewed type-specific process protocol for tool/model/event
components.

## Validation gates

Before enabling isolated plugin mode, add protocol tests for version mismatch,
malformed/oversized frames, duplicate requests, cancellation deadlines, child
crashes, forced cleanup, restart recovery, and stale fenced completion. Add
system acceptance for environment allowlists, filesystem and network denial,
resource ceilings, log redaction, executable digest mismatch, upgrade/drain,
failed upgrade, and rollback. Run those tests under the exact OS isolation
profile advertised by the deployment. Until all gates pass, describe the mode as
supervised process execution, not sandboxing.
