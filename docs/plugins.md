# Plugin extension model

Core has separate provider, tool, storage, event, scheduler, identity, artifact and
node interfaces. Native application code can inject these interfaces directly.
Dynamic plugins use the versioned C SDK rather than the compiler-dependent C++ ABI.

The working plugin kinds are deterministic tools and model providers. Both use
the same bounded synchronous C ABI; the model-provider example returns a fixed
offline response. Other component kinds remain reserved explicitly and are
rejected as unsupported. This keeps a small, reviewable boundary rather than
exposing incomplete adapter implementations.

The `examples/plugin-model/config.yaml` file maps the example logical model to
the provider plugin. Run it from the repository root after building the example
plugins; it makes no network calls and requires no credentials.

See [plugin ABI](plugin-abi.md) and [SDK README](../plugin_sdk/README.md).
