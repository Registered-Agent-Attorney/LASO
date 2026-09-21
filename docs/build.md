# Build and install

LASO is Linux-first and currently requires a Linux build environment. The
project uses CMake 3.22 or newer, C++20, and a pinned JSON Schema validator
fetched by CMake when it is not already available. A clean checkout is enough;
no development worktree, generated file, or provider account is required.

## Core SQLite build

On Ubuntu or Debian, install the core and test dependencies:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential cmake ninja-build git \
  libsqlite3-dev libyaml-cpp-dev nlohmann-json3-dev \
  libspdlog-dev libcli11-dev libboost-system-dev libgtest-dev \
  curl jq
```

Configure and build from the repository root:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
```

For a smaller runtime-only build, configure with `-DBUILD_TESTING=OFF`.
The core build does not require PostgreSQL development packages or any agent
provider executable.

The default binaries are:

```text
build/bin/laso
build/bin/laso-server
build/bin/laso-example-worker-host
```

## Release build and sanitizers

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure

cmake -S . -B build-san -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLASO_ENABLE_ASAN=ON -DLASO_ENABLE_UBSAN=ON
cmake --build build-san --parallel 2
ctest --test-dir build-san --output-on-failure
```

ThreadSanitizer is a separate build and is environment-dependent:

```sh
cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLASO_ENABLE_TSAN=ON
```

Do not combine TSan with ASan or UBSan.

## PostgreSQL build

PostgreSQL support is optional at compile time. Install the client libraries
and `libpqxx`, then configure a separate build directory:

```sh
sudo apt-get install -y libpq-dev libpqxx-dev
cmake -S . -B build-postgres -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLASO_ENABLE_POSTGRES=ON
cmake --build build-postgres --parallel 2
```

The PostgreSQL test suite uses `LASO_TEST_POSTGRES_DSN`. Use a disposable,
authenticated database; never put the DSN in a repository file or command log.
For a complete deterministic two-instance example, see
[the distributed example](../examples/distributed/README.md).

## Optional provider adapters

Provider adapters are opt-in and do not download providers or credentials:

```sh
cmake -S . -B build-providers -G Ninja -DCMAKE_BUILD_TYPE=Debug \
  -DLASO_BUILD_CODEX_ADAPTER=ON \
  -DLASO_BUILD_OPENCODE_ADAPTER=ON \
  -DLASO_BUILD_CLAUDE_ADAPTER=ON
cmake --build build-providers --parallel 2
```

The corresponding provider executable must already be installed and safely
configured. Adapter tests that need a real provider are opt-in; deterministic
fixtures remain the default test path.

## Install layout

Installation is intentionally small and conventional:

```sh
cmake --install build --prefix "$HOME/.local"
```

This installs the CLI, server, deterministic reference worker, public headers,
the C plugin SDK header, the example configuration, and public documentation.
The state directory is still selected at runtime and is not created in the
installation prefix.

## Windows and containers

The top-level CMake project rejects non-Linux systems. Windows developers can
use WSL or a Linux container; the repository's Dockerfile and Compose example
are supported development paths, not a promise of native Windows binaries.
