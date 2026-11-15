#!/usr/bin/env python3
"""xml-splitter - a euclid application in Python, using euclid-pdk.

Reads the queue file-delivery-xml, on which the file-delivery application (examples/applications/cpp)
announces every XML file it delivers, and splits each CD catalogue it is told about into one small
XML document per CD:

    <CATALOG>                                   <?xml version="1.0" encoding="UTF-8"?>
      <CD><TITLE>Blue Hour</TITLE>...</CD>  ->  <CD><TITLE>Blue Hour</TITLE>...</CD>
      <CD><TITLE>Salt Roads</TITLE>...</CD>     (one message each, on splitting-output)
    </CATALOG>

Each piece is sent as a message of its own to the queue splitting-output, with the CD's title, its
position in the catalogue and the catalogue it came from as attributes - so whatever reads that
queue handles one CD at a time and never sees the catalogue.

Like every application it is a queue consumer: take a message, do the work, delete it. The
announcement is deleted only once every piece has been sent.

Deployed as a zipapp (build-pyz.sh): this file and euclid-pdk in one .pyz the workers' python3 runs
as it is, with nothing installed on them - euclid-pdk has no dependencies of its own.
"""

import json
import os
import re
import signal
import sys
import tempfile
import time
import xml.etree.ElementTree as ElementTree
from euclid import EuclidSession, credentials
from euclid.auth import SigningScheme
from euclid.exceptions import EuclidServiceError
from euclid.http import DEFAULT_CA_CERT_PATH
from euclid.modules.eam import AUTH_BEARER
from pathlib import Path

# How long a reception may wait for work. Twenty seconds is EQS's maximum, and a long poll is how a
# worker stays responsive without spinning.
WAIT_SECONDS = 20

# How many announcements one receive may take. Each is a whole catalogue to download and split.
RECEIVE_BATCH = 4

# How many pieces go to the output queue in one call. Below EQS's default limit of 100
# (euclid.modules.eqs.max-batch-size), with room for an installation that lowered it.
SEND_BATCH = 50

# How often an announcement is tried before it is given up on. A catalogue that fails every time
# would otherwise come back after every visibility timeout for the queue's whole retention period.
MAX_ATTEMPTS = 5

_running = True


def _stop(_signum, _frame):
    # All a signal handler should do: set a flag. The loop finishes the catalogue in hand - a stop
    # halfway through would send its pieces again on the next attempt - and then exits.
    global _running
    _running = False


def log(message: str) -> None:
    # stdout and stderr land in euclid's log under this application's channel.
    print(message, flush=True)


# -- configuration ---------------------------------------------------------------------------------

class Config:
    def __init__(self) -> None:
        # Where announcements come from, and where the pieces go.
        self.input_queue = os.environ.get("INPUT_QUEUE") or "file-delivery-xml"
        self.output_queue = os.environ.get("OUTPUT_QUEUE") or "splitting-output"
        # The element a document is split on, and the one inside it that names a piece. Matched on
        # the local name, so a namespaced <c:CD> is a CD too.
        self.split_element = os.environ.get("SPLIT_ELEMENT") or "CD"
        self.title_element = os.environ.get("TITLE_ELEMENT") or "TITLE"


# -- the session -----------------------------------------------------------------------------------

def open_session() -> EuclidSession:
    """A session from the credentials the manager wrote, with no login - as in euclid_app.py.

    An application has no password: the manager hands it a bearer token in the file
    EUCLID_CREDENTIALS_FILE names, and replaces it before it runs out - hence the token provider,
    which re-reads the file per request rather than keeping the first token.
    """
    stored = credentials.load()
    if stored is None:
        raise SystemExit("no credentials: EUCLID_CREDENTIALS_FILE=%s"
                         % os.environ.get("EUCLID_CREDENTIALS_FILE", "(unset)"))

    base_url = stored.base_url or os.environ.get("EUCLID_ENDPOINT") or os.environ.get("EUCLID_BASE_URL", "")
    if not base_url:
        raise SystemExit("credentials name no server, and neither does the environment")

    session = EuclidSession(
        base_url=base_url,
        token=stored.token,
        user_id=stored.user_id,
        account_id=stored.account_id,
        region=stored.region,
        access_key_id="",
        secret_access_key="",
        is_admin=False,
        namespace=stored.namespace,
        raw={},
        # The manager names the gateway's certificate here when it is self-signed.
        ca_cert_path=os.environ.get("EUCLID_CA_CERT_PATH") or DEFAULT_CA_CERT_PATH,
        verify=True,
        timeout=60.0,
        signing_scheme=SigningScheme.RFC9421,
        auth=AUTH_BEARER,
        cache=False,
    )

    def current_token() -> str:
        latest = credentials.load()
        return latest.token if latest is not None else ""

    session.token_provider = current_token
    return session


# -- splitting -------------------------------------------------------------------------------------

class Unsplittable(Exception):
    """A document that will never split: retrying it only fails again, so it is dropped."""


def local_name(tag: str) -> str:
    # "{http://example.com/cds}CD" -> "CD"
    return tag.rsplit("}", 1)[-1]


def refuse_dtd(path: Path) -> None:
    """Refuses a document that declares a DTD.

    The files come from outside users, through FTP and SFTP. A DTD is how an XML document defines
    entities, and entities are how one goes wrong on purpose: nested ones that expand a kilobyte
    into gigabytes, or external ones that read files off this host. A CD catalogue needs none, so
    any document that declares one is refused before it is parsed. Scanned in chunks, with an
    overlap, so a large file is never read into memory whole.
    """
    marker = b"<!DOCTYPE"
    tail = b""
    with path.open("rb") as file:
        while chunk := file.read(1 << 16):
            if marker in tail + chunk:
                raise Unsplittable("declares a DTD, which is not accepted")
            tail = chunk[-(len(marker) - 1):]


def split(path: Path, config: Config):
    """Yields (title, document) for each split element, as the file is read.

    iterparse rather than parse: the catalogue is never in memory whole, only the CD being read,
    and each is cleared once it has been turned into its piece.
    """
    try:
        for _event, element in ElementTree.iterparse(path, events=("end",)):
            if local_name(element.tag) != config.split_element:
                continue
            title = ""
            for child in element:
                if local_name(child.tag) == config.title_element:
                    title = (child.text or "").strip()
                    break
            # Indented as a document of its own rather than as a part of the catalogue, and without
            # the whitespace after it, which belongs to the catalogue.
            element.tail = None
            ElementTree.indent(element)
            document = '<?xml version="1.0" encoding="UTF-8"?>\n' + ElementTree.tostring(element, encoding="unicode")
            yield title, document
            element.clear()
    except ElementTree.ParseError as ex:
        raise Unsplittable("not well-formed XML: %s" % ex) from ex


def piece_name(source_key: str, index: int, title: str) -> str:
    """A file name for a piece, for whoever writes them out: catalogue name, position, title."""
    stem = Path(source_key).stem
    slug = re.sub(r"[^A-Za-z0-9]+", "-", title).strip("-").lower()[:60] or "untitled"
    return "%s-%04d-%s.xml" % (stem, index, slug)


# -- one announcement ------------------------------------------------------------------------------

def send(eqs, output_ern: str, batch: list) -> None:
    result = eqs.send_message_batch(output_ern, batch)
    if result.failed:
        # Sending the whole catalogue again is the only way to be sure every piece arrives: the
        # announcement stays in the queue, and the next attempt splits it from the start.
        reasons = "; ".join("#%d %s" % (failure.index, failure.reason) for failure in result.failed[:3])
        raise RuntimeError("%d of %d pieces not sent: %s" % (len(result.failed), result.asked, reasons))


def handle(session: EuclidSession, output_ern: str, config: Config, body: str) -> int:
    """Splits the catalogue one announcement names, and sends every piece. Returns how many.

    Raises Unsplittable for a document that will never split, and anything else for a failure worth
    trying again.
    """
    try:
        announcement = json.loads(body)
        key = announcement["key"]
        bucket = announcement.get("bucket", "")
        bucket_ern = announcement.get("bucketErn") or session.esm().get_bucket_ern(bucket)
    except (ValueError, KeyError, TypeError) as ex:
        raise Unsplittable("not an announcement from file-delivery: %s" % ex) from ex

    with tempfile.TemporaryDirectory(prefix="xml-splitter-") as temp:
        path = Path(temp) / "document.xml"
        try:
            session.esm().download_file(bucket_ern, key, path)
        except EuclidServiceError as ex:
            if ex.status == 404:
                raise Unsplittable("%s is no longer in %s" % (key, bucket)) from ex
            raise

        refuse_dtd(path)

        eqs = session.eqs()
        batch = []
        pieces = 0
        for index, (title, document) in enumerate(split(path, config), start=1):
            batch.append({
                "body": document,
                "attributes": {
                    "title": title,
                    "index": index,
                    "fileName": piece_name(key, index, title),
                    "sourceBucket": bucket,
                    "sourceKey": key,
                },
            })
            pieces = index
            if len(batch) == SEND_BATCH:
                send(eqs, output_ern, batch)
                batch = []
        if batch:
            send(eqs, output_ern, batch)

    log("split %s/%s: %d %s piece(s) -> %s" % (bucket, key, pieces, config.split_element, config.output_queue))
    return pieces


# -- main ------------------------------------------------------------------------------------------

def main() -> int:
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    config = Config()
    log("xml-splitter starting, version: %s, from: %s, to: %s, splitting on: <%s>"
        % (os.environ.get("EUCLID_APPLICATION_VERSION", "(unset)"),
           config.input_queue, config.output_queue, config.split_element))

    session = open_session()
    eqs = session.eqs()
    try:
        # Both queues have to exist already: they are the installation's (setup-infrastructure.sh).
        input_ern = eqs.get_queue_ern(config.input_queue)
        output_ern = eqs.get_queue_ern(config.output_queue)
    except Exception as ex:  # noqa: BLE001 - said once, at startup, and fatal
        log("cannot set up: %s" % ex)
        return 1

    handled = 0
    while _running:
        try:
            received = eqs.receive_messages(input_ern, max_messages=RECEIVE_BATCH, wait_time=WAIT_SECONDS)
        except Exception as ex:  # noqa: BLE001 - a queue that cannot be reached is worth a retry
            log("receive failed: %s" % ex)
            time.sleep(1)
            continue

        for message in received.messages:
            try:
                handle(session, output_ern, config, message.body)
                handled += 1
            except Unsplittable as ex:
                # Dropped at once: trying again would fail the same way.
                log("dropped announcement %s: %s" % (message.message_id, ex))
            except Exception as ex:  # noqa: BLE001 - one catalogue must not stop the others
                if message.received_count < MAX_ATTEMPTS:
                    # Not deleted, so it comes back once its visibility timeout runs out.
                    log("split failed, attempt %d of %d, will retry: %s" % (message.received_count, MAX_ATTEMPTS, ex))
                    continue
                log("split failed %d times, giving up on %s: %s" % (message.received_count, message.message_id, ex))

            # Deleted only now: after every piece was sent, or after giving up. An announcement
            # deleted first is a catalogue never split.
            eqs.delete_message(message.receipt_handle)
            if not _running:
                break

    log("xml-splitter stopping, catalogues split: %d" % handled)
    return 0


if __name__ == "__main__":
    sys.exit(main())
