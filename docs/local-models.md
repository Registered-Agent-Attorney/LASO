# Local model services

LASO includes an optional `local-openai` model provider for an OpenAI-compatible
HTTP service running on the same machine. The adapter accepts only `http://127.0.0.1`
or `http://[::1]` endpoints. It has no credential support, cannot use TLS, and cannot
contact a remote host. This keeps the v0.1 adapter local and explicit.

Enable it in the deployment configuration:

```yaml
allow_network: true
local_openai_endpoint: "http://127.0.0.1:18081"
models:
  local:
    provider: local-openai
    model: local-model
```

`allow_network: true` is required because loopback HTTP is still a network boundary
in LASO policy. The provider is marked local (`remote: false`) and the policy engine
does not allow non-public data to remote providers.

The target service must already be running and expose `POST /v1/chat/completions`.
LASO never downloads a model, starts a model server, selects a GPU, or sends a request
outside the configured loopback endpoint. A compatible local runtime can use a CPU or
a GPU according to its own launch configuration.

An agent node receives the pipeline payload as JSON text in a user message. The node
prompt becomes the system message. If the completion content is valid JSON, LASO uses
it as the next payload; otherwise it returns `{ "text": "..." }`.
