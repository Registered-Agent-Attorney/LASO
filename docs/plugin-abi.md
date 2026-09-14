# Native ABI v1

`plugin_sdk/include/laso_plugin.h` is a standalone C header. The exported symbols
are `laso_plugin_query`, `laso_plugin_init`, and `laso_plugin_shutdown`. Compile them
as C or export them with `extern "C"`. The host uses `RTLD_NOW | RTLD_LOCAL`.

The descriptor begins with structure size and ABI version. Unknown ABI versions,
undersized descriptors, missing exports, oversized metadata, invalid registration,
duplicate components and failed init are rejected and reported. Known prefixes
allow future structs to append fields while retaining version-specific semantics.
Do not reorder or reinterpret fields under an unchanged ABI version.

Every buffer and string has defined ownership:

- Query descriptor and strings are plugin-owned and valid until `dlclose`.
- Host API is borrowed only during init. Registrations are accepted only then.
- The host copies registration names, JSON metadata, opaque instance pointers and
  callback addresses. Instances remain plugin-owned until shutdown.
- Input JSON and call context are borrowed until invoke returns. A callback must
  not retain them or asynchronously invoke a host callback after returning.
- Output is copied once through `write_json` into host-owned memory, capped at
  1 MiB. No allocator or release function crosses the ABI.
- Successful init produces a non-null handle. Failed init cleans up its allocations
  and leaves the output handle null. Successful handles are shut down once.
- No C++ exception may cross an exported function or callback. SDK examples are C;
  C++ plugins must catch their own exceptions and return status codes.

The callback context exposes cooperative cancellation/deadline checking. ABI v1
tool callbacks are synchronous, short and local. Calls to a component are guarded
against concurrent entry; busy components fail explicitly and may use bounded
pipeline retries. Long-running external I/O needs a later asynchronous ABI, not
background use of borrowed callbacks.

ABI v1 supports `TOOL` and `MODEL` components. The other component kinds remain
reserved and return `LASO_UNSUPPORTED`. Existing v1 tool components remain valid:
the appended optional `health` callback is discovered only when `struct_size`
includes it.

A model component's metadata is a bounded JSON object with `version`, `remote`,
`network`, `streaming`, `context_size`, `timeout_ms`, and `capabilities` fields.
Its `invoke` callback receives one JSON generation request containing `operation`,
`logical_model`, `model`, `prompt`, `input`, `options`, and `timeout_ms`. It must
write one JSON response containing `ok` and `output`; `model` and `provider` are
optional. A model `health` callback is optional and returns
`{ "healthy": boolean, "detail": string }`.

Plugins are discovered non-recursively in explicit configured directories.
Symlink entries and non-`.so` files are skipped. No default search of system library
paths occurs. Library constructors execute at `dlopen`, before ABI validation:
**an ABI check is not a sandbox or a security assessment**. Loading native code
grants execution inside the process. Bad pointers, memory corruption or fatal
signals cannot be made safe by metadata checks or exception handling.

The loader retains shared library ownership through every registered tool object,
so the library cannot be unloaded while its callback is in use. No hot unloading
or isolation is advertised. Tests include valid load/invoke, incompatible ABI,
invalid file, symlink exclusion and empty discovery paths.
