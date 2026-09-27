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
    "queues":  [ { "name": "parsing-in", "visibilityTimeout": 300 } ],
    "buckets": [ { "name": "parsing-work" } ]
  },
  "uses": {
    "topics":  [ { "name": "artikel-updates", "access": "subscribe", "owner": "transformation" } ],
    "buckets": [ { "name": "transfer-server", "access": "read" } ]
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
was wrong.

| `access` | means |
| --- | --- |
| `read` | list and get. Objects out of a bucket, nothing written back. |
| `write` | read, plus put and delete. What a producer needs on somebody else's bucket. |
| `consume` | receive and delete messages from a queue somebody else created. |
| `produce` | send messages to a queue, or publish to a topic. |
| `subscribe` | attach a queue of one's own to a topic, or to a bucket's events. |

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
- the same used object with two different `access` levels, which is two answers to one question.

A `euclid/` directory that does not exist is not an error. Most applications declare nothing, and a
deploy that started refusing them would be this breaking every existing deployment on arrival.
