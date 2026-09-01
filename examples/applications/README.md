# Applications

An application is a process euclid runs and scales on your behalf. It can be written in any
language: euclid starts it, restarts it if it dies, scales it between a minimum and maximum
instance count, routes requests to it through the gateway, and hands it credentials so it can call
back into euclid. Nothing about it is C++, and it links no euclid library.

## The contract

1. **Listen on `EUCLID_SOCKET`.** The manager passes a Unix socket path in that environment
   variable (and, for euclid's own modules, as `--socket`). Creating the socket is the readiness
   signal: the manager waits for it to appear and kills the process if it does not show up within
   the application's `readyTimeoutMs`.
2. **Speak HTTP/1.1 on that socket**, dispatching on the `x-euclid-action` request header, the way
   every euclid module does. Reply with JSON.
3. **Log to stdout and stderr.** The manager drains both into euclid's log.
4. **Exit on SIGTERM.**

That is the whole of it. `python/euclid_app.py` implements it in the standard library.

## What the manager puts in the environment

| Variable | Meaning |
|---|---|
| `EUCLID_SOCKET` | Unix socket this instance must listen on |
| `EUCLID_APPLICATION_ID`, `EUCLID_APPLICATION_ERN` | which application this is |
| `EUCLID_ACCOUNT_ID`, `EUCLID_REGION`, `EUCLID_USER_ID` | the identity it runs as |
| `EUCLID_ENDPOINT` | gateway URL to call other modules through |
| `EUCLID_SIGNATURE` | `rfc9421` — how to sign those calls |
| `EUCLID_ACCESS_KEY_ID`, `EUCLID_SECRET_ACCESS_KEY` | the access key of the application's EAM user |

Anything set on the application definition (`--environment`) is added underneath, so it cannot
shadow these.

## Calling back into euclid

Requests are signed with [RFC 9421](https://www.rfc-editor.org/rfc/rfc9421) HTTP Message
Signatures, keyed by the access key above, and euclid authenticates them as the application's user.
The signature must cover exactly these components, in this order:

```
"@method" "@path" "@authority" "content-digest"
"x-euclid-account-id" "x-euclid-action" "x-euclid-region" "x-euclid-target" "x-euclid-user-id"
```

with `alg="hmac-sha256"`, a `created` timestamp within 15 minutes of now, and a `Content-Digest`
header (RFC 9530) over the body. The component list is fixed at both ends on purpose: a signature
covering less could be stripped down in transit and still verify.

`python/euclid_app.py`'s `sign()` is a complete implementation in about twenty lines; the euclid
side is `Core::HttpSignature`, and `tests/HttpSignatureTest.cpp` pins the two against each other
with a recorded vector produced by that Python code.

## Deploying one

The artifact lives in an ESM bucket, so deployment is an ordinary upload — through the CLI, an
SDK, or an FTP/SFTP transfer server:

```bash
euclid-cli esm create-bucket --name apps
euclid-cli esm upload-file --bucket apps --key euclid_app.py --file python/euclid_app.py

# the application runs as this user, and signs with its access key
euclid-cli eam create-access-key --user appuser

euclid-cli eap create-application \
    --application-id demo --runtime PYTHON \
    --bucket apps --artifact euclid_app.py \
    --user appuser --min-instances 1 --max-instances 4

euclid-cli eap start-application --application-id demo
euclid-cli eap list-applications
```

The manager copies the artifact out of the bucket to `data/application/demo/`, starts it with
`python3`, and from then on treats it like any other module: round-robin across instances, scale
out when every instance is busy, scale back down after
`euclid.scaling.scale-down-idle-seconds` of idleness, restart on crash.

Reach it through the gateway the same way you reach a module — `x-euclid-target: demo` with an
`x-euclid-action` the application implements.

Redeploying is an upload followed by a restart:

```bash
euclid-cli esm upload-file --bucket apps --key euclid_app.py --file python/euclid_app.py
euclid-cli eap stop-application  --application-id demo
euclid-cli eap start-application --application-id demo
```

## Runtimes

| Runtime | Started as | Notes |
|---|---|---|
| `JAVA` | `java -jar <artifact>` | give it a `readyTimeoutMs` that fits JVM start-up |
| `PYTHON` | `python3 <artifact>` | |
| `NODEJS` | `node <artifact>` | |
| `BINARY` | `<artifact>` | Rust, C++, Go — anything executable; the manager sets the exec bit |

`--command` overrides all of it when an application needs something else entirely (a wrapper
script, an interpreter that isn't on `PATH`). The interpreter is resolved through `PATH`: euclid
launches processes, it does not manage language toolchains.
