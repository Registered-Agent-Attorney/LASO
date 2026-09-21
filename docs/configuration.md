# Configuration reference

Configuration is YAML. Values may also be supplied through `LASO_` environment
variables; environment names use uppercase field names, for example
`LASO_DATA_DIR` and `LASO_POSTGRES_DSN`. A value supplied through the explicit
CLI override takes precedence over the file and environment.

`config/laso.example.yaml` is a safe SQLite starting point. It contains no
credentials and binds the development API to loopback.

## Storage and execution

| Field | Default | Meaning |
|---|---:|---|
| `data_dir` | `.laso` | Runtime state and local artifacts. Keep it inside a dedicated service-owned directory. |
| `db_path` | `data_dir/laso.db` | SQLite database path when SQLite is selected. |
| `storage_backend` | `sqlite` | `sqlite` or `postgres`. PostgreSQL requires a PostgreSQL-enabled build. |
| `postgres_dsn` | empty | PostgreSQL connection string. Treat it as a secret when it contains credentials. |
| `postgres_schema` | `public` | Dedicated schema for this LASO deployment. |
| `execution_mode` | `single` | `single` for SQLite/local use or `multi_instance` for PostgreSQL coordination. |
| `coordination.mode` | `single_owner` | Use the documented experimental multi-instance mode only with PostgreSQL. |
| `postgres_pool_min_connections` | `1` | Minimum PostgreSQL pool size. |
| `postgres_pool_max_connections` | `4` | Maximum PostgreSQL pool size. |
| `postgres_pool_acquisition_timeout_ms` | `1000` | Bounded pool acquisition wait. |

SQLite is deliberately single-instance. Do not run multiple LASO processes
against the same SQLite state. PostgreSQL multi-instance mode uses database
leases and fencing; it permits at-least-once attempts, not exactly-once work.

## Concurrency, deadlines, and budgets

`workers`, `max_runs`, `max_nodes`, `max_nodes_per_run`, `max_worker_jobs`, and
`max_worker_jobs_per_worker` bound local concurrency. `claim_batch_size` and
`max_pending_runs` bound scheduler/recovery pressure. Node and pipeline
`timeout_ms` values are per-attempt or per-pipeline deadlines in the pipeline
document.

For PostgreSQL coordination, `coordination.lease_ttl_ms` and
`coordination.heartbeat_interval_ms` control bounded ownership. The database's
clock is authoritative for lease expiry.

The optional `max_worker_wall_time_ms`, `max_worker_tokens_per_run`, and
`max_worker_cost_units_per_run` settings limit aggregate worker usage. A zero
value disables the corresponding budget. Missing provider usage metrics are
not interpreted as zero.

## Providers and workers

`models` maps logical model names to provider/model pairs. The built-in mock
provider is offline. Process workers are explicitly configured under
`process_workers`:

```yaml
process_workers:
  reference:
    executable: /path/to/laso-example-worker-host
    args: [--mode, success]
    environment_allowlist: [PATH]
    startup_timeout_ms: 5000
    request_timeout_ms: 5000
    interaction_timeout_ms: 300000
```

The process worker starts with an empty inherited environment unless variables
are named in `environment_allowlist` or supplied as literal `environment`
overrides. Provider credentials remain in the provider environment and are not
stored in PostgreSQL state.

Optional Codex, OpenCode, and Claude adapters use the same supervised worker
boundary. Their executable paths, allowed workspace roots, and environment
allowlists should be deployment-local configuration, never committed examples.

## Security-sensitive settings

`postgres_dsn`, provider credentials, `plugin_dirs`, `schema_roots`, process
worker executable paths, and environment overrides may disclose sensitive
information. Keep them outside version control, use restrictive permissions,
and redact them from logs. `allow_network` and `allow_remote_api` default to
false. A remote API bind requires deployment-owned authentication; LASO's
development identity is intentionally local and unauthenticated.

## Cancellation and approvals

Cancellation is durable and may be observed after a restart. A provider
termination acknowledgement is reported only when the provider process or
transport confirms termination. A worker that disappears before confirmation
does not create a false acknowledgement. Late provider results cannot replace
an authoritative cancelled or otherwise terminal state.

Approval and worker-interaction state is durable. Use the approval CLI/API to
make decisions; do not edit the database manually.
