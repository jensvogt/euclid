# The application manifest

An application says what it needs from an installation in a `euclid/` directory shipped with it.
euclid reads that at deploy: it creates the objects the application owns, grants the application's
technical principal exactly those and the ones it says it consumes, and removes what the manifest
no longer lists.

Without one, a deployment says nothing, and the only safe reading of silence is the one EAP makes
today — the `application` role across every resource in the namespace, `resources = ["*"]`. The
queues, topics and buckets are then made by hand, or by a script somebody keeps, or by the
application on first run, and the roles that narrow access to them are written per application by an
operator. None of that lives anywhere near the code it describes.

## Where it goes

```
parsing/
  src/...
  euclid/
    queues.json
    buckets.json
    access.json
```

Every `*.json` directly in `euclid/` is read and merged, sorted by file name so the same directory
always merges the same way. Subdirectories are ignored. One file would do; several keep a queue
added here and a bucket added there out of each other's way in a repository with twenty services.

`euclid-cli eap apply` merges the files into one declaration and stores it beside the artifact, in the
application's own bucket, as `<application-id>.euclid.json`. EAP applies that stored declaration on
`apply` and again on every create, update and redeploy, so the folder is the statement of record
every time the application is deployed.

## What it says

```json
{
  "version": 1,
  "creates": {
    "queues":  [ { "name": "parsing-in", "visibility": 300 } ],
    "buckets": [ { "name": "parsing-work" } ]
  },
  "uses": {
    "topics":  [ { "name": "artikel-updates", "access": "subscribe", "owner": "transformation" } ],
    "buckets": [ { "name": "transfer-server", "access": ["subscribe", "read"] } ],
    "secrets": [ { "name": "parsing-username", "access": "read" },
                 { "name": "parsing-password", "access": "read" } ]
  }
}
```

**`creates`** is what this application owns. It is the producer — the one that writes to the bucket,
sends to the queue, publishes to the topic — and euclid makes the object and records the owner.
Every field other than `name` is a setting of the object:

| kind | setting | value | default |
| --- | --- | --- | --- |
| queues | `visibility` | seconds a received message stays hidden, 1–43200 | 30 |
| | `maxRetries` | receives before a message goes to the dead letter queue, 1–1000 | 3 |
| | `maxMessageLength` | bytes, 1–268435456 | 1 MB |
| | `delay` | seconds before a sent message becomes visible, 0–43200 | 0 |
| | `retention` | seconds a message may wait; 0 for the installation's default | 0 |
| | `priority` | `LOW`, `MEDIUM` or `HIGH`, for messages sent without one | `MEDIUM` |
| | `deadLetterQueue` | the name of another queue, declared here or already there | none |
| topics | `maxMessageLength` | bytes, 1–268435456 | 1 MB |
| | `retention` | seconds a message is kept; 0 for the installation's default | 0 |
| buckets | — | none yet | |
| tables | `partitionKey` | the attribute items are keyed on — **required** | |
| | `partitionKeyType` | `string`, `number` or `binary` | `string` |
| | `sortKey` | a second key attribute, for range queries | none |
| | `sortKeyType` | `string`, `number` or `binary` | `string` |
| routes | `path` | the path prefix published, starting with `/` — **required** | |
| | `methods` | HTTP methods it answers for | all |
| | `authentication` | `NONE`, `EUCLID` or `BASIC` | `NONE` |
| | `timeout` | seconds the application has to answer, 0–3600; 0 is the gateway's | 0 |

A setting is applied when the object is made, and again on every apply to the object as it is: an
existing queue whose file says `"visibility": 900` is brought to 900. A setting the file leaves out
is left alone, so removing a line never resets a value somebody changed by hand. A key a kind does
not have is refused, naming the ones it does — `"visibilty"` is a typo, not a queue at the default.

A **table's key** is fixed for its life: once items are keyed on it there is no changing it. A
file whose key differs from the table that exists is refused, naming both, rather than applied.

A **route** publishes a path on the API gateway for the application that declares it — always that
one; a route to another application, a euclid module or a bucket is made with `eag create-route`.
Whose a route is, is where it sends: one already sending somewhere else is refused, as is a path
another route answers for the same methods. A route is reached over HTTP, so nothing is granted for
it, and it holds nothing, so one no longer declared is always removed.

A dead letter queue is a queue like any other, so it is declared like one and named:

```json
"queues": [
  { "name": "parsing-in", "visibility": 300, "maxRetries": 5, "deadLetterQueue": "parsing-in-dlq" },
  { "name": "parsing-in-dlq", "retention": 1209600 }
]
```

**`uses`** is what it consumes from somebody else. Nothing is created. The object must already
exist, and a deploy that cannot find it fails naming the application that should have created it —
which is why `owner` is worth writing even though it is optional and never checked against the real
owner. `access` is required: an application that does not say how it reaches somebody else's object
has not said what it needs, and a default would grant the wrong thing to whichever half of the guess
was wrong. One way or several: a `@BucketListener` attaches to a bucket's events and then fetches
what each event names, which is `["subscribe", "read"]`. Order does not matter, and declaring the
same object twice is still refused — two declarations of one object are two answers.

| `access` | on | means |
| --- | --- | --- |
| `read` | buckets | list and get objects, multipart downloads included. Nothing written back. |
| `write` | buckets | put objects and their attributes, multipart uploads included. Not delete. |
| `delete` | buckets | delete objects, by ERN or by key. |
| `move` | buckets | copy and move objects. A move is checked on both buckets, so declare it on both. |
| `rename` | buckets | rename objects. |
| `subscribe` | buckets, topics | attach a queue of one's own to a bucket's events or to a topic. |
| `consume` | queues | receive, delete and extend the visibility of messages. |
| `produce` | queues, topics | send messages to a queue, or publish to a topic. |
| `control` | queues | stop and start the queue, which a feature flag uses to switch a consumer off. |
| `read` | secrets | read the secret's value. |
| `read` | tables | get, query and scan items, and read the table's key. |
| `write` | tables | put and delete items. `put-item` replaces an item whole, so writing and removing are one decision. |

What an application **creates** it is granted everything on — every level of that kind — on the
object itself, as an `access-<kind>-own` grant. Owning it is the access, literally: before, an
application could reach its own objects only through the `application` role on `"*"`, which never
covered deleting, moving or renaming in its own bucket. `own` cannot be asked for under `uses`.

What every application needs whatever it declares — reporting its load, egress permits, its own
listener delivery queues and their heartbeat, its metrics, its secrets — is the built-in
`application` role EAP grants at deploy, and is not declared at all.

Not every level applies to every kind. A secret has one verb — `read` — so `write` on one is not a
narrower request but somebody expecting an application to be able to rotate the credentials it uses,
and it is refused saying so.

### Secrets

The one kind an application may name and can never own. A secret's value is the point of it, and the
only place a manifest could carry one is the file itself — which ships inside the artifact, so a
`creates` entry for a secret would mean database credentials committed to the application's own
repository. A secret under `creates` is therefore refused, naming where the value goes instead:

```
euclid-cli ess create-secret --name parsing-password --value ... --description 'PIM database'
```

That is an operator's job, done once. The manifest's part is to say which secrets this application
reads, so that its grant covers those and no others. This matters more than it looks: `ess:get-secret`
is resource-checked, but a deployment that names no resources at all is granted `["*"]` — so an
application that does not declare its secrets can read every secret in its namespace, and one that
declares them can read exactly the ones it listed.

Declared by name, like everything else here, and for a reason that is specific to secrets: a name is
what a deployment's configuration can carry and what stays the same between environments, which is
how `parsing-username` means the development credentials in development and the production ones in
production. `ess:get-secret` takes that name and resolves it internally, so unlike the other kinds
there is no ERN-lookup permission to grant alongside.

Names are names, not ERNs. An ERN carries an account and a namespace, and a manifest is applied into
whichever namespace the application is deployed to — accepting one would let a development
deployment reach into production from a file nobody reads at deploy time.

## Ownership, and what it decides

The producer owns the object, and ownership is a tag on it — `euclid:application` — written when a
declaration is applied. An object that exists untagged (made by hand, or before the tag existed) is
adopted by the first application that declares it under `creates`; one tagged for another
application is refused, and the apply stops. Two applications writing one bucket is one owner that
`creates` it and one that `uses` it with `write`.

Ownership decides three things:

- **Creation.** Only the owner's manifest creates the object.
- **Pruning.** An application's own objects are the only ones it can remove. What it prunes is what
  its manifest used to list and no longer does, and an object is only ever removed while it is
  empty — a bucket with objects or a queue with messages is reported as orphaned and left alone.
  (`esm delete-bucket --if-empty`, `eqs delete-queue --if-empty`.)
- **Orphans.** An application that is undeployed leaves its objects behind if they hold anything.
  The report names them, who owned them, and who still holds grants on them — because the readers
  outlive the writer, and they keep working until somebody deletes what they read.

The tag that marks ownership is applied when the manifest is applied, not when the object is
created. This matters for euclid-spring: its listener container creates a delivery queue per run and
takes it down itself, and a tag meaning "created by this application" would put those queues in
range of a prune that is supposed to be about declarations.

## What no application owns: a platform

Some objects belong to no application: the bucket an FTP server writes deliveries into, a topic
several services publish to. They go in a folder of their own — the same format, one level up in the
repository that holds the applications:

```
euclid-pim/
  euclid/                  <- the platform
    buckets.json
  file-copy-service/
    euclid/                <- an application
```

```
euclid-cli eap apply --platform --directory ./euclid --dry-run
euclid-cli eap apply --platform --directory ./euclid
```

`--platform` takes an optional name, `platform` when none is given, so one namespace can carry more
than one. A platform declaration:

- **only creates.** It has no principal, so it reaches nothing: `uses` is refused, and nothing is
  granted for what it owns. A route is refused too, since a route sends to the application declaring it.
- **names no application.** A folder with an `applicationId` is that application's.
- **is kept on a record of its own** in EAP, there being no artifact to store it beside — see
  `eap get-platform` and `eap list-platforms`.
- **tags what it owns `platform:<name>`**, which no application id can be. An application declaring
  the same object under `creates` is refused; applications reach it with `uses`, naming
  `"owner": "platform"` for the reader.

Removal works as for an application: what it no longer declares goes when empty, and is kept and
listed when not.

## It asks; euclid decides

An artifact is built by whoever built it, which for a delivery pipeline is frequently not the
operator. So a manifest is a request. EAP applies it within what the `application` role already
allows, narrowed to the objects the manifest names, and nothing in a file can widen a principal
beyond that ceiling. Anything outside it is an operator's decision at deploy time rather than a line
that arrived with a jar.

## When it is wrong

The first problem in the directory is reported, naming its file. Nothing is applied out of a
directory that did not parse.

What is refused rather than ignored:

- an unknown section, including a misspelt one. A skipped `"queue"` is a deployment that comes up
  having created nothing, with a manifest that looks right and a log that says nothing;
- a setting a kind does not have, or one out of range — `"visibility": 0` included, which makes every
  received message visible again at once;
- any field but `name`, `access` and `owner` on a `uses` entry: a used object is reached, not configured;
- a name that is an ERN, or has whitespace in it;
- a `version` other than 1. Half-understanding a later format would apply the sections this version
  knows and silently not the ones it does not;
- the same object declared twice, in one file or two — including created here and used here, since an
  application owns an object or consumes one;
- a secret under `creates`, or one used as anything but `read`.

A `euclid/` directory that does not exist is not an error. Most applications declare nothing, and a
deploy that started refusing them would be this breaking every existing deployment on arrival.

## Applying one by hand

```
euclid-cli eap apply --application-id parsing --directory ./euclid --dry-run
```

With `--dry-run` EAP is handed the folder through `plan-infrastructure` and answers what it would
do; nothing is stored or changed. That is an action of its own rather than a flag, because an EAP
from before dry runs ignores a field it does not know and would apply for real; an action it does
not know, it refuses. Without it, the folder is stored beside the artifact and applied — by the same code that
applies it on every deploy, so what a dry run shows is what a deploy does.

```
Application 'parsing' in namespace development, dry run:
  create  queue parsing-in-dlq
  update  queue parsing-in: visibility 30 -> 300, maxRetries 3 -> 5, deadLetterQueue none -> ern:...:queue:parsing-in-dlq
  adopt   bucket parsing-work - existed untagged, now recorded as this application's
  keep    queue parsing-old - no longer declared, but it still holds 12 message(s)
  grant   access-bucket-read on bucket transfer-server
  revoke  access-topic-subscribe on topic artikel-updates-v1

--dry-run: nothing was changed.
```

- **adopt** is the one verb worth reading twice: an object that exists untagged is claimed, because
  an object an application owns and cannot manage is a manifest that lies.
- **remove** happens only to what this application owns, no longer declares and that is empty. A
  queue with messages, one with deliveries on their way, a bucket with objects or a topic with
  subscriptions is **kept** and listed with what it still holds, and stays owned, so the next apply
  offers it again. `--no-prune` keeps everything no longer declared.
- **grant** and **revoke** make the application's `access-*` grants exactly what `creates` and `uses`
  ask for.
  Grants made by hand, under any other role, are never touched.

Something used that does not exist stops the apply, naming it. `--json` prints EAP's answer as it
came. Running it twice changes nothing the second time.

A byte order mark is skipped rather than refused. Every Windows editor writes one by default,
including PowerShell's own `Set-Content -Encoding utf8`, and "syntax error at line 1" about an
invisible character is the least actionable message there is.

`eap deploy-infrastructure` is the same command under its older name.

## Deploying with one

```
euclid-cli eap apply             --application-id parsing --directory ./euclid
euclid-cli eap create-application --application-id parsing --runtime JAVA25 \
    --bucket apps --artifact parsing-1.115.0.jar --manifest ./euclid
```

`--manifest` is the half of this that matters for security. Without it a deployment says nothing
about what it needs, so EAP grants its principal the `application` role with `resources = ["*"]` —
every bucket, queue, topic and secret in the namespace. With it, the principal is granted exactly the
objects the manifest names, owned and borrowed alike:

```
principal : ern:eam:eu-central-1:000000000000:user:app-parsing
role      : application
namespaces: ['development']
  resource: ern:esm:...:development:bucket:parsing-work
  resource: ern:esm:...:development:bucket:transfer-server
  resource: ern:eqs:...:development:queue:parsing-in
  resource: ern:ess:...:development:secret:parsing-username
  resource: ern:ess:...:development:secret:parsing-password
```

Both halves count: an application reaches what it owns and what it borrows, and a grant naming only
the first would refuse it the second. `update-application --manifest` replaces the list the same way,
which is how a manifest that gained a queue becomes a principal that may use it.

Apply first, deploy second. The names have to resolve to objects that exist, and the one that creates
them is `apply` — except for secrets, which are never created. A deployment naming a secret nobody
has written is refused with `Not found: secret 'parsing-password'`, which is a better place to find
out than the first datasource the application tries to build.

A manifest declaring nothing leaves the deployment's own lists alone rather than sending four empty
ones — EAP reads "no resources named" as "every resource in the account", which is the opposite of
what a manifest is for.
