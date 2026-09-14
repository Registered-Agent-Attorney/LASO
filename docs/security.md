# Security design

See [SECURITY.md](../SECURITY.md) for the authoritative baseline boundaries.
The identity interface and `AuthorizationContext` surround all API routes. The
default local identity does not authenticate users. The rule policy is separate
from prompts and evaluates tool/provider access before invocation.

Rules identify a logical resource, `*` or a specific binding, and a decision of
`allow`, `deny` or `approval`. Deny takes precedence; an allow rule does not erase
a required approval. Network denial and non-public-to-remote denial are enforced
before rule evaluation. Metadata classifications are application-supplied rather
than an automatic data-loss-prevention system.

`EnvironmentSecretProvider` resolves uppercase environment-variable references.
It does not serialize or log values. Applications must avoid passing secrets as
normal durable messages. TLS, authentication implementations, encryption at rest,
OS sandboxing, retention policies and secret-manager adapters are future/deployment
work rather than claimed features of this skeleton.
