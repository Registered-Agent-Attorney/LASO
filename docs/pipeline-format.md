# Pipeline format 1

Required root fields: `laso: "1"`, `name`, `version`, `nodes` mapping, and `edges`
sequence. Optional root limits are `max_steps` (1..100000, default 1000) and
`timeout_ms` (1..86400000, default 300000). Unknown fields are rejected.

Names use letters, digits, `.`, `_`, or `-`, begin with a letter/digit, and are at
most 128 characters. `input` and `output` exist implicitly and may only be declared
with their corresponding types. Input cannot receive edges; output cannot emit them.

| Node field | Meaning |
|---|---|
| `type` | Required built-in type or application-registered custom type |
| `function`, `tool`, `model`, `pipeline` | At most one logical registry binding; required by the corresponding node |
| `prompt` | Inline text passed unchanged to a model adapter |
| `field`, `value` | Top-level field and expected JSON-compatible value for router/validator |
| `reason` | Human-readable explanation at an approval boundary |
| `join` | Required matching join node for a parallel node |
| `max_iterations` | Required positive loop count for loop nodes, up to 10000 |
| `max_attempts` | 1..10 total attempts, default 1 |
| `retry_delay_ms` | 0..60000, default 0 |
| `timeout_ms` | 1..3600000 per attempt, default 30000 |

Edges require `from` and `to`. Optional `condition` is one of `accepted`,
`rejected`, `repeat`, `done`. Router/validator nodes require exactly accepted and
rejected branches; loop nodes require repeat and done branches. Other nodes have
one unconditional edge, except parallel nodes with 2..16 unconditional branches.

Every cycle must contain at least one edge with a positive `max_iterations`.
Validation removes bounded edges and checks that the remaining graph is acyclic.
Edge budgets count traversals over the entire run. Attempt limits, loop visits,
step limits and queue size limits provide separate finite bounds. A loop node
chooses `repeat` for its first N visits and `done` on visit N+1; give a cycle edge
an adequate traversal budget. Exhausting a budget fails the run.

All nodes must be reachable from input. Pair parallel/join structures correctly:
every branch must reach its declared join once before output. Runtime checks reject
missing, duplicate, mismatched or unresolved join arrivals. Static verification
does not yet prove every conditional branch terminates at its matching join.

Documents are limited to 1 MiB, 256 nodes, 1024 edges, depth 32 and 20000 YAML
values. Duplicate map keys (including duplicate node IDs), custom tags, malformed
types and multi-document YAML are rejected. YAML aliases are bounded during tree
inspection; cyclic aliases fail. Payloads use JSON types. Quote YAML strings when
they look like booleans or numbers.

This format intentionally does not evaluate expressions, execute code strings,
read prompt files, expand environment variables, or embed credentials. Extend the
schema explicitly in a later version instead of relying on ignored fields.
