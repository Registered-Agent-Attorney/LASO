# Plugin extension model

Core has separate provider, tool, storage, event, scheduler, identity, artifact and
node interfaces. Native application code can inject these interfaces directly.
Dynamic plugins use the versioned C SDK rather than the compiler-dependent C++ ABI.

The first working plugin kind is a deterministic tool. Non-tool ABI component
kinds are reserved explicitly and rejected as unsupported. This keeps a small,
reviewable boundary rather than exposing incomplete adapter implementations.

See [plugin ABI](plugin-abi.md) and [SDK README](../plugin_sdk/README.md).
