# euclid

![Release](https://img.shields.io/github/v/release/jensvogt/euclid)
![License](https://img.shields.io/github/license/jensvogt/euclid)
![Language](https://img.shields.io/github/languages/top/jensvogt/euclid)
![CI](https://img.shields.io/github/actions/workflow/status/jensvogt/euclid/test.yml)

> A lightweight, modular cloud-services emulator written in modern C++ - one small
> gateway process, independent per-service module processes, and a single CLI.

---

## What is this?

euclid runs a local gateway that authenticates requests (JWT bearer tokens and
AWS-style SigV4 signing) and routes them, by service name, to one of several
independent module processes it manages as subprocesses - each communicating with
the gateway over a Unix domain socket. Persistence is pluggable: an in-memory
backend for fast, disposable test runs, or MongoDB for state that survives a
restart.

It's an early-stage rewrite, currently shipping two functional services plus
metrics:

| Module         | What it does                                                                  | Status |
|----------------|--------------------------------------------------------------------------------|--------|
| **access**     | User accounts, JWT login sessions, AWS-style access keys, admin bootstrap      | ✅      |
| **sqs**        | Queues, delayed/dead-letter delivery, and priority-weighted `receive-messages` | ✅      |
| **monitoring** | Metrics collection/retention behind the other modules                          | ✅      |
| s3, sns, dynamodb, lambda, ... | Reserved service names in the gateway's routing table          | 🚧 planned |

Everything is driven through `euclid-cli`, a single client binary with one
subcommand set per module (`euclid-cli sqs ...`, `euclid-cli access ...`).

---

## Quick start

Build and run everything with the in-memory backend (no MongoDB required):

```bash
git clone https://github.com/jensvogt/euclid.git
cd euclid
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel

./build/bin/euclid-mgr --config dist/linux/etc/euclid.json
```

In another terminal:

```bash
export PATH="$PWD/build/bin:$PATH"

# First run bootstraps a default administrator (userId: admin, password: admin) -
# change the password immediately in anything but a throwaway dev setup.
euclid-cli access login --user admin --password admin

euclid-cli queues create-queues --name my-queues
euclid-cli queues send-message --ern <queues-ern> --body "hello" --priority HIGH
euclid-cli queues receive-messages --ern <queues-ern> --maxCount 10
```

---

## Architecture

- **Gateway** (`euclid-mgr`) - single HTTP(S) entry point. Identifies the target
  service from the `x-euclid-target` header or the SigV4 credential scope, then
  forwards the request over a Unix domain socket to that module's process.
- **Modules** (`euclid-acc`, `euclid-sqs`, `euclid-mon`, ...) - independent
  processes, started and supervised by the gateway, each owning one service's
  logic and its own socket.
- **Storage** - `euclid.database.backend` selects `mongodb` (persistent) or
  `memory` (in-process, wiped on restart).
- **CLI** (`euclid-cli`) - talks to the gateway over HTTPS; credentials are cached
  under `$HOME/.euclid/credentials` after `euclid-cli access login`.

---

## Installation

### Docker

```bash
docker run -p 5566:5566 -p 4567:4567 jensvogt/euclid:latest
```

### Debian / Ubuntu

```bash
wget https://jensvogt.github.io/euclid/euclid-<version>-amd64.deb
sudo apt install ./euclid-<version>-amd64.deb
```

Or add the signed APT repository once, then install/upgrade via `apt`:

```bash
curl -fsSL https://jensvogt.github.io/euclid/apt/euclid-archive-keyring.asc | sudo gpg --dearmor -o /usr/share/keyrings/euclid-archive-keyring.gpg
echo "deb [signed-by=/usr/share/keyrings/euclid-archive-keyring.gpg] https://jensvogt.github.io/euclid/apt stable main" | sudo tee /etc/apt/sources.list.d/euclid.list
sudo apt update
sudo apt install euclid
```

### RPM (RHEL / Fedora)

```bash
wget https://jensvogt.github.io/euclid/euclid-<version>-1.x86_64.rpm
sudo rpm -i euclid-<version>-1.x86_64.rpm
```

### macOS

```bash
wget https://jensvogt.github.io/euclid/euclid-<version>-macos.tgz
tar -xzf euclid-<version>-macos.tgz
```

### Windows

Download and run `euclid-<version>-amd64.exe` from the
[releases page](https://github.com/jensvogt/euclid/releases).

### Build from source

```bash
git clone https://github.com/jensvogt/euclid.git
cd euclid
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
```

Requires a C++23 compiler (GCC 14+/Clang), CMake 3.28+, and
[vcpkg](https://github.com/microsoft/vcpkg) (dependencies are resolved from
`vcpkg.json` automatically). Binaries land in `build/bin/`.

---

## Configuration

Every process reads the same JSON config (`--config <path>`, default
`/etc/euclid/euclid.json`; see `dist/linux/etc/euclid.json` for the full,
commented reference). Key defaults:

| Setting                             | Default | Purpose                          |
|--------------------------------------|---------|-----------------------------------|
| `euclid.gateway.http.port`           | 5566    | Gateway HTTP(S) entry point       |
| `euclid.frontend.port`               | 4567    | Static frontend (when built)      |
| `euclid.logging.websocket-port`      | 4569    | Live log streaming                |
| `euclid.database.backend`            | mongodb | `mongodb` or `memory`             |
| `euclid.modules.sqs.priority-weights`| 4:2:1   | HIGH:MIDDLE:LOW receive weighting |

---

## Contributing

Contributions are welcome, especially:

- New service modules (the gateway already reserves the routing names)
- Broader API coverage for the existing modules
- Bug reports with reproduction steps

Open an issue or PR.

---

## License

[GPL-3.0](LICENSE)
