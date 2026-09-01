#!/usr/bin/env python3
"""Minimal euclid application, in the standard library only.

An application is a process euclid starts and scales. The contract is small enough to fit in one
file, and this file is that contract spelled out:

  1. Serve HTTP/1.1 on the Unix socket path given in EUCLID_SOCKET. Creating that socket is what
     tells the manager the instance is ready - it waits for it to appear, and kills the process
     if it does not within the application's readyTimeoutMs.
  2. Dispatch on the x-euclid-action request header, the way every euclid module does.
  3. Log to stdout/stderr; the manager drains both into its own log.
  4. Exit on SIGTERM.

Calling back into euclid is optional, and the second half of this file shows how: the manager
passes the application's access key in EUCLID_ACCESS_KEY_ID/EUCLID_SECRET_ACCESS_KEY, and requests
signed with it (RFC 9421 HTTP Message Signatures) are authenticated as the EAM user the
application runs as.

Deploy it with:

    euclid-cli esm upload-file --bucket apps --key euclid_app.py --file euclid_app.py
    euclid-cli eap create-application --application-id demo --runtime PYTHON \\
        --bucket apps --artifact euclid_app.py --user appuser
    euclid-cli eap start-application --application-id demo
"""

import base64
import hashlib
import hmac
import http.client
import json
import os
import signal
import socket
import socketserver
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler

# Exactly the components euclid's verifier requires, in exactly this order. The list is fixed on
# purpose at both ends: a signature that covered less could be stripped down in transit and still
# verify.
COVERED_COMPONENTS = [
    "@method",
    "@path",
    "@authority",
    "content-digest",
    "x-euclid-account-id",
    "x-euclid-action",
    "x-euclid-region",
    "x-euclid-target",
    "x-euclid-user-id",
]


def content_digest(body: bytes) -> str:
    """The RFC 9530 Content-Digest header value binding a body to its signature."""
    return "sha-256=:" + base64.b64encode(hashlib.sha256(body).digest()).decode() + ":"


def sign(method: str, path: str, authority: str, headers: dict, body: bytes, key_id: str, secret: str) -> dict:
    """Adds Content-Digest, Signature-Input and Signature to headers, and returns it."""
    headers["Content-Digest"] = content_digest(body)

    components = " ".join('"%s"' % name for name in COVERED_COMPONENTS)
    parameters = '(%s);created=%d;keyid="%s";alg="hmac-sha256"' % (components, int(time.time()), key_id)

    derived = {"@method": method.upper(), "@path": path, "@authority": authority.lower()}
    # Component names are lowercase, HTTP header names are not case-sensitive - so the lookup
    # cannot be a plain dict access, or "Content-Digest" would not answer to "content-digest".
    by_lower_name = {name.lower(): value for name, value in headers.items()}
    lines = []
    for name in COVERED_COMPONENTS:
        value = derived[name] if name.startswith("@") else by_lower_name[name]
        lines.append('"%s": %s' % (name, value.strip()))
    # The parameters are repeated verbatim on the last line; the verifier rebuilds the base from
    # what it received, so any difference here - even in spacing - is a failed signature.
    lines.append('"@signature-params": %s' % parameters)
    base = "\n".join(lines).encode()

    signature = hmac.new(secret.encode(), base, hashlib.sha256).digest()
    headers["Signature-Input"] = "sig1=" + parameters
    headers["Signature"] = "sig1=:" + base64.b64encode(signature).decode() + ":"
    return headers


def call_euclid(target: str, action: str, body: dict) -> dict:
    """Calls another euclid module through the gateway, signed as this application's user."""
    endpoint = os.environ["EUCLID_ENDPOINT"]
    scheme, _, hostport = endpoint.partition("://")
    host, _, port = hostport.partition(":")
    port = int(port or (443 if scheme == "https" else 80))
    authority = "%s:%d" % (host, port)

    payload = json.dumps(body).encode()
    headers = {
        "Host": authority,
        "Content-Type": "application/json",
        "x-euclid-target": target,
        "x-euclid-action": action,
        "x-euclid-region": os.environ.get("EUCLID_REGION", ""),
        "x-euclid-account-id": os.environ.get("EUCLID_ACCOUNT_ID", ""),
        "x-euclid-user-id": os.environ.get("EUCLID_USER_ID", ""),
    }
    sign("POST", "/", authority, headers, payload,
         os.environ["EUCLID_ACCESS_KEY_ID"], os.environ["EUCLID_SECRET_ACCESS_KEY"])

    if scheme == "https":
        # Development installations use a self-signed gateway certificate; point this at the real
        # CA (ssl.create_default_context(cafile=...)) anywhere it matters.
        context = ssl._create_unverified_context()
        connection = http.client.HTTPSConnection(host, port, context=context, timeout=30)
    else:
        connection = http.client.HTTPConnection(host, port, timeout=30)

    connection.request("POST", "/", body=payload, headers=headers)
    response = connection.getresponse()
    raw = response.read()
    return {"status": response.status, "body": json.loads(raw or b"{}")}


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        # Straight to stdout, which the manager drains into euclid's own log.
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
                    "user": os.environ.get("EUCLID_USER_ID"),
                    "pid": os.getpid(),
                    "echo": json.loads(request_body or b"{}"),
                }
            elif action == "list-buckets":
                # Proves the other half of the contract: this call is authenticated purely by the
                # signature, using credentials the manager put in this process's environment.
                result = call_euclid("esm", "list-buckets", {"pageSize": 10, "pageIndex": 0})
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

    if os.path.exists(socket_path):
        os.unlink(socket_path)

    server = UnixHttpServer(socket_path, Handler)

    def shutdown(_signum, _frame):
        # From another thread, always: shutdown() blocks until serve_forever() acknowledges it,
        # and serve_forever() is running on this very thread - calling it here deadlocks, and the
        # process then has to be killed rather than stopping when euclid asks it to.
        threading.Thread(target=server.shutdown, daemon=True).start()

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    print("application %s listening on %s" % (os.environ.get("EUCLID_APPLICATION_ID", "?"), socket_path), flush=True)
    try:
        server.serve_forever()
    finally:
        server.server_close()
        if os.path.exists(socket_path):
            os.unlink(socket_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
