# file-delivery — a C++ application, using euclid-cdk

Takes what FTP and SFTP clients upload and delivers it to another bucket. ZIP archives are unpacked
first, and each file in them is delivered separately.

It works with the transfer servers that [`examples/docker/setup-infrastructure.sh`](../../docker/setup-infrastructure.sh)
sets up. Those servers store every upload in the `transfer-server` bucket, under the home directory
of the user who uploaded it. This application only acts on files in a home's `incoming` directory:

| Uploaded to `transfer-server` | Delivered to `file-delivery` |
|---|---|
| `ftpuser1/incoming/report.csv` | `ftpuser1/report.csv` |
| `ftpuser1/incoming/2026/report.csv` | `ftpuser1/2026/report.csv` |
| `ftpuser1/incoming/batch.zip`, holding `a.csv` and `b/c.csv` | `ftpuser1/a.csv`, `ftpuser1/b/c.csv` (the archive itself is not delivered) |
| `ftpuser1/outgoing/anything` | nothing: not in an `incoming` directory |

Once everything in an upload has been delivered, the upload is **deleted from `transfer-server`**.
For a ZIP, that means after the last file in it. A file delivered under a key that already exists
replaces it. Files outside `incoming`, and uploads that fail five times, stay where they are.

An upload is deleted only if it's still the one that was delivered. If a client uploads the same
name again while the first upload is being delivered, the checksums no longer match, and the new
upload is left for its own notification, which delivers it and then deletes it. Without this check,
the new upload would be deleted without ever being delivered.

Each delivered file is then **classified by its content** and announced on a queue for its kind:

| libmagic says | Announced on |
|---|---|
| `application/pdf` | `file-delivery-pdf` |
| `image/*` (an SVG too) | `file-delivery-image` |
| `text/xml`, `application/xml`, `*+xml` | `file-delivery-xml` |
| `application/json`, `*+json` | `file-delivery-json` |
| anything else, or not classifiable | `file-delivery-misc` |

The content decides, not the name: `report.txt` holding a PDF goes to `file-delivery-pdf`. XML is
recognised by its `<?xml ...?>` declaration; without one, libmagic reports `text/plain` and the
file goes to `file-delivery-misc`. A file whose kind's queue doesn't exist goes there too. So does
every file if libmagic's database can't be loaded. `category` in the notification names the queue
it was sent to, and `mimeType` still says what the file is. The notification is a JSON message:

```json
{"category": "pdf", "mimeType": "application/pdf",
 "bucket": "file-delivery", "bucketErn": "ern:esm:...", "key": "ftpuser1/report.pdf",
 "ern": "ern:esm:...", "size": 48213, "md5Sum": "...",
 "source": {"bucket": "transfer-server", "key": "ftpuser1/incoming/batch.zip"}}
```

`source` is the uploaded file, so for a file from a ZIP it is the archive.

It is the only example here that is **compiled**, so it is also the one that shows the `BINARY`
runtime: there's no interpreter, and the artifact is the executable.

## Protocol

Every step is also published to the ENS topic **`protocol`**, one message per step. Subscribe a
queue to it (`ens subscribe`) to follow each upload from arrival to removal without reading the
application's log:

| Step | When | Details |
|---|---|---|
| `started` / `stopped` | an instance starts or stops | `version` / `handled` |
| `picked-up` | an upload in `incoming` is taken on | `size`, `md5Sum`, `owner` |
| `ignored` | an upload outside `incoming` | `reason` |
| `downloaded` | the upload is on the instance's disk | `size` |
| `gone` | the upload is no longer there (a repeat, or deleted by the client) | `reason` |
| `unpacked` | a ZIP is unpacked | `files` |
| `delivered` | one file is in `file-delivery` | `bucket`, `key`, `size`, `md5Sum`, `mimeType` |
| `announced` / `not-announced` | its notification is sent, or there is no queue for it | `key`, `category`, `queue`, `message` |
| `removed` / `not-removed` | the upload is deleted from `transfer-server`, or left there | `reason` |
| `completed` | the upload notification is done with | |
| `failed` / `given-up` | an attempt failed and will be retried, or the fifth one failed | `error`, `retry` |

Each message also carries `timestamp`, `application`, `instance`, and, except for `started` and
`stopped`, the upload notification's id (`notification`), the `attempt`, and the upload as `source`.
`notification` ties the steps of one upload together, across retries and instances:

```json
{"timestamp": "2026-11-15T14:02:11.482Z", "application": "file-delivery", "instance": "file-delivery-1",
 "step": "delivered", "notification": "6f1c...", "attempt": 1,
 "source": {"bucket": "transfer-server", "key": "ftpuser1/incoming/report.pdf"},
 "bucket": "file-delivery", "key": "ftpuser1/report.pdf", "size": 48213, "md5Sum": "...", "mimeType": "application/pdf"}
```

The protocol is a record, not part of the work. A message that can't be published is logged once
and dropped, and never fails a delivery. If the topic doesn't exist at startup, nothing is published.

## How it works

1. **On startup** it looks up both buckets and creates its queue, `file-delivery-queue`, if it
   doesn't exist yet. Then it subscribes that queue to the `transfer-server` bucket's
   object-created and object-updated events. A restart doesn't subscribe a second time, because it
   checks `ListSubscriptions` first.
2. **Each upload** puts a notification in the queue. The application receives it, downloads the
   file into a temporary directory of its own, and uploads it to `file-delivery`. For a `.zip`
   (matched case-insensitively), it unpacks the archive into that directory with libarchive and
   uploads each regular file in it. Each file is classified with libmagic before it is uploaded,
   and once it is uploaded, a notification is sent to the queue of its kind. When everything is
   delivered, the upload is deleted from `transfer-server`. The temporary directory is deleted
   afterwards, whether the delivery worked or not.
3. **The notification is deleted only after the delivery succeeded.** If a delivery fails, the
   notification comes back once its visibility timeout runs out and is tried again. After five
   failed attempts it is logged and dropped, so a file that can never be delivered (such as a
   corrupt or password-protected ZIP) isn't retried forever.

**Unpacking ZIP files from outside users safely:**
- An entry named with `..` (for example `../../etc/passwd`) is skipped and logged. Unpacking it
  where it points would write outside the temporary directory, the "zip slip" vulnerability. A
  leading `/` is dropped, so absolute names stay inside the directory.
- Links, devices and directories inside the archive are not delivered. Only regular files are.
- One archive may unpack to at most `MAX_EXTRACTED_BYTES` (4 GiB by default). The limit counts
  bytes actually written, not the sizes the archive claims, so a small "ZIP bomb" that expands to
  terabytes is stopped before it fills the worker's disk.

## Configuration

Set through `--environment` on `create-application`. Everything has a default that matches
`setup-infrastructure.sh`:

| Variable | Default | What it is |
|---|---|---|
| `SOURCE_BUCKET` | `transfer-server` | the bucket the transfer servers write to |
| `TARGET_BUCKET` | `file-delivery` | where files are delivered |
| `DELIVERY_QUEUE` | `<application id>-queue` | the queue the source bucket's events arrive in |
| `INCOMING_DIR` | `incoming` | the directory in each home whose files are delivered |
| `MAX_EXTRACTED_BYTES` | `4294967296` | the most one ZIP may unpack to |
| `CLASSIFIED_QUEUE_PREFIX` | `<application id>-` | the start of the queue names files are announced on: `<prefix>pdf`, `<prefix>image`, `<prefix>xml`, `<prefix>json`, `<prefix>misc` |
| `PROTOCOL_TOPIC` | `protocol` | the ENS topic every step is published to; set but empty publishes nothing |
| `MAGIC` | libmagic's compiled-in path | where libmagic's database (`magic.mgc`) is, on a host that keeps it somewhere else |

Both buckets have to exist before it starts. Creating buckets is left to the installation, and
`setup-infrastructure.sh` creates both. The same goes for the five classification queues and the
`protocol` topic. A
missing one is logged at startup, and files of that kind go to `file-delivery-misc`. If
`file-delivery-misc` is missing too, those files are delivered without being announced: delivery
never depends on classification.

## Build

Standalone, the way your own application's repository would be. It is linked **statically** (see
[Static linking](#static-linking)), so it needs the static archives, not just the shared libraries:
[euclid-cdk](../../../../euclid-cdk) (its `libeuclid-cdk.a`), Boost, OpenSSL, **libmagic**, and
**libarchive** with the libraries it was built against. On Debian/Ubuntu:

```bash
sudo apt install libarchive-dev libmagic-dev libboost-json-dev libboost-url-dev libssl-dev \
    libjitterentropy3-dev nettle-dev libacl1-dev libzstd-dev liblz4-dev libbz2-dev zlib1g-dev liblzma-dev libxml2-dev
```

`libjitterentropy3-dev` is easy to miss: Ubuntu's static OpenSSL needs it, and without it the link
fails with `cannot find -l:libjitterentropy.a`.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 64
```

If something is in a place cmake doesn't search, add `-DCMAKE_PREFIX_PATH=/where/it/is`. Separate
several prefixes with `;`:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="/usr/local/euclid-cdk-0.12.0-linux-x64;/usr/local"
```

**Build it for the worker it will run on.** The `euclid-wrk` image of `examples/docker` is Alpine,
which uses musl rather than glibc. A binary built on Debian or Ubuntu won't start there, even linked
statically as below, because it still uses the host's C library. For Alpine, build it in an Alpine
container with `-DFILE_DELIVERY_FULLY_STATIC=ON`. `build-alpine.sh` does exactly that with nothing
but docker: it builds euclid-cdk (from a checkout next to this repository, or `CDK_SRC`) and then
this application, and leaves the binary in `examples/docker/apps/`, where
`setup-infrastructure.sh` deploys it from:

```bash
./build-alpine.sh
```

The result has no dependencies at all and runs on any x86-64 Linux, Alpine or not.

## Deploy

```bash
euclid-cli esm upload-file --bucket apps --key file-delivery --file build/file-delivery

euclid-cli eap create-application --application-id file-delivery --runtime BINARY \
    --bucket apps --artifact file-delivery --version 1.0.0 \
    --min-instances 1 --max-instances 4 \
    --buckets transfer-server,file-delivery \
    --queues file-delivery-queue,file-delivery-pdf,file-delivery-image,file-delivery-xml,file-delivery-json,file-delivery-misc

euclid-cli eap start-application --application-id file-delivery
```

`--buckets` and `--queues` narrow the application's own principal to the two buckets and its six
queues: the one it reads and the five it announces on.
Without them it may use every bucket and queue in its account.

Then upload something through FTP or SFTP into `incoming` and watch it arrive:

```bash
euclid-cli esm list-objects --bucket file-delivery
```

## Grants

`create-application` gives the application a technical principal of its own, `app-file-delivery`,
and puts it in the built-in `application` role, limited to the buckets and queues it names. That role covers the queue, the subscription, load
reporting, sending to queues, and single-request transfers (`get-object`, `put-object`). It doesn't
cover these four things:

- **Looking up a bucket by name** (`esm:get-bucket-ern`), which the application does once at
  startup.
- **Files larger than 5 MiB.** The SDK moves those in parts, using `create-upload`, `upload-part`
  and `complete-upload` (and the matching download actions). Without these, small files are
  delivered and large ones fail.
- **Removing delivered uploads** (`esm:list-objects` to check the upload is unchanged,
  `esm:delete-object` to delete it). Without these, files are delivered but stay in
  `transfer-server`, and every notification is retried until it's given up on.
- **Publishing the protocol** (`ens:get-topic-ern`, `ens:publish-message`). The built-in role holds
  both, but only for the buckets and queues named at creation, and `create-application` has no
  option to name a topic. Without them, the work goes on and nothing is published.

Three extra roles. The first is granted on both buckets:

```bash
euclid-cli eam create-role --name file-delivery \
    --permission esm:get-bucket-ern \
    --permission esm:create-download --permission esm:download-part --permission esm:complete-download \
    --permission esm:create-upload --permission esm:upload-part --permission esm:complete-upload \
    --description "Look up and move large files between the transfer and delivery buckets"

euclid-cli eam grant-role --role file-delivery \
    --principal ern:eam:eu-central-1:000000000000:user:app-file-delivery \
    --namespace development \
    --resource 'ern:esm:eu-central-1:000000000000:development:bucket:transfer-server*' \
               'ern:esm:eu-central-1:000000000000:development:bucket:file-delivery*'
```

The second is granted on `transfer-server` only, so the application can never delete a file it has
delivered:

```bash
euclid-cli eam create-role --name file-delivery-cleanup \
    --permission esm:list-objects --permission esm:delete-object \
    --description "Remove delivered uploads from the transfer bucket"

euclid-cli eam grant-role --role file-delivery-cleanup \
    --principal ern:eam:eu-central-1:000000000000:user:app-file-delivery \
    --namespace development \
    --resource 'ern:esm:eu-central-1:000000000000:development:bucket:transfer-server*'
```

The third is granted on the `protocol` topic only:

```bash
euclid-cli eam create-role --name file-delivery-protocol \
    --permission ens:get-topic-ern --permission ens:publish-message \
    --description "Publish the file-delivery protocol"

euclid-cli eam grant-role --role file-delivery-protocol \
    --principal ern:eam:eu-central-1:000000000000:user:app-file-delivery \
    --namespace development \
    --resource 'ern:ens:eu-central-1:000000000000:development:topic:protocol'
```

Strictly, the application only needs the download actions on `transfer-server` and the upload
actions on `file-delivery`. Splitting the first role in two is the tighter version of the same
grant. `setup-infrastructure.sh` creates and grants all three roles.

## Things worth knowing

**A repeated notification finds nothing.** If deleting the notification fails after the upload was
delivered and removed, the notification comes back, finds the upload gone (404), and is dropped
without an error. The same happens when a client deletes a file before it's picked up.

**Delivered at least once, not exactly once.** A delivery that crashes halfway, or a notification
whose visibility timeout runs out while a large archive is still being unpacked, is handled again.
That is harmless here because delivering writes the same bytes to the same key. If two instances
start at the same moment, both can subscribe the queue, and each file then arrives twice. The
result is the same; the second subscription can be removed with `esm unsubscribe`.

**Large archives and the visibility timeout.** A notification is claimed when it is received, not
when its turn in the batch comes. That's why a batch holds only four. An archive that takes longer
to download, unpack and deliver than the queue's visibility timeout is handed to another instance
while the first is still working on it. Raise the queue's visibility timeout
(`eqs set-queue-visibility`) if your archives are that large.

**Same entry name in one archive.** The later entry wins, as it would with `unzip`.

**Announced at least once, too.** A file is announced only after it was delivered, so whoever reads
the notification finds the file. If the send fails, the upload's notification comes back, and the
file is delivered and announced again. A consumer of the classification queues can therefore see
the same file twice, and should treat a repeat as harmless, for example by checking `key` and
`md5Sum`.

**libmagic's database.** A static binary still reads `magic.mgc` at runtime, from the path the build
host's libmagic used. Built with `build-alpine.sh`, that is `/usr/share/misc/magic.mgc`, which the
worker image's `libmagic` package provides. On another host, point `MAGIC` at the database.

## What else it demonstrates

These are unchanged from the earlier queue-consumer version of this example:

**A session with no login.** An application has no password. It reads a bearer token for its own
technical principal from the file `EUCLID_CREDENTIALS_FILE` names. `AuthMode::Bearer` is used because
a technical principal has no access key at all.

**`SetTokenProvider`.** The manager replaces the token before it expires. A session handed only the
first token gets `401 Bearer token expired` about an hour in. The provider re-reads the file per
request. Transfers send their parts from several threads, so it is called from those too, which is
fine because it only reads the file.

**Load reporting, including when idle.** Without it, the pool never grows. An instance that stops
reporting counts as busy and is never scaled back down.

**Delete last.** A notification deleted before the delivery is a file that's never delivered. One
delivered and not deleted is delivered twice, which can be recovered from.

**SIGTERM.** The handler only sets a flag. The flag is also checked between the files of an archive,
so a stop doesn't wait for a large archive to finish. The notification stays in the queue and the
next instance delivers the archive again from the start.

## Static linking

The artifact is one file copied onto whichever worker runs it, with no package manager there to
install a missing library. So `CMakeLists.txt` links everything in: `euclid::cdk-static`, Boost,
OpenSSL, libarchive and the libraries libarchive itself needs, plus the C++ runtime
(`-static-libstdc++ -static-libgcc`). The cost is size, and having to rebuild and redeploy to pick up
a library fix.

Only the C library stays shared by default. glibc can't be linked statically into a program that
resolves host names, and this one does to reach the gateway. That binary runs on any glibc
distribution at least as new as the one it was built on.

`-DFILE_DELIVERY_FULLY_STATIC=ON` links the C library in too (`-static`), for a binary with no
dependencies at all. Use it on Alpine, where musl supports this. On glibc it links with warnings,
and the result can fail to resolve host names on a worker whose glibc differs.

To link against the shared libraries instead, replace `euclid::cdk-static` and the libarchive
lookup with `euclid::cdk` and `find_package(LibArchive)` / `LibArchive::LibArchive`, and drop the
`CMAKE_FIND_LIBRARY_SUFFIXES`, `Boost_USE_STATIC_LIBS` and `OPENSSL_USE_STATIC_LIBS` lines.
