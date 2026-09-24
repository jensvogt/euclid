# echo-worker — a C++ application, using euclid-cdk

A queue consumer: take a message, do something with it, delete it, repeat. What it does with a
message is one line, because that is the part you would replace. Everything else is what an
application has to get right to be a well-behaved one.

It is the only example here that is **compiled**, so it is also the one that shows the `BINARY`
runtime — no interpreter, the artifact is the executable.

## Build

Standalone, the way your own application's repository would be. It needs
[euclid-cdk](../../../../euclid-cdk) installed, and Boost and OpenSSL findable — euclid-cdk's
exported package resolves `Boost::headers`, `Boost::json`, `Boost::url` and `OpenSSL::SSL` before it
hands you its own target.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 64
```

If the SDK is somewhere cmake does not look by itself, add
`-DCMAKE_PREFIX_PATH=/where/euclid-cdk/is`. Several prefixes are separated by `;`, which is how to
point at an SDK in one place and Boost in another:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="/usr/local/euclid-cdk-0.12.0-linux-x64;/usr/local"
```

## Deploy

```bash
euclid-cli esm create-bucket --name apps
euclid-cli esm upload-file --bucket apps --key echo-worker --file build/echo-worker

euclid-cli eap create-application --application-id echo-worker --runtime BINARY \
    --bucket apps --artifact echo-worker --version 1.0.0 \
    --min-instances 1 --max-instances 4 --queues echo-worker-queue

euclid-cli eap start-application --application-id echo-worker
```

Then give it something to do:

```bash
euclid-cli eqs send-message --queue echo-worker-queue --body 'hello'
euclid-cli eap get-application --application-id echo-worker   # watch "instances" as you send more
```

The worker creates the queue itself if it is not there, so the order does not matter.

## Grants

`create-application` gives the application a technical principal of its own, `app-echo-worker`, and
puts it in the built-in `application` role. That covers five of the six things this worker does:

| What the worker calls | Action | In the `application` role? |
|---|---|---|
| `ExistsQueue`, `GetQueueErn` | `eqs:get-queue-ern` | yes |
| `CreateQueue` | `eqs:create-queue` | yes |
| `ReceiveMessages` | `eqs:receive-messages` | yes |
| `DeleteMessage` | `eqs:delete-message` | yes |
| `ReportLoad` | `eap:report-load` | yes |
| `GetMessageCount` | `eqs:get-message-count` | **no** |

`eap:report-load` being in there matters more than it looks: without it the manager sees nothing, and
the pool never grows however much work is waiting.

### The one that is missing

`eqs:get-message-count` is deliberately not in the role — counting a queue is not something every
application should be able to do to every queue. Without it the worker runs normally and says so
once, but reports no backlog, so only the utilisation half of the autoscaler signal is live: the pool
grows when every instance is saturated, not ahead of that because thousands of messages are waiting.

To get the other half:

```bash
euclid-cli eam create-role --name queue-counter \
    --permission eqs:get-message-count \
    --description "Read a queue's depth, for an application that reports its own backlog"

euclid-cli eam grant-role --role queue-counter \
    --principal ern:eam:eu-central-1:000000000000:user:app-echo-worker \
    --namespace development \
    --resource 'ern:eqs:eu-central-1:000000000000:development:queue:echo-worker-*'
```

The `--resource` pattern is worth passing and worth not relying on: `get-message-count` does not
check it today, so the grant is account-wide in effect. It costs nothing, it records the intent, and
it starts being enforced the day the action joins the ones below.

### What `--queues` actually narrows

`--queues echo-worker-queue` on `create-application` narrows the principal to that queue — for the
EQS actions that check a resource, which as of today are:

```
delete-queue   get-queue         get-message    send-message
send-message-batch   receive-messages   purge-queue    start-queue   stop-queue
```

`receive-messages` is the one that matters here, and it is the action through which every message
this worker touches arrives. `get-queue-ern`, `create-queue`, `delete-message` and
`get-message-count` authenticate but do not compare the queue against the grant, so a narrowed
principal still reaches those four on any queue in its namespace. Worth knowing before treating
`--queues` as a boundary rather than as most of one.

Leave `--queues` off entirely and the principal keeps the account-wide access it gets by default,
which is the right choice for a first deployment and the wrong one to leave in place.

## The five things it is actually demonstrating

**1. A session with no login.** An application has no password. It is handed a bearer token for its
own technical principal in the file `EUCLID_CREDENTIALS_FILE` names, and `Credentials::Load()` reads
exactly that file. `AuthMode::Bearer`, because a technical principal has no access key at all — its
long-lived secret never leaves EAM, so the token is the whole of what the process holds.

**2. `SetTokenProvider`, which is the difference between working and working for an hour.** The
manager replaces the token before it expires. A session handed a copy of the first one collects
`401 Bearer token expired` about an hour in, a long way from the change that caused it. The provider
re-reads the file per request — cheap, and the file is written beside-and-renamed so a reader never
sees half of one.

This is also why the worker never rebuilds its `Eqs` or `Eap` client: `ModuleClient` holds a
*reference* to the session and is neither copyable nor assignable. The session is the mutable thing,
and the clients follow it.

**3. Load reporting, without which the pool never grows.** Nothing asks an application for anything
over a socket, so the manager cannot see its load the way it sees a module's — the application is the
only thing that knows. An application that reports nothing runs one instance forever, whatever the
backlog.

Two figures, read against different bars: `utilisation` (below 5% the idle timer runs, above 75% the
pool grows) and `backlog` (what lets it grow *ahead* of saturation — one instance at 100% with 4,000
messages waiting needs more than one instance, and utilisation alone cannot say so).

The non-obvious half: **report when idle too.** An instance whose last report is older than 45
seconds counts as *busy*, deliberately, so one the manager has lost sight of is never stopped out
from under work it might be doing. A worker that only reported while it had messages would therefore
never scale back *down*.

`eap:report-load` is in the built-in `application` role, so this works on a fresh application with no
extra grant.

**4. Delete last.** A message deleted before the work is a message lost; one processed and not
deleted comes back and is processed twice. Twice is recoverable and gone is not.

**5. SIGTERM.** The handler sets a lock-free flag and does nothing else — a signal handler runs
between two arbitrary instructions and almost nothing is safe there, certainly not logging or an
HTTP call. The flag is checked inside the batch as well as around it, so a stop with nine messages
left does not wait for all nine.

## Static or shared?

`CMakeLists.txt` links `euclid::cdk`, the shared library. `euclid::cdk-static` is worth considering
for something euclid deploys: the artifact is one file copied out of a bucket onto whichever host
runs it, with no package manager on the other side and nothing to install a matching
`libeuclid-cdk.so`. A static binary cannot arrive and fail to start for want of a library. The cost
is size, and having to redeploy to pick up an SDK fix.
