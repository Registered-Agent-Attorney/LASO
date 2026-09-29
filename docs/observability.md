# Readiness and metrics

## Health routes

`GET /api/v1/health` remains the compatibility liveness probe. It reports that
the API process can answer a request. `GET /api/v1/health/live` is the explicit
alias.

`GET /api/v1/health/ready` is a readiness signal for a supervisor or proxy.
It does not itself block normal API requests or change existing admission
limits. It reports Core storage, schema, and runtime readiness. Queue and worker
capacity are reported separately.

The route checks PostgreSQL connectivity, the current schema version, and
whether the runtime is draining. It returns `200` with `status: ready` only
when all three checks pass. Otherwise it returns `503` with `status: not_ready`
and only these bounded state values:

| Check | Values |
| --- | --- |
| `storage` | `available`, `unavailable` |
| `schema` | `current`, `incompatible`, `unknown` |
| `runtime` | `active`, `draining` |

Readiness caps pool acquisition and connection establishment at one second.
It applies a one-second PostgreSQL statement timeout to connection setup and
the schema probe, and does not scan workflow history. The response contains no
DSN, schema name, SQL, or exception details.

Readiness does not guarantee spare queue or worker capacity, nor that every
provider, plugin, artifact backend, or worker is healthy. Those summaries remain
available through the existing sanitized operator API.

## Prometheus endpoint

`GET /api/v1/metrics` returns Prometheus text exposition format 0.0.4 on the
main API listener with `Cache-Control: no-store`. It uses the same
`IdentityProvider` authentication and authorization check as other API routes.
The local development identity is unauthenticated, so keep that listener
loopback-bound. Remote scrapers require the deployment's API authentication
and network boundary.

The current bounded metric set is:

| Metric | Type | Labels / meaning |
| --- | --- | --- |
| `laso_api_responses_total` | Counter | Fixed method (`GET`, `POST`, `PUT`, `DELETE`, `PATCH`, `OTHER`) and status class (`2xx`, `4xx`, `5xx`, `other`). |
| `laso_api_request_duration_seconds` | Histogram | Fixed latency buckets; no labels. |
| `laso_session_sse_active_streams` | Gauge | Current accepted session streams. |
| `laso_session_sse_stream_limit` | Gauge | Configured per-process stream limit. |
| `laso_session_sse_streams_accepted_total` | Counter | Accepted streams. |
| `laso_session_sse_streams_rejected_total` | Counter | Rejected at stream admission. |
| `laso_session_sse_streams_closed_total` | Counter | Closed streams. |

No metric labels contain request paths, IDs, principals, user input, prompts,
message contents, credentials, provider continuation values, filenames, URLs,
or free-form errors. Counters and histograms are process-local and reset on
restart. API request metrics cover responses handled by Core, not requests
rejected before the API handler. The endpoint does not export database pool
utilization, worker/provider timings, tracing spans, or per-node queue labels.
Use the sanitized operator status view for existing PostgreSQL pool, queue, and
capacity summaries.
