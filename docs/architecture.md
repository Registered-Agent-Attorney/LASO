# Architecture and ownership

The public API is C++20. The shared-library extension boundary is C. The framework
version (0.1.0), YAML format (1), SQLite schema (1), and plugin ABI (1) are distinct.

| Target | Responsibility and dependencies |
|---|---|
| `laso_core` | Domain types, safe parser, config, policy contracts, registry, events, security and artifacts; no HTTP includes |
| `laso_storage_sqlite` | Native SQLite C API behind `Storage`, transactional checkpoints |
| `laso_plugin_loader` | Linux dynamic loader and C adapters for tools and model providers |
| `laso_runtime` | Async node execution, state transitions, finite scheduling and checkpoint decisions |
| `laso_application` | Owns dependencies, registration, recovery inspection and shared services |
| `laso_api` | Versioned JSON routes and Boost.Beast asynchronous HTTP |
| `laso_cli` | CLI11 parsing, calls the same services |

Boost.Beast was selected because Ubuntu and Debian ship Boost, it supplies a
maintained HTTP parser, and Asio also provides the execution/event-loop facilities.
No custom HTTP parser or provider SDK is in the runtime. SQLite uses its small C
API instead of an ORM. YAML and JSON remain at parsing and payload boundaries.

Registries use shared mutexes and shared ownership of registered objects. Plugin
tool and provider objects retain their library handle; shutdown and `dlclose` occur only after
the last registered reference disappears. A batch registration either replaces the
registry snapshot atomically or leaves it unchanged. Registration is a startup
operation. Plugins are never hot-reloaded during execution.

The executor owns a configured finite number of `std::jthread` workers and runs
Asio coroutines. Slot limiters suspend rather than block. One coroutine owns each
run's mutable snapshot. Storage serializes its short SQLite operations; runtime
admission and cancellation maps have their own mutex. HTTP connections and
scheduler timers use strands. These are independent locks, not a global framework
lock. Implementations registered by applications must handle concurrent calls.

The owner must stop HTTP acceptance and scheduling, request runtime cancellation,
and drain/join workers before destroying `Service`. The daemon follows this order.
Use service and runtime APIs from C++ without initializing HTTP:

```cpp
laso::Executor executor(2);
laso::Config config;
laso::Service service(executor.context(), config);
service.register_pipeline(yaml_text);
auto id = service.start("hello", laso::Json{{"value", 42}});
executor.start();
executor.join();
auto record = service.get(laso::RecordKind::Run, id);
```

An `ExecutionContext` gives extensions run/node identity, attempt/visit number,
cooperative stop token and a steady-clock deadline. `Task<T>` is an Asio awaitable.
The baseline performs no external network or model calls. Short SQLite calls and
v1 native callbacks are synchronous; future external I/O implementations must
suspend rather than hold a worker on a blocking operation.

## Schema contracts

The runtime uses the pinned `pboettch/json-schema-validator` Draft 7 library
through `SchemaValidator`. `input_schema` is checked immediately before a node is
invoked and `output_schema` immediately after it produces a payload. `validator`
nodes call the same service explicitly. Parsed documents are cached by canonical
path behind a mutex, while each validation owns its validator instance, so branch
validation is safe concurrently. Configured `schema_roots`, canonical path checks,
disabled remote references, and bounded document/depth/payload limits protect the
filesystem and runtime.
