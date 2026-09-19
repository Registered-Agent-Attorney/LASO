# Distributed execution (Milestone 2)

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

The run controller remains the authority for DAG advancement, joins, terminal
states, approvals, and cancellation. When a parallel node reaches independent
branches, it durably records one `NodeWork` record per branch and pauses the run
controller. Any healthy instance may claim an eligible record with a
`node:<work-id>` lease, a fencing token, and bounded global/per-run node-slot
leases. The existing branch execution path performs input/output schema checks,
policies, retries, provenance, and attempt persistence; the result is committed
with the node fence before the controller resumes the join.

Only deterministic local branch paths are distributed today: every node on the
branch path must be a `function`, `validator`, or `router` until its join. A
branch containing a tool, model, worker, subpipeline, approval, artifact-local,
or other side-effecting/session-local node stays in the owning runtime. This
boundary avoids pretending that a different process can resume a local process
worker or repeat an external side effect safely. Per-process runtime limits apply
to local work, while PostgreSQL node slots enforce the configured global and
per-run limits across instances.

Each claim records a durable work attempt ID. Lease takeover increments the
claim-attempt counter and assigns a new attempt ID; ordinary node retries retain
their normal distinct `NodeExecution` records. A takeover is therefore not
silently represented as a new logical retry policy decision.

## Recovery and guarantees

If a process exits, its lease is not released. After the configured TTL, another
service can take over with a higher fencing token and replay the durable run
checkpoint. This protects persistence, but an external tool/provider/worker may
have performed a side effect before the crash. LASO therefore does not claim
distributed exactly-once execution or exactly-once external effects.

Control-plane cancellation is owner-independent and updates the durable run
checkpoint. The current owner observes the request at its next checkpoint or
cooperative cancellation check. Distributed node workers also observe the
durable parent cancellation flag and stop before committing a result. Approval
decisions are owner-independent control-plane records; a waiting run is
re-queued after approval and can be claimed by any healthy instance.

If storage or coordination connectivity fails while a process owns work, the
process cannot renew or prove its lease. It stops the affected execution and
does not make further authoritative node mutations. After lease expiry, another
instance may take over; stale completion or checkpoint writes are rejected by
the fencing predicate.

During shutdown a service enters `DRAINING`, stops claiming new work, and asks
active runs to stop. Existing leases may remain until release or expiry. Instance
inspection is available at `GET /api/v1/instances` and `laso instance list`.

## Safety boundaries

- PostgreSQL is required for multi-instance mode and must be reachable by each
  process.
- Lease and fencing state is durable, but this is not a cluster membership,
  leader-election, or distributed worker system. It is a PostgreSQL coordination
  plane for opt-in LASO instances.
- `NodeWork` is a durable storage record covered by both storage adapters;
  PostgreSQL migration 8 adds its table. SQLite remains single-instance and
  executes the existing local branch path.
- Scheduler occurrence and event-delivery claims remain durable deduplication
  records; no arbitrary external source receives an exactly-once guarantee.
- Native plugins and external workers remain privileged integrations and must
  still obey the existing policy, timeout, cancellation, and cleanup contracts.
- Local filesystem artifacts and local workspaces are not replicated by this
  feature. Distributed branch paths carry structured messages through durable
  PostgreSQL records; adapters requiring host-local state remain owner-local.
- Do not use a mutable `latest` pipeline identity for historical work; pipeline
  revisions remain immutable `name@version` records.
