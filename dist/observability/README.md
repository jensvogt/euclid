# Shipping euclid's logs to OpenSearch

Two indexes: **`euclid-logs`** for what euclid itself wrote, and **`euclid-app-logs`** for what the
applications EAP runs wrote. One installation goes into one cluster; which environment a record
came from is the `namespace` field rather than a cluster of its own.

Nothing here is installed by euclid's packages. Fluent Bit's configuration belongs in Fluent Bit's
own directory, and the OpenSearch objects belong to the cluster — these are the files to copy
there, kept with euclid so they stay honest about the fields it actually emits.

## What euclid has to be told first

```json
"logging": {
  "file-active": true,
  "level": "info",
  "file-format": "json",
  "console-format": "text",
  "console-level": "warning",
  "dir": "/usr/local/euclid/log"
}
```

`file-active` is off by default and `format` is `"text"`, so at least those two have to change.

The console and the file are then set separately, which is the arrangement worth having once the
file is being shipped: the console stays plain text at `warning` for whoever is watching a
terminal, and the file carries `info` and up as JSON for the collector. `format` still sets both
at once for anyone who wants that; the per-sink keys override it.

One constraint to know: **`level` is the floor.** A sink can only narrow what it allows, never
widen it — `file-level: debug` under `level: warning` still gets warnings, because a record below
the floor is never produced at all.

Nothing else changes: the channels, levels and `eap set-log-level` all work exactly as before, and
they are still the way to turn a talkative application down. That matters more once logs cost
storage than it did when they scrolled past.

## Why files and not journald

euclid writes to stdout, and under systemd that lands in journald, which Fluent Bit can read
directly with no file handling at all. Don't.

**journald rate-limits at 10,000 messages per 30 seconds per service and drops the rest.** The
burst somebody later wants to read about is exactly the one that exceeds it. `RateLimitBurst=0`
in `journald.conf` disables the limit if you would rather go that way, but the file sink plus
`tail` costs nothing and has no such cliff.

## Installing

```sh
# 1. The cluster: templates, retention policy, write aliases.
OPENSEARCH_PASSWORD=... ./opensearch/setup.sh -u https://opensearch:9200

# 2. The collector.
cp fluent-bit.conf parsers.conf /etc/fluent-bit/
install -d -o fluent-bit -g fluent-bit /var/lib/fluent-bit/storage

# 3. Where it ships to, which does not belong in a file the repository carries.
install -D fluent-bit.service.d-euclid.conf /etc/systemd/system/fluent-bit.service.d/euclid.conf
systemctl daemon-reload
systemctl restart fluent-bit
```

On its first run the collector starts at the *end* of the log file, so whatever was written
before it existed is not shipped. That is almost always what is wanted; `Read_from_Head On` in
the `[INPUT]` section changes it. After that the `DB` file is what makes a restart resume where
it left off rather than ship the file again from the top.

Check it is ingesting:

```sh
curl -s localhost:2020/api/v1/metrics | jq '.output'
curl -s -u "$OPENSEARCH_USER:$OPENSEARCH_PASSWORD" https://opensearch:9200/_cat/indices/euclid-*
```

## How the split is made

Every euclid log record carries a **channel**, which is what already tells euclid's own records
from the output of a process it started:

| channel | source | index |
|---|---|---|
| `esm`, `eqs`, `eag`, … | the module itself | `euclid-logs` |
| `module.<name>` | a module's stdout, as the manager read it | `euclid-logs` |
| **`app.<applicationId>`** | an application's own output | **`euclid-app-logs`** |

One `rewrite_tag` rule on `^app\.` does it, and it is the same vocabulary `euclid.logging.channels`
and `eap set-log-level` use — so a dashboard filter and the knob that quietens it name the same
thing.

## What a record looks like

euclid's own, from `Core::LogStream`'s JSON formatter:

```json
{"@timestamp":"2026-09-20T14:30:43.610Z","log.level":"error","message":"Upload part 3 refused",
 "channel":"eag","service.name":"eag","host.name":"euclid-1","process.pid":2121816,
 "region":"eu-central-1","log.origin.function":"ProxyServer::sendUploadPart","log.origin.line":663}
```

An application's, spliced rather than wrapped — its own fields stay queryable, which is the whole
reason for asking applications to log JSON:

```json
{"@timestamp":"2026-09-20T12:00:00.000Z","log.level":"WARN","message":"slow batch",
 "logger_name":"de.libri.Parser","thread_name":"main","batchSize":4096,
 "channel":"app.parser","application.id":"parser","namespace":"development",
 "service.name":"parser","host.name":"euclid-1","process.pid":2131002}
```

Three things about that second one:

- **Logback calls it `level`; this renames it to `log.level`,** so one query finds every record
  whatever wrote it.
- **euclid's fields are written last and win.** An application cannot claim to be in `production`
  by logging a `namespace` field.
- **The severity is the application's own.** Before, every stdout line was `info` and every stderr
  line `error`, so turning an application down to `error` silenced its errors too.

A program that logs plain text still works: its line becomes `message` and euclid supplies
everything around it.

## Where a stack trace goes

There is no multiline reassembly here, and adding one would not help. euclid has already split
the trace: the manager reads a child's pipe line by line (`Controller::drainPipe`) and emits one
record per line, so by the time it reaches the log file a JVM's crash dump is N separate records
each holding one line. Nothing is left for a collector to join.

The fix is at the other end. An application that logs JSON puts its trace in a `stack_trace`
field, which arrives whole as one record - which is the whole reason for asking applications to
log JSON rather than text.

## What is dropped

A line that is not one of euclid's JSON records never reaches OpenSearch. `channel` is the test,
because every record the formatter writes has one and nothing else does. That covers a euclid
older than the JSON formatter, `format` left at `"text"` by mistake, and half a line read while
the file was rotating - none of which is worth indexing, and all of which would otherwise fill the
index during exactly the period when nobody is yet watching it.

## Two things to watch

**`Replace_Dots` must stay `Off`.** euclid emits ECS names as flat dotted keys — `log.level`,
`host.name` — which OpenSearch expands into the nested objects the templates describe. Fluent
Bit's default rewrites the dots to underscores, and every mapping would then describe a field
that no longer exists.

**An application logging a scalar field named `log`** collides with the `log.level` object and
OpenSearch rejects that document — only that one; the rest of the bulk is indexed and Fluent Bit
records the rejection. If it happens, rename the field in the application or add an explicit
mapping.

## Retention

`euclid-logs-ism-policy.json` rolls each index over at 20 GB or one day and deletes it after 30
days. Both indexes use the same policy, which it attaches to itself through `ism_template` — so
it has to be in place *before* the first index exists, which is why `setup.sh` does it in that
order.

## Files

| file | goes to |
|---|---|
| `fluent-bit.conf` | `/etc/fluent-bit/fluent-bit.conf` |
| `parsers.conf` | `/etc/fluent-bit/parsers.conf`, or its `[PARSER]` appended to the existing one |
| `fluent-bit.service.d-euclid.conf` | `/etc/systemd/system/fluent-bit.service.d/euclid.conf` |
| `opensearch/euclid-logs-template.json` | `PUT _index_template/euclid-logs` |
| `opensearch/euclid-app-logs-template.json` | `PUT _index_template/euclid-app-logs` |
| `opensearch/euclid-logs-ism-policy.json` | `PUT _plugins/_ism/policies/euclid-logs` |
| `opensearch/setup.sh` | run once, does the three above |
