# Distributed execution (Milestone 1)

LASO can optionally run more than one service process against the same
PostgreSQL schema. Enable it explicitly with:

```yaml
storage_backend: postgres
postgres_dsn: "..."
execution_mode: multi_instance
```

The default remains `execution_mode: single`. SQLite rejects multi-instance
configuration. PostgreSQL schema migrations use a short transaction advisory
lock, while the normal single-owner PostgreSQL mode retains its session-held
advisory lock.

## Ownership model

Manual launches, schedules, event triggers, and child pipelines all use the
normal `Runtime::run` path. In multi-instance mode that path durably creates a
`Queued` run. Each service has an opaque UUID instance identity persisted in
`laso_instances` and periodically heartbeats it. A bounded dispatcher scans a
small page of non-terminal checkpoints and attempts to acquire `run:<run_id>`.

The first successful claim records the owner instance, lease timestamps, and a
monotonically increasing fencing token in the run record. The owner then runs
the ordinary LASO runtime, including policies, schemas, retries, approvals,
plugins, tools, providers, subpipelines, and existing global limiters.

The lease is renewed from database time. Every owner checkpoint is committed in
one PostgreSQL transaction that checks the current owner, fencing token, and
unexpired lease. A stale process therefore cannot overwrite a newer owner's
checkpoint. Lease loss stops the local run and fails closed; it is not converted
into a false successful completion.

Only complete runs are claimed. Parallel graph branches continue to execute
within the owning runtime and are not independently leased. Per-process runtime
limits apply to the runs owned by that process, while PostgreSQL coordination
prevents two processes from owning the same run at the same time.

## Recovery and guarantees

If a process exits, its lease is not released. After the configured TTL, another
service can take over with a higher fencing token and replay the durable run
checkpoint. This protects persistence, but an external tool/provider/worker may
have performed a side effect before the crash. LASO therefore does not claim
distributed exactly-once execution or exactly-once external effects.

Control-plane cancellation is owner-independent and updates the durable run
checkpoint. The current owner observes the request at its next checkpoint or
cooperative cancellation check. Approval decisions remain ordinary durable
records; a waiting run is re-queued after approval and can be claimed by any
healthy instance.

During shutdown a service enters `DRAINING`, stops claiming new work, and asks
active runs to stop. Existing leases may remain until release or expiry. Instance
inspection is available at `GET /api/v1/instances` and `laso instance list`.

## Safety boundaries

- PostgreSQL is required for multi-instance mode and must be reachable by each
  process.
- Lease and fencing state is durable, but this is not a cluster membership,
  leader-election, or distributed worker system.
- Scheduler occurrence and event-delivery claims remain durable deduplication
  records; no arbitrary external source receives an exactly-once guarantee.
- Native plugins and external workers remain privileged integrations and must
  still obey the existing policy, timeout, cancellation, and cleanup contracts.
- Do not use a mutable `latest` pipeline identity for historical work; pipeline
  revisions remain immutable `name@version` records.
