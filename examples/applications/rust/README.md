# protocol-printer — a Rust application, using euclid-rdk

Prints every message published to the ENS topic `protocol` to stdout, one per line, exactly as it
was published. The [file-delivery](../cpp) example publishes a JSON message there for every step it
takes, so this prints that application's protocol as a stream:

```text
{"timestamp":"2026-11-15T14:02:11.482Z","application":"file-delivery","step":"picked-up",...}
{"timestamp":"2026-11-15T14:02:11.613Z","application":"file-delivery","step":"downloaded",...}
```

It's the Rust counterpart of the [C++ example](../cpp), and the one that shows the `RUST` runtime.

## How it works

A topic isn't read directly. Its messages are delivered to the queues subscribed to it. So:

1. **On startup** it looks up the topic, creates its own queue if it doesn't exist yet, and
   subscribes that queue to the topic. A restart doesn't subscribe a second time, because it checks
   `list-subscriptions` first. The topic itself has to exist already; `setup-infrastructure.sh`
   creates it.
2. **Then it long-polls the queue** (up to 20 seconds, 10 messages at a time). It prints each
   message, flushes stdout, and only then deletes the message. A crash in between prints a message
   twice rather than never.
3. **On SIGTERM or Ctrl-C** it stops waiting and exits. A batch already received is printed first.

stdout carries the messages and nothing else. The application's own lines (starting, stopping,
errors) go to stderr, so `| jq` works.

Every instance of the application shares one queue, so each message is printed once, by one of
them. Messages published before the subscription existed aren't delivered: a subscription only
receives what is published after it was made.

## Configuration

| Variable | Default | What it is |
|---|---|---|
| `PROTOCOL_TOPIC` | `protocol` | the topic to print |
| `PROTOCOL_QUEUE` | `<application id>-queue`, or `protocol-printer-local-queue` outside euclid | the queue subscribed to it |

Outside euclid, where the manager provides no credentials:

| Variable | Default | What it is |
|---|---|---|
| `EUCLID_URL` | `https://localhost:5566` | the gateway |
| `EUCLID_USER`, `EUCLID_PASSWORD` | | who to log in as |
| `EUCLID_NAMESPACE` | the user's default | the namespace the topic is in, `development` in `examples/docker` |
| `EUCLID_CA_CERT_PATH` | euclid's installed certificate | the gateway's certificate, if it is self-signed and not the installed one |

## Run it locally

```bash
cargo build --release

# The gateway certificate of the examples/docker stack:
docker compose -f ../../docker/docker-compose.yml cp euclid:/usr/local/euclid/certs/euclid_cert.crt /tmp/gateway.crt

EUCLID_USER=admin EUCLID_PASSWORD=admin EUCLID_NAMESPACE=development \
EUCLID_CA_CERT_PATH=/tmp/gateway.crt ./target/release/protocol-printer | jq .
```

A local run uses a queue of its own (`protocol-printer-local-queue`), so it doesn't take messages
away from a deployed instance. With a user and password it logs in without touching the session
`euclid-cli` keeps in `~/.euclid/credentials`. Without them it uses a session euclid-rdk or
euclid-jdk cached. A session cached by `euclid-cli` doesn't work, because it doesn't record which
server it's for.

The local queue and its subscription stay after you stop it, and keep collecting messages. Remove
them when you're done:

```bash
euclid-cli ens list-subscriptions --topic protocol        # find the one to protocol-printer-local-queue
euclid-cli ens unsubscribe --subscription <ern>
euclid-cli eqs delete-queue --queue protocol-printer-local-queue
```

## Build for the workers

The `euclid-wrk` image of `examples/docker` is Alpine, which uses musl rather than glibc, so a
binary built on Debian or Ubuntu won't start there. `build-alpine.sh` builds it in an Alpine
container, fully static (musl and OpenSSL linked in), and leaves it in `examples/docker/apps/`:

```bash
./build-alpine.sh
```

The result runs on any x86-64 Linux, Alpine or not.

It uses OpenSSL (`native-tls`, euclid-rdk's default) rather than rustls on purpose: the gateway of a
development euclid has a self-signed certificate, and rustls refuses a self-signed certificate as
its own trust anchor.

## Deploy

```bash
euclid-cli esm upload-file --bucket apps --key protocol-printer --file ../../docker/apps/protocol-printer

euclid-cli eap create-application --application-id protocol-printer --runtime RUST \
    --bucket apps --artifact protocol-printer --version 1.0.0 \
    --min-instances 1 --max-instances 1

euclid-cli eap start-application --application-id protocol-printer
```

Its stdout lands in euclid's log under the application's channel.

**Grants.** `create-application` gives the application its own principal, `app-protocol-printer`, in
the built-in `application` role. That role covers everything it does: looking up the topic,
subscribing, and creating and reading its queue. Deployed without `--buckets` or `--queues`, as
above, the role applies to every resource in the namespace.

To narrow it, deploy with `--queues protocol-printer-queue`, as `setup-infrastructure.sh` does. The
queue has to exist by then, since `create-application` refuses a resource that doesn't. ENS checks
only that a role holds the action, not which topic it names, so the built-in role still covers the
topic. But `create-application` can't name a topic, so a role on it is what keeps the application
allowed once ENS does check:

```bash
euclid-cli eam create-role --name protocol-printer \
    --permission ens:get-topic-ern --permission ens:subscribe --permission ens:list-subscriptions \
    --description "Subscribe to the protocol topic"

euclid-cli eam grant-role --role protocol-printer \
    --principal ern:eam:eu-central-1:000000000000:user:app-protocol-printer \
    --namespace development \
    --resource 'ern:ens:eu-central-1:000000000000:development:topic:protocol'
```

## What it demonstrates

**Reading a topic through a queue**, which is how every euclid consumer reads one.

**An application's credentials in Rust.** The manager hands an application a short-lived token in
the file `EUCLID_CREDENTIALS_FILE` names. euclid-rdk only reuses a cached token whose file records
the server it's for, and the application's file names it `endpoint` rather than `baseUrl`. So
`connect()` copies the token into a file of the shape euclid-rdk expects, opens the session from
that (no password, no call to the server), and deletes the copy.

**Token rotation.** The manager replaces the token halfway through its life, and a client keeps
presenting the token it was made with. So before each receive the application checks whether the
file holds a new token, and opens a new session if it does. Without that it would work for one
token's lifetime and then get `401`s.

**Delete last.** A message deleted before it is printed is a message lost. One printed and not
deleted is printed twice, which is the better failure.
