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
is pluggable: MongoDB for state that survives a restart, or an in-memory store for a disposable installation that needs
no database at all - either inside one process, or held by the EMD module and shared by all of them over a socket.

Requests to that gateway are authenticated one of three ways: a JWT bearer token from `eam login`, an
[RFC 9421](https://www.rfc-editor.org/rfc/rfc9421) HTTP Message Signature (the default for signed calls), or AWS-style
SigV4 for clients that need it. See [Signing](#signing) below. The API gateway (`eag`) is separate and decides per
route - see [Architecture](#architecture).

| Module  | What it does                                                                                    | Status |
|---------|-------------------------------------------------------------------------------------------------|--------|
| **eam** | Users, user groups, accounts, namespaces, JWT login sessions and access keys                    | ✅     |
| **eqs** | Queues: delayed and dead-letter delivery, priority-weighted receive, long polling               | ✅     |
| **ens** | Notifications: publish/subscribe topics fanning out to queues                                   | ✅     |
| **esm** | Storage: buckets and objects, multipart transfer, encryption at rest                            | ✅     |
| **ees** | Events: subscribe to what the other modules publish                                             | ✅     |
| **ekm** | Key management: cryptographic keys, encrypt/decrypt, TLS certificates                           | ✅     |
| **ess** | Secrets store: passwords and connection details, encrypted under an EKM key                     | ✅     |
| **emm** | Module management: start, stop, restart, instance and thread limits, export/import              | ✅     |
| **ets** | Transfer servers: FTP and SFTP endpoints onto ESM buckets                                       | ✅     |
| **eap** | Applications: Java, Python, Node.js, Rust or C++ processes euclid runs, scales and supervises   | ✅     |
| **emo** | Monitoring: metric collection, rollup and retention behind the other modules                    | ✅     |
| **eag** | API gateway: publishes paths to the outside world and proxies them to EAP application instances | ✅     |

Everything is driven through `euclid-cli`, a single client binary with one subcommand set per module
(`euclid-cli eqs ...`, `euclid-cli eam ...`), through the desktop UI, or from a program through the Java or Python
client libraries - see [Related projects](#related-projects).

---

## Quick start

Build and run everything. The shipped configuration uses MongoDB. For an installation that should leave nothing behind,
set `euclid.database.backend` to `emd` and activate the `emd` module - see
[Running without a database](#running-without-a-database). The `memory` backend lives inside one process and does not
carry a login from the module that issued it to the module being called, so it will not do for the flow below:

```bash
git clone https://github.com/jensvogt/euclid.git
cd euclid
cmake -B build -DCMAKE_TOOLCHAIN_FILE=<path-to-vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build --parallel

sudo ./build/bin/euclid-mgr --config dist/linux/etc/euclid.json
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

# A secret is encrypted under an EKM key on the way in; --value-stdin keeps it
# out of the process list and the shell history.
openssl rand -base64 24 | euclid-cli ess create-secret --name db-password --value-stdin
euclid-cli ess get-secret --name db-password --raw
```

---

## Signing

A request reaches the gateway with one of three credentials.

| Scheme           | Header(s)                                        | Used by                                                                     |
|------------------|--------------------------------------------------|-----------------------------------------------------------------------------|
| **Bearer token** | `Authorization: Bearer <jwt>`                    | `eam login` sessions; the RUI by default                                    |
| **RFC 9421**     | `Signature`, `Signature-Input`, `Content-Digest` | `euclid-cli` by default, the RUI when signing, and every euclid application |
| **SigV4**        | `Authorization: AWS4-HMAC-SHA256 ...`            | clients that need AWS compatibility (`--signature sigv4`)                   |

[RFC 9421](https://www.rfc-editor.org/rfc/rfc9421) HTTP Message Signatures is the default for signed calls. It proves
the request with the same access key SigV4 uses, but says so in an open standard: the signature lives in
`Signature`/`Signature-Input`, and the body is bound through a `Content-Digest` header
([RFC 9530](https://www.rfc-editor.org/rfc/rfc9530))
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

- **Gateway** (`euclid-mgr`) - single HTTP (S) entry point. Authenticates the caller, identifies the target service from
  the `x-euclid-target` header, then forwards the request asynchronously over a Unix domain socket to one instance of
  that module. Long-polling callers park on the gateway's I/O context rather than holding a worker thread, so a module's
  concurrency is bounded by its own instances and threads rather than by the gateway.
- **Modules** (`euclid-eam`, `euclid-eqs`, `euclid-emo`, ...) - independent processes, started and supervised by the
  gateway, each owning one service's logic and its own socket.
- **API gateway** (`euclid-eag`) - a second, quite different listener. Where the gateway above speaks euclid's own
  protocol and routes by `x-euclid-target`, this one speaks nothing but HTTP and routes by path, to the application
  instances EAP is running. It exists because an autoscaled application has no fixed address: its instances come and go
  and their ports change, so nothing outside can be told where to send a request. Each route decides what it requires of
  a caller - nothing, a euclid credential, or HTTP Basic against a euclid password, which is the one that makes a
  browser
  prompt. A route may also name a euclid module and one of its actions, which is how something outside reaches
  `eam login` without a second port to talk to. Each of its listeners says what it speaks - `"protocol": "http"` or
  `"https"` - and an HTTPS one terminates TLS itself with a certificate held by EKM
  (`euclid-cli ekm import-certificate`),
  generating a self-signed one if it has not been given any, so an installation can serve HTTPS before anybody has
  bought
  it a certificate. `euclid-cli eag list-listeners` reports what it ended up bound to: which port speaks what, and
  whether the certificate behind an HTTPS one is somebody's or that self-signed stopgap.
- **Storage** - `euclid.database.backend` selects `mongodb` (persistent), `emd` (in memory, shared by every module,
  wiped on restart) or `memory` (in memory, private to one process). ESM object files are named after a generated UUID
  and stored fanned out over
  two levels of that name (`<data-dir>/objects/96/71/96719be3-…`): one directory per object storage stops being viable
  at a few million files, where ext4's directory index reaches its maximum depth and refuses new names with ENOSPC on a
  disk that is nearly empty. Two levels is 65,536 directories with the same room again in each, so the ceiling moves
  from millions of objects to billions. Objects written before the fan-out are still read and deleted where they are,
  so nothing has to be migrated.
- **Secrets** - ESS holds the passwords, connection strings and tokens the things euclid runs need in order to reach
  anything else, so they do not end up in a configuration file, an environment variable or a deployment script. Every
  value is encrypted under an EKM key before it is stored and decrypted only by `ess get-secret`: the key management
  module keeps the key, the secrets store keeps the ciphertext, and a copy of either alone - a database dump, a backup -
  is worth nothing. A secret records which key it was written under, so one can be re-keyed without touching any other,
  and a secret that names no key gets the namespace's own, which the module creates the first time something needs it.
- **CLI** (`euclid-cli`) - talks to the gateway over HTTPS; credentials are cached under `$HOME/.euclid/credentials`
  after `euclid-cli eam login`.
- **Names** - a queue, topic, bucket or secret may be named rather than spelled out as a full ERN. The server resolves
  a bare name in the caller's own account and namespace, which is what keeps account, region and namespace out of client
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

| Setting                                         | Default     | Purpose                                                                                                                                                                                 |
|-------------------------------------------------|-------------|-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| `euclid.gateway.http.port`                      | 5566        | Gateway HTTP(S) entry point                                                                                                                                                             |
| `euclid.gateway.websocket.enabled`              | true        | Accept websocket upgrades on the gateway HTTP(S) port                                                                                                                                   |
| `euclid.gateway.websocket.max-message-size`     | 1048576     | Max inbound websocket frame size, in bytes                                                                                                                                              |
| `euclid.gateway.websocket.idle-timeout-seconds` | 300         | Websocket ping/pong idle timeout                                                                                                                                                        |
| `euclid.gateway.event-socket-path`              | (none)      | Unix domain socket modules push business events to, for websocket clients (`Core::EventPusher`)                                                                                         |
| `euclid.database.backend`                       | mongodb     | `mongodb`, `emd` (the shared in-memory store) or `memory` (in-process) - see [Running without a database](#running-without-a-database)                                                  |
| `euclid.modules.emd.socketPath`                 | (none)      | Socket the memory database listens on; every module reaches the store here                                                                                                              |
| `euclid.modules.emd.connect-timeout-ms`         | 1000        | How long a module retries reaching the store before a query fails                                                                                                                       |
| `euclid.logging.level`                          | info        | Level every channel logs at unless it says otherwise                                                                                                                                    |
| `euclid.logging.channels`                       | (none)      | Per-channel levels, e.g. `{"app.parser": "off"}` - see [Logging channels](#logging-channels)                                                                                            |
| `euclid.modules.eqs.priority-weights`           | 4:2:1       | HIGH:MIDDLE:LOW receive weighting                                                                                                                                                       |
| `euclid.modules.eag.port`                       | 8080        | API gateway listener; ignored when `listeners` is set                                                                                                                                   |
| `euclid.modules.eag.listeners`                  | (none)      | One listener per namespace, keyed by namespace - for an installation serving more than one environment. Each takes `port`, and optionally `protocol` (`http`/`https`) and `certificate` |
| `euclid.modules.eag.protocol`                   | http        | What the single listener speaks, when `listeners` is not set                                                                                                                            |
| `euclid.modules.eag.certificate`                | (none)      | Name of the EKM certificate an HTTPS listener serves; a self-signed one is generated under that name if it does not exist                                                               |
| `euclid.modules.eag.basic-auth-cache-seconds`   | 60          | How long a verified Basic credential stays verified; 0 checks every request                                                                                                             |
| `euclid.modules.eap.http-port-min` / `-max`     | 9000 / 9999 | Range the manager hands application instances their own HTTP port from                                                                                                                  |

### Running without a database

`euclid.database.backend: "emd"` runs the whole installation on an in-memory store held by the EMD module, which every
other module reaches over a Unix domain socket. Nothing is written to disk and nothing survives a restart, which is the
point: an installation for a test run, a demo or a container that should leave nothing behind. Activate the module and
let the others declare they need it, exactly as `dist/*/etc/euclid.json` already does:

```json
"emd": {
  "active": true,
  "readiness": "liveness",
  "socketPath": "/var/run/euclid/euclid-emd.sock"
},
"eam": {
  "active": true,
  "dependencies": [
    "emd"
  ]
}
```

The repositories are the ones written for MongoDB - there is one implementation, running against whichever backend is
configured - so a module behaves the same either way. Three things do not carry over, all of them features of the
database rather than of euclid:

- **Monitoring's derived tiers.** EMO's sample-weighted averages and its hourly/daily rollups are aggregation
  pipelines, and the store implements documents rather than an aggregation engine. Samples are still collected, stored
  and listed, and EMO's other work - the queue, bucket and topic recounts every module's counters depend on - is
  unaffected; only the aggregated figures are absent, and the module says so once rather than failing on a timer.
  Metrics that are subtly wrong would be worse than metrics that are missing, which is why they are not computed some
  other way.
- **Event change streams.** The event bus normally tails MongoDB's oplog to be woken the moment an event is published.
  That needs a replica set, so on this backend the bus falls back to its poll, which is what delivers events in either
  case - the change stream only ever made it sooner. Delivery is unchanged; latency is the poll interval.
- **TTL expiry.** An index that expires documents is a background job of the database. Events belonging to an
  abandoned external subscriber are removed when the subscription is, rather than a week later.

### Logging channels

Every log record carries a channel: the name of what produced it. A module logs on its own name (`esm`, `eag`, `mgr`),
and output the manager reads back from a process it started is logged on that
process's channel - `app.<applicationId>` for an application, `module.<name>` for a euclid module.
Each channel can be given its own level, or turned off, so one talkative application does not bury
everything euclid itself has to say:

```json
"logging": {
  "level": "info",
  "channels": {
    "app": "warning",
    // every application euclid runs
    "app.parser": "off",
    // except this one, which says nothing at all
    "esm": "debug"
    // while ESM is being looked at
  }
}
```

A channel with no entry of its own follows the nearest enclosing one - `app` covers `app.parser`
without naming it - and `euclid.logging.level` if none of them says anything. `off` silences a
channel entirely, whatever the severity.

Application and module output defaults to `info` rather than following `euclid.logging.level`,
because it used to bypass the log machinery altogether and was printed whatever the level was;
turning the installation down to `warning` should not silently take an application's own logging
with it.

The levels are re-read on `SIGUSR1`, so they can be changed on a running installation without
restarting anything:

```bash
sudo -e /usr/local/euclid/etc/euclid.json
sudo pkill -USR1 '^euclid-'
```

(Matching on the process name rather than `pkill -f` on the full command line: a `-f` pattern also
matches the `sudo` invocation carrying it, which then takes the signal and dies.)

Nothing else in the file is acted on by that signal. (The manager's `SIGHUP` still means "restart
every module", which is not a price worth paying to turn down a log.)

A single application or module can also be turned down without touching the file at all:

```bash
euclid-cli eap set-log-level --application-id parser --level off
euclid-cli emm set-log-level --module esm --level error
euclid-cli eap set-log-level --application-id parser --level default   # and back
```

The level is stored on the application or module row and applied by the manager on its next
reconcile, within seconds. It restarts nothing: a log level is deliberately not part of what the
manager treats as a change of definition, so silencing something noisy does not bounce its
instances. Both are reported by `list-applications` / `list-modules` as `logLevel`.

These filter what the manager passes on - a line written to standard output arrives as information
and one written to standard error as an error - so they turn things *down* completely and *up* only
as far as what the process already emits. Making a module say more is `euclid.logging.level` in its
own process, which is what `SIGUSR1` re-reads.

---

## Related projects

| Project                                                    | What it is                                                                                                                      |
|------------------------------------------------------------|---------------------------------------------------------------------------------------------------------------------------------|
| [euclid-rui](https://github.com/jensvogt/euclid-rui)       | Desktop UI (Qt/QML) - browse and administer queues, topics, buckets, keys, applications and transfer servers, with live metrics |
| [euclid-jdk](https://github.com/jensvogt/euclid-jdk)       | Java client library for every module                                                                                            |
| [euclid-spring](https://github.com/jensvogt/euclid-spring) | Spring Boot starter: `@QueueListener`, `@TopicListener` and `@BucketListener`, plus autoconfiguration                           |
| [euclid-pdk](https://github.com/jensvogt/euclid-pdk)       | Python client library - EAM, ESM, EQS, ENS, EKM and ESS, both signing schemes, and no dependencies beyond the standard library  |

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
