# LASO

**LASO is an orchestration framework, not a prebuilt AI assistant.**

Local AI System for Orchestration is an early, Linux-first C++20 framework for
declarative workflows, deterministic functions, model and tool registries, policy
checks, durable human approval, and execution history. Applications supply their
own logic and integrations. Agents are one node type; pipelines are the root
abstraction. Version **0.1.0** is a foundation, not a production-readiness claim.

**Validation status:** implemented, statically reviewed, and validated on Ubuntu
with GCC and Clang, ASan/UBSan, a Debian 13 container build, a runtime image
health check, and GitHub Actions. These are development and CI results, not a
production-readiness claim. See [VALIDATION.md](VALIDATION.md) for exact scope,
pending systemd deployment validation, and the blocked host TSan run.

```text
                 API / CLI
                     |
              Application services
                     |
               Pipeline runtime
                     |
          +----------+----------+
          |          |          |
        Nodes   Storage backends  Events
                   /      \\
               SQLite   PostgreSQL*
          |
       Registries
          |
     Versioned C plugin ABI
          |
 Native tool and model-provider adapters
```

## Linux requirements and build

Target environments: Ubuntu 24.04 LTS / Debian 13, x86-64, GCC or Clang,
C++20, CMake 3.22+, Ninja. No Python, Node.js, Java, model download, external AI
account, or GUI is required to build or run LASO. Dependencies come from the
distribution; CMake fetches the pinned small MIT-licensed JSON Schema validator
when it is not already available locally.

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake ninja-build \
  libsqlite3-dev libyaml-cpp-dev nlohmann-json3-dev libspdlog-dev \
  libcli11-dev libboost-system-dev libgtest-dev curl jq
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

PostgreSQL is an optional build and runtime backend. Install `libpqxx-dev` and
`libpq-dev`, configure with `-DLASO_ENABLE_POSTGRES=ON`, then select it with
`storage_backend: postgres` and a `postgres_dsn` connection string (or the
`LASO_STORAGE_BACKEND`, `LASO_POSTGRES_DSN`, and `LASO_POSTGRES_SCHEMA`
environment variables). SQLite remains the default. DSNs are never included
in LASO error messages or logs.

The default build has no PostgreSQL development-library requirement. Produced
binaries are `build/bin/laso`, `build/bin/laso-server`, and
`build/laso_tests`. Example C plugins are
`build/plugins/liblaso_example_tool.so` and
`build/plugins/liblaso_example_model_provider.so`. Core, runtime, storage factory,
SQLite, optional PostgreSQL, plugin loader, application, API, and CLI are separate
library targets. Installation currently
installs the executables, public headers, C SDK header, and example configuration;
a relocatable CMake SDK package is deferred.

## First pipeline

```yaml
laso: "1"
name: hello
version: 1
nodes:
  greet:
    type: function
    function: hello
edges:
  - {from: input, to: greet}
  - {from: greet, to: output}
```

`input` and `output` are implicit boundary nodes. Functions and tools resolve
through registries; no shell command interpretation occurs. See
[pipeline format](docs/pipeline-format.md) for the intentionally small schema.

```sh
./build/bin/laso pipeline validate examples/hello-pipeline/pipeline.yaml
./build/bin/laso run start examples/hello-pipeline/pipeline.yaml --input '{"value":42}'
./build/bin/laso run start examples/agent-review/pipeline.yaml
./build/bin/laso run start examples/human-approval/pipeline.yaml
./build/bin/laso pipeline register examples/composition/normalize.yaml
./build/bin/laso pipeline register examples/composition/process.yaml
./build/bin/laso run start process@1 --input '{"value":42}'
./build/bin/laso approval list
./build/bin/laso approval approve APPROVAL_ID --actor operator --comment Reviewed
```

The approval example exits with `WaitingApproval`. A later CLI process opens the
same SQLite database, records the decision, and continues. `LASO_DATA_DIR` defaults
to `.laso` relative to the working directory. The CLI is a local service adapter,
not an HTTP command wrapper. Only one service process may own a database: while
the daemon is running, use its API. Stop it before using local CLI database commands.
`laso health` checks local storage initialization; use HTTP health to probe the daemon.

## API

```sh
./build/bin/laso-server --config config/laso.example.yaml
curl -fsS http://127.0.0.1:8080/api/v1/health
curl -fsS http://127.0.0.1:8080/api/v1/version
jq -n --rawfile yaml examples/hello-pipeline/pipeline.yaml '{yaml:$yaml}' |
  curl -fsS http://127.0.0.1:8080/api/v1/pipelines \
  -H 'Content-Type: application/json' --data-binary @-
curl -fsS -X POST http://127.0.0.1:8080/api/v1/pipelines/hello@1/runs \
  -H 'Content-Type: application/json' -d '{"input":{"value":42}}'
```

Development identity is unauthenticated and bound to `127.0.0.1:8080`. Remote
binding requires deliberate `allow_remote_api` configuration and deployment-owned
authentication. The API never accepts filesystem paths for pipeline registration.
[API and CLI reference](docs/access.md) lists all endpoints and commands.

## Native plugins

```sh
LASO_PLUGIN_DIR=build/plugins ./build/bin/laso plugin list
LASO_PLUGIN_DIR=build/plugins ./build/bin/laso run start examples/native-plugin/pipeline.yaml
LASO_PLUGIN_DIR=build/plugins ./build/bin/laso --config examples/plugin-model/config.yaml \
  run start examples/plugin-model/pipeline.yaml
```

Plugins are loaded with `dlopen`/`dlsym`, only from configured directories.
The SDK uses a versioned C ABI, explicit structure sizes, borrowed inputs,
host-owned output callbacks, and no STL objects or exceptions across the boundary.
Tool and model-provider components are operational. Model providers are resolved
through the existing provider registry, so built-in mock and local providers remain
available. Other component kinds have reserved IDs and return `LASO_UNSUPPORTED`.

**Loading a native LASO plugin grants that plugin code execution inside the LASO
process.** Metadata validation does not isolate native code. See the
[SDK](plugin_sdk/README.md) and [ABI contract](docs/plugin-abi.md).

## Examples

| Directory | Behavior |
|---|---|
| `hello-pipeline` | Input → deterministic function → output |
| `agent-review` | Two offline mock model calls and a structured validator |
| `human-approval` | Durable pause and CLI/API decision |
| `native-plugin` | Harmless JSON echo through a native C plugin |
| `plugin-model` | Offline AgentNode response from a native model-provider plugin |
| `parallel-join` | Fork, checkpoint each branch, combine results in branch order |
| `schema-contract` | Offline function pipeline with input/output JSON Schema contracts |
| `bounded-loop` | Exactly two deterministic repetitions |
| `subpipeline` | Invoke registered `hello@1` through `parent@1`; register it first with `pipeline register` |
| `composition` | Offline versioned child pipeline and A → B → C composition |
| `local-openai` | Optional loopback-only OpenAI-compatible local model call |

For an optional loopback-only OpenAI-compatible local model service, see
[local model services](docs/local-models.md). It is disabled by default and does
not download or launch models.

## Development and deployment

See [architecture](docs/architecture.md), [runtime semantics](docs/runtime.md),
[pipeline composition](docs/pipelines.md),
[storage backends](docs/storage.md),
[Linux deployment](docs/linux-deployment.md), [security](SECURITY.md), and
[contribution instructions](CONTRIBUTING.md). CI specifies Ubuntu GCC/Clang,
Debian 13, ASan/UBSan, formatting, clang-tidy, and a real PostgreSQL service job.
Tests use GoogleTest and CTest, plus a shell process/restart smoke test; no external
AI services are used.

```sh
docker compose -f deploy/docker/compose.yaml up --build
```

The Compose example deliberately uses Linux host networking with the API on host
loopback. Persistent data lives in a named volume. A sample systemd unit runs the
daemon in the foreground as an unprivileged service account.

## Current limitations and deferred work

- Linux builds, tests, sanitizer builds, and the Debian container path have been
  executed. Full systemd installation and shutdown behavior remain unvalidated.
- One process owns each selected storage database: SQLite uses a lock file and
  PostgreSQL uses a session-held advisory lock. There is no distributed scheduling
  or horizontal scaling. Storage calls are short synchronous transactions; the
  PostgreSQL adapter uses one bounded connection protected by a mutex.
- Fork branches execute concurrently through the bounded executor when capacity is
  available, while join results retain pipeline branch order. Global, per-run,
  model-call, and tool-call limits bound work; cancellation is cooperative. Approval
  pauses the entire run.
- Registered pipeline revisions are immutable `name@version` records. Subpipeline
  nodes execute normal durable child runs with persisted parent/child links,
  version resolution, approval/retry/recovery behavior, and a configurable maximum
  depth. There is no distributed execution or package registry.
- Deadlines and cancellation are cooperative. A native plugin that blocks or
  misbehaves can block a worker or crash the process. The v1 plugin invocation ABI
  is for short local operations; asynchronous external plugin I/O is deferred.
- Approval waits and history survive restart. In-flight external effects are not
  exactly once; an explicit resume may replay an unfinished node. Operators must
  review interrupted runs. Automatic general crash recovery is deferred.
- Validator nodes retain the legacy field/value routing mode when no `schema` is
  declared; declared schemas use the shared local JSON Schema engine. Prompt values
  are inline text, not automatically read from files.
- Mock is the default built-in provider. An optional loopback-only OpenAI-compatible
  adapter can call an already-running local model service. No model serving, remote
  adapters, streaming, secret persistence, sandboxing, authentication platform, or
  GUI is included.
- Scheduler registration is an in-process C++ interface, finite one-shot/interval
  only. Durable schedules, cron, event backends, distributed storage, and the
  non-tool plugin adapters are deferred.

Licensed under Apache License 2.0.
