# Node contracts

All nodes implement `Node::execute(ExecutionContext&, const Message&) -> Task<NodeResult>`
and `type()`. A `NodeResult` contains an envelope and optional branch condition.
Node exceptions are caught at runtime boundaries and translated into recorded
failures. Arbitrary exception text is not persisted.

| Type | Baseline behavior |
|---|---|
| input / output | Accept and return structured envelopes |
| function | Await a registered deterministic C++ function |
| agent | Resolve logical model, check policy, await provider; mock ships offline |
| tool | Resolve registry tool, check policy, await invocation |
| router | Compare a top-level field to configured JSON value |
| validator | Same structured check, plus provenance validation result |
| approval | Durable request and pause; no stdin access |
| parallel | Queue 2..16 branch tokens with a unique group and matching join |
| join | Collect all group inputs in declared branch order into a JSON array |
| loop | Emit repeat/done according to its finite visit budget |
| subpipeline | Start a registered child run, await result, propagate approval waits |

Structural node semantics are implemented by the same runtime that persists all
node execution. Branch tokens contain their own message and group stack. The
baseline schedules ready tokens sequentially within a run, keeping scheduling and
combination deterministic. Different runs use the bounded concurrent executor.

Register C++ custom node factories in `Service::node_registry()` before registering
pipelines that use them. Register deterministic functions similarly with
`Service::functions()`. These are privileged application extensions, not a code
evaluation feature available over HTTP. The native plugin SDK currently bridges
tools; native custom-node adapters are reserved for later releases.
