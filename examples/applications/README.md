# Applications

An application is a process euclid runs and scales on your behalf. It can be written in any
language: euclid starts it, restarts it if it dies, scales it between a minimum and maximum
instance count, routes requests to it through the gateway, and hands it credentials so it can call
back into euclid. Nothing about it is C++, and it links no euclid library.

## The contract

1. **Listen on `EUCLID_SOCKET`.** The manager passes a Unix socket path in that environment
   variable (and, for euclid's own modules, as `--socket`). Creating the socket is the readiness
   signal: the manager waits for it to appear and kills the process if it does not show up within
   the application's `readyTimeoutMs`.
2. **Speak HTTP/1.1 on that socket**, dispatching on the `x-euclid-action` request header, the way
   every euclid module does. Reply with JSON.
3. **Log to stdout and stderr.** The manager drains both into euclid's log.
4. **Exit on SIGTERM.**

That is the whole of it. `python/euclid_app.py` implements it in the standard library.

## What the manager puts in the environment

| Variable | Meaning |
|---|---|
| `EUCLID_SOCKET` | Unix socket this instance must listen on |
| `EUCLID_APPLICATION_ID`, `EUCLID_APPLICATION_ERN` | which application this is |
| `EUCLID_ACCOUNT_ID`, `EUCLID_REGION`, `EUCLID_USER_ID` | the identity it runs as |
| `EUCLID_ENDPOINT` | gateway URL to call other modules through |
| `EUCLID_SIGNATURE` | `rfc9421` — how to sign those calls |
| `EUCLID_CREDENTIALS_FILE` | file holding a short-lived bearer token for the application's identity |
| `EUCLID_ACCESS_KEY_ID`, `EUCLID_SECRET_ACCESS_KEY` | an access key — only for an application deployed with `--user`, whose key its operator manages |

Anything set on the application definition (`--environment`) is added underneath, so it cannot
shadow these.

## Calling back into euclid

The usual way is the token in `EUCLID_CREDENTIALS_FILE`, sent as `Authorization: Bearer <token>`.
The file is JSON:

```json
{
  "token": "eyJ0eXAi...",
  "expiresAt": "2026-09-01T15:07:36.000Z",
  "userId": "app-inbox",
  "accountId": "000000000000",
  "region": "eu-central-1",
  "endpoint": "https://localhost:5566"
}
```

**Re-read it; don't cache the token.** It is valid for an hour by default
(`euclid.modules.eap.credentials-ttl-seconds`), and the manager replaces the file once less than
half of that is left — atomically, by writing alongside and renaming, so a reader never sees half a
file. An application that held on to the first token it saw would work for an hour and then start
getting `401 Bearer token expired`. Both examples here re-read the file rather than remember it;
the Java one rebuilds its euclid-jdk session when the token changes.

This is the arrangement AWS uses for container and web-identity credentials, for the same reason:
after `exec()` an environment cannot be rewritten, so anything that has to be replaced while a
process runs has to live in a file.

An application deployed with `--user` gets that user's access key instead, and signs with
[RFC 9421](https://www.rfc-editor.org/rfc/rfc9421) HTTP Message Signatures. The signature must
cover exactly these components, in this order:

```
"@method" "@path" "@authority" "content-digest"
"x-euclid-account-id" "x-euclid-action" "x-euclid-region" "x-euclid-target" "x-euclid-user-id"
```

with `alg="hmac-sha256"`, a `created` timestamp within 15 minutes of now, and a `Content-Digest`
header (RFC 9530) over the body. The component list is fixed at both ends on purpose: a signature
covering less could be stripped down in transit and still verify.

`python/euclid_app.py`'s `sign()` is a complete implementation in about twenty lines; the euclid
side is `Core::HttpSignature`, and `tests/HttpSignatureTest.cpp` pins the two against each other
with a recorded vector produced by that Python code.

## Being told when a bucket changes

An application that has to react to objects rather than poll for them subscribes to ESM's object
events. There are three, and every path that changes an object publishes one — an upload, a
multipart completion, a copy, a move, a rename, an attribute change, a delete, and a purge (one per
object it removed):

| Event | Published when |
| --- | --- |
| `esm.object.created` | an object appears at a key that held none |
| `esm.object.updated` | an existing key is re-uploaded, or an attribute changes |
| `esm.object.deleted` | an object is removed, including the source side of a move |

A move is a `created` followed by a `deleted`, in that order, so a listener keeping its own index of
keys never has the object missing from both places.

The payload is flat, and every field can be filtered on:

```json
{
  "ern": "ern:esm:eu-central-1:000000000000:development:object:inbox/reports/q3.csv",
  "bucketErn": "ern:esm:eu-central-1:000000000000:development:bucket:inbox",
  "bucketName": "inbox", "key": "reports/q3.csv", "prefix": "reports/", "directory": false,
  "size": 4211, "contentType": "text/csv", "md5Sum": "2aacead6864fa88adab90b825464f87c",
  "owner": "admin", "userId": "admin", "accountId": "000000000000",
  "region": "eu-central-1", "namespace": "development",
  "eventTime": "2026-09-01T14:57:20.842631687Z"
}
```

Subscribing is four actions on the `ees` module — `subscribe-events`, `receive-events` (long-polls,
up to 20 seconds), `ack-events`, `unsubscribe-events`:

```json
{"name": "invoice-import",
 "eventTypes": ["esm.object.created", "esm.object.updated"],
 "filter": {"bucketName": "inbox", "prefix": "reports/", "directory": false}}
```

A filter is exact-match, and is applied when the event is published rather than when it is
delivered — so a subscriber's backlog is proportional to what it asked for, not to how busy the
installation is. `prefix` is what makes "this directory" expressible; `directory: false` keeps the
zero-byte markers an FTP `MKD` leaves behind out of it.

Three things follow from events being stored per subscriber rather than in a queue you create:

- **Nothing is lost while you are down.** An event waits until the subscriber claims it, and is
  removed only when it acks. Redeploying an application does not miss the objects that arrived
  during the restart.
- **Two applications watching the same bucket both get it.** One acking its copy does not take the
  event from the other, so no queue per consumer is needed.
- **Two instances of one application share the work.** They use the same subscriber name, and a
  claim is atomic, so exactly one of them handles each event. An unacked claim becomes visible
  again when its lease runs out, which is what makes a consumer crashing mid-work harmless.

An unclaimed event expires after seven days, so an application that never comes back cannot fill the
database.

## Deploying one

The artifact lives in an ESM bucket, so deployment is an ordinary upload — through the CLI, an
SDK, or an FTP/SFTP transfer server:

```bash
euclid-cli esm create-bucket --name apps
euclid-cli esm upload-file --bucket apps --key euclid_app.py --file python/euclid_app.py

euclid-cli eap create-application \
    --application-id demo --runtime PYTHON \
    --bucket apps --artifact euclid_app.py \
    --min-instances 1 --max-instances 4

euclid-cli eap start-application --application-id demo
euclid-cli eap list-applications
```

## Who an application is

Nothing above named a user, and that is deliberate: `create-application` gives the application a
**technical principal** of its own, `app-<application-id>`. It is an EAM identity with no password
and `loginEnabled` false - `eam login` refuses it - whose whole capability is the one access key
the manager injects. Everything the application does is attributed to it rather than to whoever
deployed it, and `delete-application` deletes it again, so no credential outlives what it was
issued to.

Pass `--user <existing-user>` only when an application really should act as somebody who already
exists; that user is then yours to manage, and is left alone when the application is deleted.

The principal is granted its own account - the one the application belongs to - and the namespace
it was created in. It needs that much to be allowed to do anything at all: every module checks the
account a request names against the grants its caller holds, so a principal with no grant would
authenticate perfectly and then be refused by everything.

Name the resources an application needs and the principal is narrowed to exactly them:

```bash
euclid-cli eap create-application \
    --application-id inbox --runtime JAVA \
    --bucket apps --artifact euclid-inbox-app.jar \
    --buckets inbox --queues inbox-queue
```

The storage and queueing modules then refuse anything else that principal asks for - a compromised
application reaches what it was deployed with and nothing more. `update-application --buckets/--queues`
re-grants it, taking effect immediately, for running instances too. Name neither and the principal
keeps the account-wide access it has always had.

Enforced today on the data-plane actions an application uses: ESM's put-object, get-object,
list-objects, delete-object, create-upload, complete-upload, purge-bucket and the object attribute
actions, and EQS's send-message and receive-messages. Everything else remains account-scoped.

A technical principal's own access key never leaves EAM: the process is handed only the expiring
token described under **Calling back into euclid**, so there is nothing long-lived in it to steal.

The manager copies the artifact out of the bucket to `data/application/demo/`, starts it with
`python3`, and from then on treats it like any other module: round-robin across instances, scale
out when every instance is busy, scale back down after
`euclid.scaling.scale-down-idle-seconds` of idleness, restart on crash.

Reach it through the gateway the same way you reach a module — `x-euclid-target: demo` with an
`x-euclid-action` the application implements.

Redeploying is an upload followed by a restart:

```bash
euclid-cli esm upload-file --bucket apps --key euclid_app.py --file python/euclid_app.py
euclid-cli eap stop-application  --application-id demo
euclid-cli eap start-application --application-id demo
```

## The Java example

`java/` is the same contract in Java, using the [euclid-jdk](https://github.com/jensvogt/euclid-jdk)
to talk back to euclid: it logs in, makes sure a bucket and a queue exist, then logs every file
that appears in the bucket and every message that arrives on the queue.

```bash
cd java
mvn package                              # -> target/euclid-inbox-app.jar (one self-contained jar)

euclid-cli esm upload-file --bucket apps --key euclid-inbox-app.jar --file target/euclid-inbox-app.jar
euclid-cli eap create-application \
    --application-id inbox --runtime JAVA \
    --bucket apps --artifact euclid-inbox-app.jar \
    --ready-timeout 30000
euclid-cli eap start-application --application-id inbox
```

Then drop a file in and watch euclid's log:

```bash
euclid-cli esm upload-file --bucket inbox --key report.csv --file report.csv
# ... new file: key=report.csv size=1234 contentType=text/csv
```

Subscribe the bucket to the queue and the same upload arrives twice - once as the file, once as the
notification ESM published about it:

```bash
euclid-cli esm subscribe --source-ern <inbox bucket ern> --type SQS --target-ern <inbox-queue ern>
```

Note what it does *not* need: a username or a password. Deployed by euclid it builds its
euclid-jdk session straight from the injected access key, because its principal has no login to
use. From a command line, where there is no such key, it falls back to logging in.

Two details in it are worth copying into any application:

- **It binds its socket before logging in.** Readiness is the process's own business; an
  application that waited for a remote login before creating its socket would be killed and
  restarted every time EAM was briefly slow, which is a worse failure than being up and retrying.
- **It answers `ping` and `status` regardless**, so `status` reports `connected: false` while it is
  still trying rather than the application looking dead.

Run it from a command line too - after `euclid-cli eam login` it picks up the cached session, and
with no `EUCLID_SOCKET` set it simply skips the socket half:

```bash
java -jar target/euclid-inbox-app.jar
```

## What an application runs as

The manager starts every application as itself, and the service runs as the unprivileged `euclid`
user - so applications run as `euclid` too, not as root. That is one uid for the manager, every
module and every application: it keeps them all off root, but it does not separate applications
from each other. A per-application uid would need the manager to hold `CAP_SETUID`, which it
deliberately does not.

Identity inside euclid is a different matter, and is separated: each application authenticates as
its own principal with its own short-lived credentials, whatever uid it happens to run under.

## Runtimes

| Runtime | Started as | Notes |
|---|---|---|
| `JAVA` | `java -jar <artifact>` | give it a `readyTimeoutMs` that fits JVM start-up |
| `PYTHON` | `python3 <artifact>` | |
| `NODEJS` | `node <artifact>` | |
| `BINARY` | `<artifact>` | Rust, C++, Go — anything executable; the manager sets the exec bit |

`--command` overrides all of it when an application needs something else entirely (a wrapper
script, an interpreter that isn't on `PATH`). The interpreter is resolved through `PATH`: euclid
launches processes, it does not manage language toolchains.
