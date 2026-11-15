# Python applications, using euclid-pdk

Two applications:

| File | What it shows |
|---|---|
| [`xml_splitter.py`](xml_splitter.py) | a queue consumer: splits CD catalogues into one message per CD |
| [`euclid_app.py`](euclid_app.py) | the optional server side: answering requests on `EUCLID_SOCKET` (see [the applications README](../README.md)) |

`rfc9421_reference.py` is the request-signing reference implementation the applications README
describes.

## xml-splitter

Reads the queue `file-delivery-xml`, where the [file-delivery](../cpp) application announces every
XML file it delivers. It splits each CD catalogue into one small XML document per CD, and sends each
one as a message of its own to the queue `splitting-output`:

```xml
<CATALOG>                                        <?xml version="1.0" encoding="UTF-8"?>
  <CD><TITLE>Blue Hour</TITLE>...</CD>     ->    <CD>
  <CD><TITLE>Salt Roads</TITLE>...</CD>            <TITLE>Blue Hour</TITLE>
</CATALOG>                                         ...
                                                 </CD>
```

Each message carries attributes, so a reader can act on a piece without parsing it:

| Attribute | Example |
|---|---|
| `title` | `Blue Hour` |
| `index` | `1`, the CD's position in the catalogue |
| `fileName` | `cd-catalog-0001-blue-hour.xml`, a name for a reader that writes the pieces out as files |
| `sourceBucket`, `sourceKey` | `file-delivery`, `ftpuser1/cd-catalog.xml` |

[`sample/cd-catalog.xml`](sample/cd-catalog.xml) is a catalogue to try it with. Upload it into an
FTP user's `incoming` directory, and three messages arrive on `splitting-output`.

### How it works

1. **It takes up to four announcements at a time** from `file-delivery-xml`, waiting up to 20
   seconds. Each announcement is the JSON `file-delivery` sends, naming the bucket and key of the
   delivered file.
2. **It downloads the file** into a temporary directory, and reads it with `iterparse`. Only the CD
   being read is in memory, never the whole catalogue.
3. **It sends the pieces in batches of 50** (`send-message-batch`).
4. **It deletes the announcement only after every piece was sent.** A failure leaves it in the queue
   to be tried again, up to five times.

**At least once, not exactly once.** A catalogue that fails halfway is split again from the start
on the next attempt, so a reader of `splitting-output` can see a piece twice. `sourceKey` and `index`
together identify a piece.

**Dropped at once, not retried,** because a retry would fail the same way:
- an announcement that isn't valid JSON from `file-delivery`
- a file no longer in its bucket
- XML that isn't well-formed
- a document that declares a DTD

**DTDs are refused on purpose.** The files come from outside users. A DTD is how an XML document
defines entities, and entities are how a document attacks its reader: nested ones that expand a
kilobyte into gigabytes, or external ones that read files from the host. A CD catalogue needs none.

### Configuration

| Variable | Default | What it is |
|---|---|---|
| `INPUT_QUEUE` | `file-delivery-xml` | where the announcements come from |
| `OUTPUT_QUEUE` | `splitting-output` | where the pieces go |
| `SPLIT_ELEMENT` | `CD` | the element a document is split on, matched without its namespace |
| `TITLE_ELEMENT` | `TITLE` | the child that gives a piece its `title` |

Both queues have to exist before it starts. `setup-infrastructure.sh` creates them.

### Build and deploy

The workers' `python3` has no euclid-pdk installed, and nothing should have to be. `build-pyz.sh`
packs `xml_splitter.py` and euclid-pdk into one zipapp, `xml-splitter.pyz`, which any `python3`
runs as is. euclid-pdk has no dependencies of its own, so the archive is pure Python and runs on the
Alpine workers like anywhere else:

```bash
./build-pyz.sh                     # writes ../../docker/apps/xml-splitter.pyz
```

`examples/docker/setup-infrastructure.sh` deploys it with the `PYTHON` runtime. By hand:

```bash
euclid-cli esm upload-file --bucket apps --key xml-splitter.pyz --file ../../docker/apps/xml-splitter.pyz

euclid-cli eap create-application --application-id xml-splitter --runtime PYTHON \
    --bucket apps --artifact xml-splitter.pyz --version 1.0.0 \
    --buckets file-delivery --queues file-delivery-xml,splitting-output

euclid-cli eap start-application --application-id xml-splitter
```

### Grants

`create-application` puts the application's principal, `app-xml-splitter`, in the built-in
`application` role, limited to the bucket and queues named. That role covers receiving, deleting,
and downloading in one request. It doesn't cover two things this application needs:

- **`eqs:send-message-batch`.** Sending a batch is an action of its own, not a kind of
  `send-message`. Without it, every catalogue fails and is retried until it's given up on.
- **Downloading in parts** (`esm:create-download`, `esm:download-part`, `esm:complete-download`), for
  an XML file over 5 MiB.

`setup-infrastructure.sh` grants both through a role of its own, on `splitting-output` and the
`file-delivery` bucket only:

```bash
euclid-cli eam create-role --name xml-splitter \
    --permission eqs:send-message-batch \
    --permission esm:create-download --permission esm:download-part --permission esm:complete-download \
    --description "Send CD pieces in batches, and download large XML files"

euclid-cli eam grant-role --role xml-splitter \
    --principal ern:eam:eu-central-1:000000000000:user:app-xml-splitter \
    --namespace development \
    --resource 'ern:eqs:eu-central-1:000000000000:development:queue:splitting-output' \
               'ern:esm:eu-central-1:000000000000:development:bucket:file-delivery*'
```
