# Linux build and deployment

Primary targets: Ubuntu 24.04 and Debian 13 on x86-64. The source intentionally
requires Linux; there are no alternate Windows loaders or native runtime branches.
ARM64 is not validated, but no x86-specific application data representation is used.

Install the dependencies listed in the README. For a fresh validation, follow
[first Linux validation](first-linux-validation.md), including both compilers,
formatting, clang-tidy, sanitizers and process-restart tests. No dependencies are
downloaded by CMake.

## Local native execution

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
./build/bin/laso run start examples/hello-pipeline/pipeline.yaml
./build/bin/laso-server --config config/laso.example.yaml
```

Local data defaults to `.laso`; root is not required. Run the CLI from the same
working directory or use `LASO_DATA_DIR`. `LASO_DB_PATH` explicitly selects a
database and takes precedence over the derived data-directory location.

## systemd

The following commands are operator installation instructions, not actions run by
this repository automatically:

```sh
sudo cmake --install build --prefix /usr/local
sudo groupadd --system laso
sudo useradd --system --gid laso --home-dir /var/lib/laso laso
sudo install -d -m 0750 -o root -g laso /etc/laso
sudo install -m 0640 -o root -g laso deploy/examples/laso.yaml /etc/laso/laso.yaml
sudo install -m 0644 deploy/systemd/laso.service /etc/systemd/system/laso.service
sudo systemctl daemon-reload
sudo systemctl enable --now laso
curl -fsS http://127.0.0.1:8080/api/v1/health
journalctl -u laso
```

Skip account creation if the account already exists. `StateDirectory` supplies a
private writable state directory. The sample unit is a hardening starting point;
its behavior has not yet been tested on systemd here. Trusted plugin locations
must remain readable under its filesystem restrictions. SIGTERM/SIGINT are handled
through Asio signals, with no manual daemonization.

## Containers and Debian validation

```sh
docker build --target validation -t laso-validation .
docker run --rm laso-validation
docker compose -f deploy/docker/compose.yaml up --build
```

The multi-stage Dockerfile runs CTest while building. The runtime image contains
native runtime dependencies and an unprivileged service user, not a compiler or
managed runtime. A named volume preserves state. The Linux host-network Compose
example makes the loopback API accessible on the host without exposing an
unauthenticated service to every interface. An alternative bridged deployment
requires deliberate binding/port publication and authentication decisions.

Package installation and Docker builds require distribution network access;
the installed framework and test suite do not require external services. The
healthcheck probes local HTTP only. Docker and systemd validation are PENDING.
