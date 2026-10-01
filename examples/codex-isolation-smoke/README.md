# Three isolated Codex workers

`pipeline.yaml` runs three Codex tasks at one bounded parallel fork. Each node
uses its own process-backed worker ID. A single Codex worker process holds one
app-server thread at a time, so pointing several parallel nodes at the same
worker can reuse one session and does not provide agent isolation.

Configure the three workers as separate process entries. Keep the global job
limit at three and each worker's job limit at one for a three-agent run:

```yaml
max_worker_jobs: 3
max_worker_jobs_per_worker: 1
process_workers:
  codex_agent_one:
    executable: /opt/laso-core/current/bin/laso-codex-worker
    args: [--worker-id, codex_agent_one, --codex,
           /opt/laso-core/current/bin/laso-codex-lock, --allowed-root,
           /var/lib/laso-core-test/workspaces, --timeout-ms, "180000"]
    environment_allowlist: [HOME, CODEX_HOME, PATH]
    startup_timeout_ms: 30000
    request_timeout_ms: 180000
    interaction_timeout_ms: 300000
  codex_agent_two:
    executable: /opt/laso-core/current/bin/laso-codex-worker
    args: [--worker-id, codex_agent_two, --codex,
           /opt/laso-core/current/bin/laso-codex-lock, --allowed-root,
           /var/lib/laso-core-test/workspaces, --timeout-ms, "180000"]
    environment_allowlist: [HOME, CODEX_HOME, PATH]
    startup_timeout_ms: 30000
    request_timeout_ms: 180000
    interaction_timeout_ms: 300000
  codex_agent_three:
    executable: /opt/laso-core/current/bin/laso-codex-worker
    args: [--worker-id, codex_agent_three, --codex,
           /opt/laso-core/current/bin/laso-codex-lock, --allowed-root,
           /var/lib/laso-core-test/workspaces, --timeout-ms, "180000"]
    environment_allowlist: [HOME, CODEX_HOME, PATH]
    startup_timeout_ms: 30000
    request_timeout_ms: 180000
    interaction_timeout_ms: 300000
```

Keep the existing approved Codex model guard and default-deny Computer policy.
The smoke pipeline gives each worker a different harmless reasoning task and a
distinct expected marker without accessing files, the network, browser state,
or Microsoft data. Its results include each provider session ID so the caller
can verify that the sessions differ. Core also records when the matching
Codex app-server `turn/started` and `turn/completed` notifications reach the
worker; these UTC timestamps describe the observed Codex turn lifecycle.
