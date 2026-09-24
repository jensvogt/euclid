#!/usr/bin/env python3
"""A euclid application in Python, using euclid-pdk.

An application is a process euclid starts, restarts, and scales between a minimum and a maximum
instance count. The contract is small:

  1. Survive your own startup - still running a couple of seconds later.
  2. Log to stdout and stderr; the manager drains both into euclid's log.
  3. Exit on SIGTERM.

Optionally, serve HTTP/1.1 on the Unix socket path in EUCLID_SOCKET, dispatching on the
x-euclid-action header the way every euclid module does. That is how a request addressed to this
application through the gateway reaches it, and it is the half no SDK can do for you - euclid-pdk is
a client, and this is the server side. The rest of this file is the client side, and the PDK is all
of it.

Deploy it with:

    euclid-cli esm upload-file --bucket apps --key euclid_app.py --file euclid_app.py
    euclid-cli eap create-application --application-id demo --runtime PYTHON \\
        --bucket apps --artifact euclid_app.py --version 1.0.0
    euclid-cli eap start-application --application-id demo

euclid-pdk has to be importable by the interpreter the manager starts this with - `pip install
euclid-pdk` for that interpreter, or ship a virtualenv and point the application's `--command` at
its python.
"""

import json
import os
import signal
import socketserver
import sys
import threading
from http.server import BaseHTTPRequestHandler

from euclid import EuclidSession, credentials
from euclid.auth import SigningScheme
from euclid.http import DEFAULT_CA_CERT_PATH
from euclid.modules.eam import AUTH_BEARER


def open_session() -> EuclidSession:
    """A session built from the credentials the manager wrote, with no login.

    An application has no password. It is handed a bearer token for its own technical principal in
    the file EUCLID_CREDENTIALS_FILE names, and :func:`credentials.load` reads exactly that file -
    :func:`credentials.path` prefers the environment variable over ``~/.euclid``.

    ``AUTH_BEARER`` because a technical principal has no access key at all: its long-lived secret
    never leaves EAM, so the token is the whole of what this process holds and there is nothing here
    to sign with. ``cache=False`` because ``~/.euclid/credentials`` is the CLI's file and an
    application has no business writing it.
    """
    stored = credentials.load()
    if stored is None:
        raise SystemExit("no credentials: EUCLID_CREDENTIALS_FILE=%s"
                         % os.environ.get("EUCLID_CREDENTIALS_FILE", "(unset)"))

    # The manager writes the server as "endpoint"; EUCLID_ENDPOINT carries the same value, and is
    # the fallback for an older euclid whose file this SDK could not read the server out of.
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
        # What lets this name a queue or a bucket rather than spell out a full ERN.
        namespace=stored.namespace,
        raw={},
        ca_cert_path=DEFAULT_CA_CERT_PATH,
        verify=True,
        timeout=30.0,
        signing_scheme=SigningScheme.RFC9421,
        auth=AUTH_BEARER,
        cache=False,
    )

    # And the part that makes it keep working. The manager replaces the token once less than half
    # its hour is left, so the session is told where to get the current one rather than handed a
    # copy of the first. Without this an application works for an hour and then collects
    # "401 Bearer token expired", a long way from the change that caused it.
    #
    # Re-reading the file per request is what this looks like, and it is cheap: the manager writes
    # it beside and renames, so a reader never sees half a file, and the page has been in cache
    # since the last call.
    def current_token() -> str:
        latest = credentials.load()
        return latest.token if latest is not None else ""

    session.token_provider = current_token
    return session


class Handler(BaseHTTPRequestHandler):
    """Two actions: one that answers from this process, one that calls back into euclid."""

    protocol_version = "HTTP/1.1"

    # Set once in main(), because a handler is constructed per request and a session per request
    # would be a login per request. The PDK builds one client per module per session, so
    # ``session.esm()`` inside a handler costs a dict lookup rather than a connection.
    session: EuclidSession = None  # type: ignore[assignment]

    def log_message(self, fmt, *args):
        # Straight to stdout, which the manager drains into euclid's own log under this
        # application's own channel.
        sys.stdout.write("%s\n" % (fmt % args))
        sys.stdout.flush()

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        request_body = self.rfile.read(length) if length else b""
        action = self.headers.get("x-euclid-action", "")

        try:
            if action == "ping":
                result = {
                    "application": os.environ.get("EUCLID_APPLICATION_ID"),
                    "version": os.environ.get("EUCLID_APPLICATION_VERSION"),
                    "instance": os.environ.get("EUCLID_INSTANCE_ID"),
                    "user": self.session.user_id,
                    "pid": os.getpid(),
                    "echo": json.loads(request_body or b"{}"),
                }
            elif action == "list-buckets":
                # The other half of the contract, and the whole reason to use the SDK: one call,
                # authenticated as this application's own principal. No signing, no credentials
                # file, no HTTP - the session above already knows all of it.
                page = self.session.esm().list_buckets(page_size=10)
                result = {"total": page.total, "buckets": [bucket.name for bucket in page.items]}
            else:
                self.respond(404, {"error": "Action not implemented: %s" % action})
                return
            self.respond(200, result)
        except Exception as ex:  # noqa: BLE001 - an application must not die on one bad request
            self.respond(500, {"error": str(ex)})

    def respond(self, status: int, payload: dict):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class UnixHttpServer(socketserver.ThreadingUnixStreamServer):
    allow_reuse_address = True

    def get_request(self):
        # BaseHTTPRequestHandler wants a (host, port) pair to format log lines with; a Unix socket
        # has no such thing, so one is supplied.
        request, _ = super().get_request()
        return request, ("euclid", 0)


def main() -> int:
    socket_path = os.environ.get("EUCLID_SOCKET")
    if not socket_path:
        sys.stderr.write("EUCLID_SOCKET is not set - this program is started by euclid-mgr\n")
        return 1

    Handler.session = open_session()

    if os.path.exists(socket_path):
        os.unlink(socket_path)
    server = UnixHttpServer(socket_path, Handler)

    def shutdown(_signum, _frame):
        # From another thread, always: shutdown() blocks until serve_forever() acknowledges it, and
        # serve_forever() is running on this very thread - calling it here deadlocks, and the process
        # then has to be killed rather than stopping when euclid asks it to.
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    # Logged on start-up because stdout goes to euclid's log: a build that prints its version makes
    # "what is actually running?" answerable from the log alone.
    print("application %s version %s listening on %s as %s"
          % (os.environ.get("EUCLID_APPLICATION_ID", "?"),
             os.environ.get("EUCLID_APPLICATION_VERSION", "(unset)"),
             socket_path, Handler.session.user_id), flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()
        if os.path.exists(socket_path):
            os.unlink(socket_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
