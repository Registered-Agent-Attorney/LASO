# Execution, durability and recovery

Run states: Queued, Starting, Running, WaitingTool, WaitingModel, WaitingApproval,
Retrying, Paused, Completed, Failed, Cancelled, TimedOut. Terminal runs cannot
transition again. Pending approvals can be decided once. Rejection fails the run.
Node attempts have separate states, start/end timestamps, attempt numbers, safe
error categories, and duration. Messages and events are durable.

The SQLite adapter maintains separate tables for pipelines, runs, attempts,
messages, approvals, artifacts and events. Each row has an ID, indexed run ID,
insertion sequence and JSON representation of the typed record. WAL,
`synchronous=FULL`, a busy timeout, prepared parameter bindings and explicit
transactions are enabled. Schema version is stored using `PRAGMA user_version`.
Newer database schemas are rejected instead of silently interpreted.

Each successful node checkpoint includes its final attempt, output message, run
cursor/branch queues and event in one transaction. A pending approval includes its
request, waiting attempt and run state in one transaction. An approval decision and
resumable queued state commit together. SQLite history survives process restart.

The database has a Linux process lease (`flock`) so two independent services cannot
execute or approve the same run concurrently. This is local single-writer service
ownership, not a distributed claim protocol. API readers share the same adapter.

On startup, runs left in active states are marked Paused with a recovery-required
event. Interrupted attempts are marked failed. Completed and approval-waiting runs
are retained unchanged. `run resume ID` / the resume API can explicitly resume a
Paused or Queued checkpoint. An in-flight operation may already have produced an
external effect before crashing; inspect it before resuming. There is no general
exactly-once guarantee or automatic replay of interrupted side effects.

Retries apply to node execution failures up to the declared total attempt count.
Timeout and cancellation are not retried. Retry delays are asynchronous. A timeout
is cooperative and checked before/after node calls and during framework delays.
Each active execution segment has a pipeline deadline; time spent waiting for an
approval or across a restart is excluded. A resumed segment gets a fresh deadline.
The per-attempt deadline is capped by the current pipeline segment deadline and
provider/tool timeout metadata, including for concurrent branches. Persisted visit,
edge and step budgets do not reset; branch work consumes the same run step budget.

Cancellation propagates via `std::stop_source`/`std::stop_token`, including child
runs. A tool ignoring cancellation may finish its side effect before control
returns. The daemon cannot safely kill arbitrary native code. SIGTERM/SIGINT stop
HTTP acceptance, stop scheduler timers, request cancellation and drain workers.
Pending approval records remain durable. systemd may eventually terminate a
noncooperative process at its configured stop timeout.

Subpipeline depth is capped at eight. Children have separate run records and a
durable parent link. Child approval waits pause the parent. Completing a child
resumes a waiting parent, including after a restart. Parent/child creation crosses
separate transactions; interrupted creation can leave an orphan child requiring
operator inspection. Full atomic child dispatch and outbox recovery are deferred.

Local artifacts use generated filenames, exclusive creation and fsync before
metadata registration. User names never select filesystem paths. A crash between
file creation and metadata commit may leave an unreferenced file; garbage collection
is deferred. Payloads/results and comments are stored as supplied: applications
must keep credentials out of them. Resolved secret-provider values are not recorded.
