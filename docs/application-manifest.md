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

The directory ships inside the artifact, so EAP can read it at deploy without access to the
repository.

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
sends to the queue, publishes to the topic — and euclid makes the object and records the owner. Any
field other than `name` is passed through to the module that makes it: a visibility timeout is EQS's
business and an encryption key is ESM's, and a manifest parser that knew about every setting either
of them has would need changing whenever either gained one.

**`uses`** is what it consumes from somebody else. Nothing is created. The object must already
exist, and a deploy that cannot find it fails naming the application that should have created it —
which is why `owner` is worth writing even though it is optional and never checked against the real
owner. `access` is required: an application that does not say how it reaches somebody else's object
has not said what it needs, and a default would grant the wrong thing to whichever half of the guess
was wrong. One way or several: a `@BucketListener` attaches to a bucket's events and then fetches
what each event names, which is `["subscribe", "read"]`. Order does not matter, and declaring the
same object twice is still refused — two declarations of one object are two answers.

| `access` | means |
| --- | --- |
| `read` | list and get. Objects out of a bucket, nothing written back. |
| `write` | read, plus put and delete. What a producer needs on somebody else's bucket. |
| `consume` | receive and delete messages from a queue somebody else created. |
| `produce` | send messages to a queue, or publish to a topic. |
| `subscribe` | attach a queue of one's own to a topic, or to a bucket's events. |

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

The producer owns the object. If two applications write to one bucket, the first to declare it owns
it and the second's identical declaration is a no-op; a declaration that differs is refused rather
than silently losing to whichever deployed first.

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

## It asks; euclid decides

An artifact is built by whoever built it, which for a delivery pipeline is frequently not the
operator. So a manifest is a request. EAP applies it within what the `application` role already
allows, narrowed to the objects the manifest names, and nothing in a file can widen a principal
beyond that ceiling. Anything outside it is an operator's decision at deploy time rather than a line
that arrived with a jar.

## When it is wrong

Every problem in the directory is reported at once, each naming its file — somebody fixing a
manifest wants the list, not one error per deploy. Nothing is half-applied out of a directory that
did not parse.

What is refused rather than ignored:

- an unknown section, including a misspelt one. A skipped `"queue"` is a deployment that comes up
  having created nothing, with a manifest that looks right and a log that says nothing;
- a `version` other than 1. Half-understanding a later format would apply the sections this version
  knows and silently not the ones it does not;
- the same object declared in two files, naming both;
- an object that is created here and used here — an application owns an object or consumes one;
- the same used object with two different `access` levels, which is two answers to one question;
- a secret under `creates`, or one used as anything but `read`.

A `euclid/` directory that does not exist is not an error. Most applications declare nothing, and a
deploy that started refusing them would be this breaking every existing deployment on arrival.

## Applying one by hand

```
euclid-cli eap apply --application-id parsing --directory ./euclid --dry-run
```

Everything is printed before anything is done, and `--dry-run` stops there:

```
Application 'parsing' in namespace development:

  create  queue parsing-in  (queues.json)
  ok      bucket parsing-work - already there
  use     bucket transfer-server as read
  use     secret parsing-username as read
  MISSING topic artikel-updates - used but does not exist; created by 'transformation'
  MISSING secret parsing-password - used but does not exist
```

A declared secret is checked like anything else used, and a missing one is `MISSING` rather than
created — but note which half of the pair that is. `apply` looks a secret up through `ess
list-secrets`, which carries the name and the ERN and nothing sensitive; `ess get-secret`, the only
action that takes a name directly, decrypts and returns the value, which is not something a tool
checking whether a declaration resolves should ever ask for.

`adopt` is the one verb worth reading twice. An object that already exists and carries no owner tag
predates the manifest, or was made by hand; applying claims it, because an object an application
owns and cannot manage is a manifest that lies. One already owned by a different application is a
`CLASH` and stops the run.

Exit codes are the three [`Exists`](../cli/include/euclid/cli/ExistsCheck.h) gives, for the same
reason: `0` the installation matches the manifest, `1` it does not and could not be made to, `2` the
question could not be asked at all — an expired session, an unreachable gateway. Nothing is changed
on `1` or `2`.

Running it twice changes nothing the second time.

A byte order mark is skipped rather than refused. Every Windows editor writes one by default,
including PowerShell's own `Set-Content -Encoding utf8`, and "syntax error at line 1" about an
invisible character is the least actionable message there is.

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
them is `apply` — except for secrets, which `apply` only checks. A deployment naming a secret nobody
has written is refused with `Not found: secret 'parsing-password'`, which is a better place to find
out than the first datasource the application tries to build.

A manifest declaring nothing leaves the deployment's own lists alone rather than sending four empty
ones — EAP reads "no resources named" as "every resource in the account", which is the opposite of
what a manifest is for.
