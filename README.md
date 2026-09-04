# euclid

![Release](https://img.shields.io/github/v/release/jensvogt/euclid)
![License](https://img.shields.io/github/license/jensvogt/euclid)
![Language](https://img.shields.io/github/languages/top/jensvogt/euclid)
![CI](https://img.shields.io/github/actions/workflow/status/jensvogt/euclid/test.yml)

> A lightweight, modular cloud-services emulator written in modern C++ - one small
> gateway process, independent per-service module processes, and a single CLI.

---

## What is this?

euclid runs a local gateway that authenticates requests and routes them, by service name, to one of several independent
module processes it manages as subprocesses - each communicating with the gateway over a Unix domain socket. Persistence
is pluggable: an in-memory backend for fast, disposable test runs, or MongoDB for state that survives a restart.

Requests are authenticated one of three ways: a JWT bearer token from `eam login`, an [RFC 9421](https://www.rfc-editor.org/rfc/rfc9421)
HTTP Message Signature (the default for signed calls), or AWS-style SigV4 for clients that need it. See
[Signing](#signing) below.

| Module                             | What it does                                                                                | Status     |
|------------------------------------|---------------------------------------------------------------------------------------------|------------|
| **eam**                            | Users, user groups, accounts, namespaces, JWT login sessions and access keys                  | ✅          |
| **eqs**                            | Queues: delayed and dead-letter delivery, priority-weighted receive, long polling             | ✅          |
| **ens**                            | Notifications: publish/subscribe topics fanning out to queues                                 | ✅          |
| **esm**                            | Storage: buckets and objects, multipart transfer, encryption at rest                          | ✅          |
| **ees**                            | Events: subscribe to what the other modules publish                                           | ✅          |
| **ekm**                            | Key management: cryptographic keys, encrypt/decrypt                                           | ✅          |
| **emm**                            | Module management: start, stop, restart, instance and thread limits, export/import            | ✅          |
| **ets**                            | Transfer servers: FTP and SFTP endpoints onto ESM buckets                                     | ✅          |
| **eap**                            | Applications: Java, Python, Node.js, Rust or C++ processes euclid runs, scales and supervises | ✅          |
| **emo**                            | Monitoring: metric collection, rollup and retention behind the other modules                  | ✅          |
| dynamodb, secretsmanager, ssm, ... | Reserved service names in the gateway's routing table                                         | 🚧 planned |

Everything is driven through `euclid-cli`, a single client binary with one subcommand set per module
(`euclid-cli eqs ...`, `euclid-cli eam ...`), or through the desktop UI - see [Related projects](#related-projects).

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
euclid-cli eam login --user admin --password admin

euclid-cli eqs create-queue --name my-queue
euclid-cli eqs send-message --queue my-queue --body "hello" --priority HIGH
euclid-cli eqs receive-messages --queue my-queue --maxCount 10
```

---

## Signing

A request reaches the gateway with one of three credentials.

| Scheme | Header(s) | Used by |
|--------|-----------|---------|
| **Bearer token** | `Authorization: Bearer <jwt>` | `eam login` sessions; the RUI by default |
| **RFC 9421** | `Signature`, `Signature-Input`, `Content-Digest` | `euclid-cli` by default, the RUI when signing, and every euclid application |
| **SigV4** | `Authorization: AWS4-HMAC-SHA256 ...` | clients that need AWS compatibility (`--signature sigv4`) |

[RFC 9421](https://www.rfc-editor.org/rfc/rfc9421) HTTP Message Signatures is the default for signed calls. It proves
the request with the same access key SigV4 uses, but says so in an open standard: the signature lives in
`Signature`/`Signature-Input`, and the body is bound through a `Content-Digest` header ([RFC 9530](https://www.rfc-editor.org/rfc/rfc9530))
that means something on its own rather than being folded into a proprietary canonical string.

The server does not take a signature on trust:

- the covered components are **fixed by the server**, not negotiated - a signature that covers less than method, path,
  authority, content-digest and euclid's routing headers is rejected;
- the algorithm is fixed, so there is nothing to downgrade;
- `Content-Digest` is recomputed from the body actually received and compared before the signature means anything;
- `created` must sit within a 15-minute window, in either direction;
- digests and signatures are compared in constant time.

Switch a single call with `--signature sigv4`, or an installation with `euclid.cli.signature` in the configuration file.

---

## Architecture

- **Gateway** (`euclid-mgr`) - single HTTP(S) entry point. Authenticates the caller, identifies the target service from
  the `x-euclid-target` header, then forwards the request asynchronously over a Unix domain socket to one instance of
  that module. Long-polling callers park on the gateway's I/O context rather than holding a worker thread, so a module's
  concurrency is bounded by its own instances and threads rather than by the gateway.
- **Modules** (`euclid-eam`, `euclid-eqs`, `euclid-emo`, ...) - independent processes, started and supervised by the
  gateway, each owning one service's logic and its own socket.
- **Storage** - `euclid.database.backend` selects `mongodb` (persistent) or
  `memory` (in-process, wiped on restart).
- **CLI** (`euclid-cli`) - talks to the gateway over HTTPS; credentials are cached under `$HOME/.euclid/credentials`
  after `euclid-cli eam login`.
- **Names** - a queue, topic or bucket may be named rather than spelled out as a full ERN. The server resolves a bare
  name in the caller's own account and namespace, which is what keeps account, region and namespace out of client
  configuration entirely - and means a name can never reach another namespace.

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

### The CLI on its own

The packages above all contain `euclid-cli`, because a server is usually administered from the
machine it runs on. For a machine that only ever talks to a euclid somewhere else - a workstation,
a build agent, a container - there is a package with just the command and its manual pages, and
none of the module binaries, systemd unit or service account a server needs:

```bash
sudo apt install euclid-cli          # from the APT repository added above
```

```bash
wget https://jensvogt.github.io/euclid/euclid-cli-<version>-amd64.deb
sudo apt install ./euclid-cli-<version>-amd64.deb
```

```bash
wget https://jensvogt.github.io/euclid/euclid-cli-<version>-1.x86_64.rpm
sudo rpm -i euclid-cli-<version>-1.x86_64.rpm
```

On Linux both packages install the same command at the same path, so a machine wants one or the
other: installing `euclid` where `euclid-cli` is present replaces it, and vice versa.

macOS - unpacks straight into `/usr/local`, which puts the command on the PATH and the manual
pages where `man` looks for them:

```bash
wget https://jensvogt.github.io/euclid/euclid-cli-<version>-macos.tgz
sudo tar -xzf euclid-cli-<version>-macos.tgz -C /usr/local
```

Windows - download and run `euclid-cli-<version>-amd64.exe`, which installs the command and adds
it to the system PATH. Open a new terminal afterwards; an existing one keeps the PATH it started
with.

Then point it at the server:

```bash
euclid-cli --endpoint https://euclid.example.com eam login --user jens --password <secret>
```

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
`/etc/euclid/euclid.json`; see `dist/linux/etc/euclid.json` for the full, commented reference). Key defaults:

| Setting                                       | Default | Purpose                                                             |
|------------------------------------------------|---------|-----------------------------------------------------------------------|
| `euclid.gateway.http.port`                    | 5566    | Gateway HTTP(S) entry point       |
| `euclid.gateway.websocket.enabled`            | true    | Accept websocket upgrades on the gateway HTTP(S) port |
| `euclid.gateway.websocket.max-message-size`   | 1048576 | Max inbound websocket frame size, in bytes |
| `euclid.gateway.websocket.idle-timeout-seconds` | 300   | Websocket ping/pong idle timeout |
| `euclid.gateway.event-socket-path`            | (none)  | Unix domain socket modules push business events to, for websocket clients (`Core::EventPusher`) |
| `euclid.frontend.port`                        | 4567    | Static frontend (when built)      |
| `euclid.logging.websocket-port`               | 4569    | Live log streaming                |
| `euclid.database.backend`                     | mongodb | `mongodb` or `memory`             |
| `euclid.modules.sqs.priority-weights`         | 4:2:1   | HIGH:MIDDLE:LOW receive weighting |

---

## Related projects

| Project | What it is |
|---------|------------|
| [euclid-rui](https://github.com/jensvogt/euclid-rui) | Desktop UI (Qt/QML) - browse and administer queues, topics, buckets, keys, applications and transfer servers, with live metrics |
| [euclid-jdk](https://github.com/jensvogt/euclid-jdk) | Java client library for every module |
| [euclid-spring](https://github.com/jensvogt/euclid-spring) | Spring Boot starter: `@QueueListener`, `@TopicListener` and `@BucketListener`, plus autoconfiguration |

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
