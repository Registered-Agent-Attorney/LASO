# ADR 0006: Native workers use an isolated transport seam

## Context

The native C worker ABI is functional and must remain compatible, while future
deployments may need supervised child processes or remote transports.

## Decision

`WorkerManager` depends on the backend-neutral `WorkerTransport` lifecycle
interface. The existing `WorkerAdapter` remains the in-process native-ABI
implementation. The interface covers start/stop, submit, status/recovery,
result, and cancel; transport exceptions are distinct from job failures.

## Consequences

Current plugins and API semantics remain unchanged. Bounded timeout,
cancellation, process supervision, Unix sockets, and remote protocols can be
added behind the seam later, but no process isolation is implied today.

## Supersedes

None; this records the current architecture.
