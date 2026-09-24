#!/usr/bin/env python3
"""RFC 9421 signing in the standard library alone - a reference, not an example to copy.

**If you are writing an application, you do not want this file.** Use the SDK for your language;
`euclid_app.py` beside it is the example, and euclid-pdk signs for you. Nothing here is needed to
write an application, and hand-rolling a signature is a good way to spend an afternoon on a
mismatched byte.

This exists for one reason: euclid's own `tests/HttpSignatureTest.cpp` pins the output of this code.
Signing and verifying with the same implementation proves nothing - a shared mistake verifies
perfectly - so the C++ verifier is checked against a signature produced here instead, written
against the RFC rather than against euclid's code. The signature base, the digest and the HMAC all
have to agree byte for byte.

Run it to reproduce that vector and check it against what the C++ test asserts. It exits non-zero
on a mismatch, so it is a reproducer rather than only a printer:

    python3 rfc9421_reference.py
"""

import base64
import hashlib
import hmac
import time

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


def signature_base(method: str, path: str, authority: str, headers: dict, parameters: str) -> bytes:
    """The exact bytes both sides HMAC. Any difference here - even in spacing - fails the signature."""
    derived = {"@method": method.upper(), "@path": path, "@authority": authority.lower()}
    # Component names are lowercase, HTTP header names are not case-sensitive - so the lookup
    # cannot be a plain dict access, or "Content-Digest" would not answer to "content-digest".
    by_lower_name = {name.lower(): value for name, value in headers.items()}

    lines = []
    for name in COVERED_COMPONENTS:
        value = derived[name] if name.startswith("@") else by_lower_name[name]
        lines.append('"%s": %s' % (name, value.strip()))
    # The parameters are repeated verbatim on the last line; the verifier rebuilds the base from
    # what it received, so this has to be the same string that went into Signature-Input.
    lines.append('"@signature-params": %s' % parameters)
    return "\n".join(lines).encode()


def sign(method: str, path: str, authority: str, headers: dict, body: bytes,
         key_id: str, secret: str, created: int | None = None) -> dict:
    """Adds Content-Digest, Signature-Input and Signature to headers, and returns it.

    `created` is a parameter only so that the vector below is reproducible. A real caller leaves it
    alone and gets `now`, which euclid requires to be within fifteen minutes of its own clock.
    """
    headers["Content-Digest"] = content_digest(body)

    components = " ".join('"%s"' % name for name in COVERED_COMPONENTS)
    parameters = '(%s);created=%d;keyid="%s";alg="hmac-sha256"' % (
        components, int(created if created is not None else time.time()), key_id)

    signature = hmac.new(secret.encode(), signature_base(method, path, authority, headers, parameters),
                         hashlib.sha256).digest()
    headers["Signature-Input"] = "sig1=" + parameters
    headers["Signature"] = "sig1=:" + base64.b64encode(signature).decode() + ":"
    return headers


def main() -> None:
    """Reproduces the vector tests/HttpSignatureTest.cpp is pinned against, and checks it.

    Every input is the one the C++ test carries, including `created` and the key: the point is that
    two independent implementations produce the same bytes, which is only demonstrated if both are
    given identical input. If this prints a mismatch, one of the two has changed and the other has
    not.

    The timestamp being fixed is also why the C++ side verifies through BuildSignatureBase() rather
    than Verify(): a recorded `created` is always too old for a verifier that rightly refuses stale
    signatures.
    """
    body = b'{"pageSize":10}'
    authority = "localhost:5566"
    headers = {
        "Host": authority,
        "x-euclid-target": "esm",
        "x-euclid-action": "list-buckets",
        "x-euclid-region": "eu-central-1",
        "x-euclid-account-id": "000000000000",
        "x-euclid-user-id": "appuser",
    }
    signed = sign("POST", "/", authority, headers, body, "AKIAEXAMPLE", "topsecret", created=1788255252)

    # What the C++ test asserts, spelled out here so this file is a check rather than only a printer.
    expected_digest = "sha-256=:0iKG6xLm1EmJVf31IlRq2NUIwWuh2GKuD8una+P3jh8=:"
    expected_signature = "sig1=:RmC2h+H6ntZSgvXb6q5XbHqeRvTcBlyipElVGWViv2M=:"

    print("Content-Digest:  %s" % signed["Content-Digest"])
    print("Signature-Input: %s" % signed["Signature-Input"])
    print("Signature:       %s" % signed["Signature"])
    print()
    print("signature base, as HMACed:")
    print(signature_base("POST", "/", authority, signed, signed["Signature-Input"][len("sig1="):]).decode())
    print()

    if signed["Content-Digest"] == expected_digest and signed["Signature"] == expected_signature:
        print("matches tests/HttpSignatureTest.cpp")
    else:
        print("DOES NOT MATCH tests/HttpSignatureTest.cpp")
        print("  expected digest:    %s" % expected_digest)
        print("  expected signature: %s" % expected_signature)
        raise SystemExit(1)


if __name__ == "__main__":
    main()
