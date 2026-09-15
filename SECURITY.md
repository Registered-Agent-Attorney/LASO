# Security boundaries

LASO v0.1 is an early framework. It has not been independently audited or validated
in production. Ubuntu GCC/Clang, ASan/UBSan, Debian container, runtime-image, and
hosted CI validation have been performed; full systemd deployment behavior remains
unvalidated. See [VALIDATION.md](VALIDATION.md) for the exact record.

- The only built-in model provider is offline Mock. No external AI service is
  contacted and no model is downloaded automatically.
- Default tools/functions are harmless deterministic operations; there is no
  shell, script interpreter or arbitrary code execution endpoint.
- Pipelines are parsed using yaml-cpp and validated before execution. Duplicate
  keys, unsupported versions, custom YAML tags, invalid references and unbounded
  cycles are rejected. Payload and graph sizes are bounded.
- Policy checks occur in the runtime before invoking tools/providers. Network
  access defaults off, and remote providers cannot receive non-public metadata
  classifications through the baseline policy. Policy is not an OS sandbox.
- The management API is unauthenticated development mode on loopback by default.
  Non-loopback binding requires an explicit configuration change. A deployment
  exposing it remotely must supply an identity/authorization implementation or a
  correctly configured authenticating proxy. Do not equate loopback with complete
  access isolation from other local processes.
- Loading a native LASO plugin grants that plugin code execution inside the LASO
  process. Even shared-library constructors run before ABI checks. Only explicitly
  trusted, configured plugin directories should be used, with write access limited
  to the operator. Plugins are not isolated, and metadata can be false.
- The environment secret provider resolves values on demand but is not wired into
  automatic logging or persistence. Core logs contain event identifiers, not node
  payloads, prompts or arbitrary exception messages. User input, node results and
  approval comments are deliberately durable; applications must not place resolved
  secrets in those values. No generic redaction guarantee is claimed.
- Native binaries set restrictive umask. Use a private state directory, restrict
  database backups and artifact access, and keep configuration outside public
  repositories. SQLite and artifacts are not an encrypted secret vault.
- Cancellation and deadlines are cooperative. A faulty native extension can block
  a worker, corrupt memory or crash the daemon. Out-of-process isolation is deferred.

For a vulnerability, use the repository host's private vulnerability reporting
feature when it is enabled, or contact the repository maintainer privately. Do not
post exploit details or sensitive records in public issues. No security contact
address or response-time commitment has been invented for this initial skeleton.

JSON Schema validation is local-only. Remote `$ref` retrieval is disabled, schema
paths are canonicalized beneath configured roots (including symlink checks), and
schema, reference, nesting, payload and cache limits bound validation work. External
reference cycles are rejected during declaration validation, and parsed schemas are
shared only through a mutex-protected bounded cache; validator instances remain
per-call.
