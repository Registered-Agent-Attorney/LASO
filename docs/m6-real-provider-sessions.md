# M6: real provider execution in durable sessions

## Contract

LASO's session ID, ordered turn journal, run binding, and event journal remain
authoritative. A worker's native conversation identifier is opaque adapter
state. It is never the LASO session ID and is not returned from session or
worker-job API views.

An optional worker may advertise `session-continuation` and
`session-context`. A durable session turn is rejected before provider
submission unless the configured worker advertises continuation support. When
a context generation is selected, the worker must also advertise context
support. The worker process protocol carries the immutable context snapshot and
opaque continuation over its private local pipe.
For approval and input requests, LASO also passes its canonical session ID;
the adapter uses LASO session and worker-job identities in interaction records
instead of exposing the provider-native thread identity.

Turns remain sequential under the existing per-session dispatch and fencing
rules. The worker returns a candidate continuation for the run. LASO makes it
current only when the run's completion is accepted by the authoritative fence.
Cancelled, stale, failed, or ambiguous work cannot advance the session
continuation. The stored run snapshot identifies the context generation and
the continuation used to start that attempt.

Provider attempts and external side effects are at least once at the existing
recovery boundary. LASO does not promise exactly-once provider execution. An
interrupted or unqueryable provider turn remains ambiguous; LASO does not
silently resubmit it. A later turn can resume only from the last continuation
committed by an authoritative successful run.

## Codex adapter

The optional Codex worker advertises the two generic capabilities. It keeps the
Codex thread identifier inside LASO's opaque continuation record. A worker
process restart or LASO restart resumes the saved thread. If the selected
immutable context generation changes, the adapter starts a fresh provider
thread and seeds it with that generation's derived payload and the durable
completed-turn tail after its boundary. Later turns continue the new thread.
The prior provider thread is left intact; LASO's complete turns and events
remain authoritative regardless of provider retention.

For durable-session turns, the adapter returns only the response text and safe
model/provider labels. It omits the native thread identifier, project path,
and raw command/action details from the session result. The returned opaque
continuation is staged in the private durable worker job so a LASO restart
between provider completion and the run checkpoint does not lose it. Worker
job APIs redact this value. The enclosing run publishes it through the
authoritative session fence; staging alone never advances session continuation.

## Failure behavior

Timeouts, child-process death, malformed messages, and interrupted streams fail
the current attempt or leave it `Unknown` according to the existing worker
recovery contract. They never create a successful turn. LASO only commits a
continuation returned by a completed provider response and accepted through
the run's current fence. A cancellation that races with provider completion is
settled by the existing run state machine; a late stale response cannot
overwrite durable continuation.

## Validation

The real Codex acceptance used a disposable fixture project and an already
authenticated local Codex installation. It did not accept credentials on the
command line or inspect provider account state. Three turns completed through
the durable session. Turn 1 read a harmless fixture fact; Turns 2 and 3
recalled it without rereading the file. LASO was restarted between turns, and
the third turn ran after an immutable context generation was created at turn 2.
The persisted run snapshot selected that generation, and the provider
continuation remained private in the API.

Browser acceptance used the private Go LASO-Web against the same LASO build.
It verified a real provider-backed turn, two independent browser clients
observing the same LASO-owned session, Web restart, next-turn continuation,
and SSE replay using `Last-Event-ID`.

A separate real-provider acceptance ran two LASO service processes against the
same disposable PostgreSQL schema. Instance A created the pipeline and session
and completed Turn 1. Instance B then listed and retrieved that session and
replayed Turn 1's completion event. After A was stopped between turns, Turn 2
was submitted through B; its newly supervised Codex process resumed the saved
continuation and recalled the fixture marker. This verifies continuation after
the prior service owner is gone. It does not claim that an in-flight Codex
side effect can be transferred safely to another LASO instance.

The complete GCC Debug and Release suites each discovered 290 tests; 285
passed, none failed, and five were intentionally skipped for optional S3,
installed-provider, and separate distributed acceptance lanes. The opt-in
real Codex session acceptance passed
independently. Focused fencing, cancellation, continuation redaction, and
provider-death recovery tests passed. Private LASO-Web Go
unit/race tests, browser tests, production build, and the real-provider browser
acceptance passed. The M6 pull request records the exact candidate's hosted
compiler, sanitizer, PostgreSQL, S3, formatting, and static-analysis CI lanes.
The real provider acceptance remains separate from credential-free core CI.
