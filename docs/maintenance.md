# Coordinated drain and maintenance

LASO drain mode lets an operator stop one instance from accepting or claiming new work before a restart or maintenance window. It does not stop the process, cancel work, or transfer ownership by force. Existing lease expiry and fencing remain authoritative.

## State model

| State | New submissions | New ownership claims | Existing owned work | Ready for new work |
|---|---|---|---|---|
| `active` | Accepted subject to normal admission limits | Allowed | Runs normally | Yes, if storage/schema checks pass |
| `draining` | Rejected with HTTP 503, `INSTANCE_NOT_ACCEPTING_WORK`, and `Retry-After: 1` | Blocked | May finish; valid leases continue to renew | No |
| `drained` | Rejected | Blocked | No active owned runs or nodes remain | No |
| `maintenance` | Rejected | Blocked | Entry is allowed only from drained state | No |

`drained` is derived from the draining state plus zero active owned runs and nodes. It is not a separate durable transition. The PostgreSQL instance registry continues to show `DRAINING` until the operator enters maintenance; either state prevents new claims. `safe_to_stop` becomes true only when no locally owned runs or nodes remain and, in multi-instance mode, the registry confirms this instance is visible in a non-accepting state. A registry update failure leaves local admission closed but reports the cluster state as `ACTIVE` or unavailable and `safe_to_stop: false`.

## Persistence and cluster scope

The desired state, start time, actor, and a bounded history of the latest 128 transition results are atomically written to `operator-maintenance.json` in the configured LASO data directory. The file is local to the instance and must be retained across restarts. The record contains only actor/action/state/time/result metadata; it does not contain prompts, request bodies, credentials, or headers. This local transition journal supports restart reconciliation. Operator/admin API requests also create cluster-visible records in the PostgreSQL operator audit feed; see docs/operator-audit.md. The two records serve different purposes and have separate failure behavior.

In multi-instance mode, the PostgreSQL `laso_instances` row publishes `ACTIVE`, `DRAINING`, or `MAINTENANCE`. PostgreSQL is the coordination authority for claims. Drain updates take effect under the same row-lock coordination used by ownership acquisition. A crashed instance is not an active cluster member indefinitely: the configured stale interval marks its registry view `STALE`, and work ownership can move only through normal lease expiry and fencing. Old registry rows are retained for bounded operator visibility; they do not grant work ownership.

The local drain intent is persisted before the registry update. If PostgreSQL cannot confirm that update, LASO rejects new local work and reports that stopping is unsafe. Repeating the drain request retries registry reconciliation. A restart restores the local desired state before distributed scheduling starts. If no state file exists, LASO treats this as a first start and defaults to `active`. Operators must retain the data directory: deleting a previously written state file loses the local drain intent and a later restart will default to active. Corrupt, symlinked, or oversized state files fail startup rather than being silently accepted.

## API and authorization

The operator routes use the same deployment-owned `IdentityProvider` as other operator APIs:

- `GET /api/v1/operator/maintenance`
- `POST /api/v1/operator/maintenance/drain`
- `POST /api/v1/operator/maintenance/enter`
- `POST /api/v1/operator/maintenance/resume`

State-changing routes require an operator or admin actor and a JSON body containing `{"confirmed":true}`. Missing confirmation returns `400 CONFIRMATION_REQUIRED`; invalid transitions return `409 CONFLICT`; a failed coordination update returns a bounded storage error. Ordinary users are denied server-side. Actor attribution is taken from the authenticated identity provider, never from the body. The unauthenticated local-development identity is loopback-only. Remote deployments use a deployment-owned user-authentication gateway and the Core trusted-gateway credential described in [the API access guide](access.md); Core does not validate the proxy peer address.

Drain is idempotent and may be repeated to retry cluster synchronization. Resume returns an instance to active and permits claims again. Entering maintenance is allowed only after drain has no active owned work. Maintenance is still reversible through the authorized resume operation. The API does not execute shell commands or control systemd.

There is no dedicated operator CLI in this release. Use an authenticated deployment operator console or API client. The Web application only changes LASO state; it does not stop or restart services.

## Health and metrics

`GET /api/v1/health/live` remains successful while draining or in maintenance. `GET /api/v1/health/ready` reports `503 not_ready` with maintenance state while the instance is not accepting work. This readiness signal complements admission enforcement; it does not replace it. The metrics endpoint exports bounded one-hot maintenance state and owned run/node gauges without instance, run, session, or principal labels.

## Restart procedure

For a single-instance systemd deployment:

1. Request drain through the authorized Web admin surface or operator API.
2. Poll the status route at a reasonable interval until it reports `state: drained` and `safe_to_stop: true`. Use a bounded client-side timeout and handle interruption; do not add an unbounded systemd stop hook.
3. Stop or restart the service using the deployment's normal service manager.
4. Verify liveness, readiness, and the maintenance status after startup.
5. Resume the instance after checking the deployment is ready to accept work.

For a rolling multi-instance deployment, drain one member at a time and verify the PostgreSQL-visible state before stopping it. Other active members may continue taking work. LASO does not claim zero-downtime deployment; the result depends on remaining capacity, workload duration, database availability, and external worker behavior.

Forced termination remains safe with respect to authoritative state because expired leases and fencing reject stale completion. It may interrupt side effects and cause at-least-once retry behavior. Drain does not make provider side effects exactly once and does not migrate an in-flight operation.

## Failure behavior

If PostgreSQL is unavailable or a registry update fails, the instance closes local admission, reports the failure without SQL or connection details, and does not claim it is safe to stop. Existing valid leases renew normally while PostgreSQL is reachable. During an outage, renewals fail and leases expire normally. A takeover increments the fence, so a returning old process cannot commit stale completion. If the operator client disconnects, the local state remains durable. The PostgreSQL registry reflects the request only if its update committed; query status after reconnecting to confirm both local state and cluster visibility.
