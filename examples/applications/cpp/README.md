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

The uploaded file stays where it is in `transfer-server`. A file delivered under a key that already
exists replaces it.

It is the only example here that is **compiled**, so it is also the one that shows the `BINARY`
runtime: there's no interpreter, and the artifact is the executable.

## How it works

1. **On startup** it looks up both buckets and creates its queue, `file-delivery-queue`, if it
   doesn't exist yet. Then it subscribes that queue to the `transfer-server` bucket's
   object-created and object-updated events. A restart doesn't subscribe a second time, because it
   checks `ListSubscriptions` first.
2. **Each upload** puts a notification in the queue. The application receives it, downloads the
   file into a temporary directory of its own, and uploads it to `file-delivery`. For a `.zip`
   (matched case-insensitively), it unpacks the archive into that directory with libarchive and
   uploads each regular file in it. The temporary directory is deleted afterwards, whether the
   delivery worked or not.
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

Both buckets have to exist before it starts. Creating buckets is left to the installation, and
`setup-infrastructure.sh` creates both.

## Build

Standalone, the way your own application's repository would be. It needs
[euclid-cdk](../../../../euclid-cdk) and **libarchive** installed, and Boost and OpenSSL findable.

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
which uses musl rather than glibc. A binary built on Debian or Ubuntu won't start there. Build it in
an Alpine container, or link it statically (see below). The image already includes libarchive, so a
shared build only needs euclid-cdk from you.

## Deploy

```bash
euclid-cli esm upload-file --bucket apps --key file-delivery --file build/file-delivery

euclid-cli eap create-application --application-id file-delivery --runtime BINARY \
    --bucket apps --artifact file-delivery --version 1.0.0 \
    --min-instances 1 --max-instances 4 \
    --buckets transfer-server,file-delivery --queues file-delivery-queue

euclid-cli eap start-application --application-id file-delivery
```

`--buckets` and `--queues` narrow the application's own principal to the two buckets and its queue.
Without them it may use every bucket and queue in its account.

Then upload something through FTP or SFTP into `incoming` and watch it arrive:

```bash
euclid-cli esm list-objects --bucket file-delivery
```

## Grants

`create-application` gives the application a technical principal of its own, `app-file-delivery`,
and puts it in the built-in `application` role. That role covers the queue, the subscription, load
reporting, and single-request transfers (`get-object`, `put-object`). It doesn't cover these two
things:

- **Looking up a bucket by name** (`esm:get-bucket-ern`), which the application does once at
  startup.
- **Files larger than 5 MiB.** The SDK moves those in parts, using `create-upload`, `upload-part`
  and `complete-upload` (and the matching download actions). Without these, small files are
  delivered and large ones fail.

One extra role, granted only on the two buckets:

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

Strictly, the application only needs the download actions on `transfer-server` and the upload
actions on `file-delivery`. Splitting them into two roles is the tighter version of the same grant.

## Things worth knowing

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

## Static or shared?

`CMakeLists.txt` links `euclid::cdk` and libarchive as shared libraries. For something euclid
deploys, `euclid::cdk-static` and a static libarchive are worth considering. The artifact is one
file copied onto whichever worker runs it, with no package manager there to install a missing
library. The cost is size, and having to redeploy to pick up a library fix.
