# Codex worker adapter

`laso-codex-worker` is an optional coding-agent adapter outside LASO Core:

```text
WorkerNode -> WorkerManager -> ProcessWorkerTransport
           -> laso-codex-worker -> codex app-server (stdio JSON-RPC)
```

## Supported interface

The adapter targets the structured local app-server interface shipped with
Codex CLI 0.154.0 and later-compatible revisions. It uses JSON-RPC messages for
`initialize`, `thread/start`, `thread/resume`, `turn/start`, `turn/interrupt`,
and the experimental `item/tool/call` flow. It consumes structured thread,
turn, item, and token-usage notifications; it does not scrape terminal output.
The app-server interface is experimental in Codex, so deployments should pin
and validate their installed Codex version.

Build and install the adapter explicitly when the deployment uses it. Configure
the final install prefix before building so the generated systemd unit points at
the installed server path:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local -DLASO_BUILD_CODEX_ADAPTER=ON
cmake --build build --parallel 2
sudo cmake --install build
```

The default LASO build does not require Codex or build/install this executable.

## Configuration

Configure the generic process boundary with an explicit executable, Codex
executable, and allowed project root:

```yaml
process_workers:
  codex:
    executable: /srv/laso/bin/laso-codex-worker
    args:
      - --codex
      - /usr/local/bin/codex
      - --allowed-root
      - /srv/laso/workspaces
      - --timeout-ms
      - "120000"
    environment_allowlist: [HOME, CODEX_HOME, PATH]
    startup_timeout_ms: 120000
    request_timeout_ms: 120000
    interaction_timeout_ms: 300000
```

The outer process transport starts with an empty environment. Only explicitly
allowlisted variables and literal overrides are passed to the adapter and then
to Codex. Do not place credentials, prompts, session transcripts, or DSNs in
configuration, worker metadata, or logs.

Jobs normally provide `metadata.project_dir`. A durable-session job may omit it
when exactly one `--allowed-root` is configured; the adapter uses that root as
the session workspace. With multiple roots the caller must provide an explicit
project directory. The adapter canonicalizes the directory and rejects
traversal, missing paths, symlink escapes detectable by canonicalization, and
paths outside the configured roots. Existing explicit Git worktrees are
supported; automatic worktree creation is not performed.

## Sessions and recovery

The first turn creates a persistent Codex thread and returns its identifier in
the generic result and metadata. A later turn can provide
`metadata.codex_session_id` to use `thread/resume` and continue the same
conversation. The identifier is durable LASO metadata, not a claim of exactly
once external execution. After adapter or LASO restart, a known session can be
resumed and a new turn can continue it. An in-flight turn that cannot be
reconciled remains subject to LASO's conservative `Unknown` semantics and is
never silently resubmitted.

Results include the final agent message when available, session ID, model and
provider identity, bounded command/action metadata, project directory, turn
duration, and normalized token usage. Missing Codex metrics remain absent; LASO
does not invent pricing or billing data. Existing wall-time, token, cost-unit,
and job-count budgets apply through `WorkerManager`.

### Isolated parallel agents

Each adapter process owns one active Codex app-server thread and serializes its
worker protocol. Non-durable submissions start a fresh thread unless the
request explicitly supplies `metadata.codex_session_id` to resume one. Durable
LASO sessions continue only through their fenced continuation state.

For parallel independent agents, configure one process-backed Codex worker per
concurrently active agent, each with a unique `--worker-id`; see
[`examples/codex-isolation-smoke`](../examples/codex-isolation-smoke/README.md).
Set the global job cap to the intended number of agents and each worker's cap
to one. Several parallel nodes aimed at one Codex worker process will be
serialized and must not be treated as independent concurrent sessions.

### Durable LASO sessions

The adapter advertises the generic `session-continuation` and `session-context`
capabilities. LASO passes its private opaque continuation to the adapter and
stages the returned candidate in a private field of the durable worker-job
record. That closes the recovery gap if LASO restarts after the provider has
completed but before the run checkpoint. Worker-job API views redact the
candidate. Only the enclosing run's authoritative session fence can publish it
as current continuation; staging does not advance session state. In this mode,
the response includes safe text/model/provider fields only; it omits the Codex
thread ID, project path, and raw command/action details.
If Codex requests approval or input during a durable turn, the worker
interaction is linked to the LASO session and worker-job IDs. The provider
thread ID is kept out of that public interaction record.

After an adapter or LASO restart, the adapter opens a new Codex app-server
process and resumes the committed thread. When LASO selects a new immutable
context generation, the adapter starts a new thread and seeds it with the
generation payload plus the completed-turn tail after the generation
boundary. LASO continues to own full durable turn/event history. If the
provider outcome is ambiguous, the turn remains subject to LASO's conservative
`Unknown` recovery semantics and is not blindly resubmitted. This is not an
exactly-once side-effect guarantee.

## Approvals and questions

Codex app-server command-execution, file-change, additional-permission, and
user-input requests are translated into LASO's bounded generic
permission/approval/question channel. LASO policy or a durable operator
decision returns the correlated response; the Codex worker cannot approve its
own request. Unsupported app-server requests are rejected rather than treated
as approval.

## Bounded Windows Computer tool

Only a worker node authorized by validated pipeline configuration with
`required_tool: laso.browser_status` receives the experimental
`laso.browser_status({})` dynamic tool. Other Codex threads do not receive this
tool. Its schema has no arguments. Core accepts the namespace and tool only for
the authorized active worker job with an empty object, then maps it internally
to the configured `windows_computer` worker and `browser.status` capability.
It never uses model-supplied worker IDs, commands, prompts, or capability names.
Core fails closed unless the Computer worker is healthy and the deployment
policy allows that exact worker.

GPT-6 Luna's bundled Codex catalog selects Code Mode, which does not expose the
Core dynamic tool as a direct provider function. The Computer-capable worker
must therefore use the release-pinned
[`gpt-6-luna-high-direct-v0.156.1.json`](../deploy/codex/gpt-6-luna-high-direct-v0.156.1.json)
catalog at app-server startup. It contains only GPT-6 Luna, sets direct tool
mode, disables shell, search, native Codex agent collaboration, direct patching,
Node REPL metadata, image viewing, planning, and experimental tools, and exposes
only High reasoning.
Pass its absolute
path to `laso-codex-worker` with `--codex-model-catalog` for the worker assigned
to `required_tool: laso.browser_status`. The worker validates the catalog
before launching Codex and fails closed if the catalog is absent or differs
from that policy. Its `thread/start` disables Code Mode, shell, search, plugin,
app, experimental-request, and native-agent features and supplies only the
authorized `laso.browser_status({})` dynamic tool.

The catalog is pinned to Codex CLI 0.156.1 because `model_catalog_json` is a
process-start override and the catalog schema is version-specific. Update the
file from the matching Codex release catalog when the worker CLI version
changes; keep the same single-model, High-only, direct-tool constraints.

The paired worker environment must set LASO_CODEX_ALLOW_DIRECT_CATALOG=1 for
this Computer-capable worker only. The lock wrapper rejects the catalog
override without that opt-in.

The Computer request is a durable child `WorkerJob` in the parent run. Its
metadata records the parent worker-job ID, Codex session and turn IDs, and
dynamic-tool call ID. After terminal status, Core fetches the Computer result
and stores it on the child job before returning the JSON result to the same
Codex turn. Parent cancellation or deadline expiry cancels the child job. The
acceptance example is
[`codex-browser-status-pipeline.yaml`](../tests/acceptance/codex-browser-status-pipeline.yaml).

## Cancellation and security boundary

LASO cancellation calls `turn/interrupt` first and only reports the transport
acknowledgement returned by Codex. A lost or ambiguous turn is not relabeled as
successful or safely retried. Process-group cleanup prevents the supervised
adapter and its app-server child from being orphaned during normal shutdown or
bounded escalation.

This is supervised process isolation, not an OS or container sandbox. Codex is
trusted to operate within the explicitly selected project environment, while
LASO controls the adapter executable, argument vector, environment allowlist,
project-root boundary, protocol limits, policy decisions, and lifecycle.

The deterministic adapter fixture and mock tests run without a Codex account.
The real Codex test is opt-in (`LASO_RUN_REAL_CODEX=1`) and must use a temporary
fixture project. Remote workers, distributed leasing, other vendor adapters,
and SaaS billing remain outside this adapter.
