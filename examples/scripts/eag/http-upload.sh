#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

#
# Publishes a bucket as an HTTP upload endpoint and puts a file through it, from nothing.
#
# Everything an HTTP upload needs, in the order it needs it: a bucket to write into, a role
# naming the four actions an upload makes, a user holding that role for that bucket, and a
# gateway route that turns a PUT into all of it. The upload itself is then one curl with no
# euclid-cli involved, which is the point - what is being demonstrated is that something outside
# euclid can send a file to a bucket without knowing euclid exists.
#
# Usage: http-upload.sh [-u USER] [-p PASSWORD] [-b BUCKET] [-r ROUTE_PATH] [-g GATEWAY]
#                       [-s SIZE] [-e ENDPOINT] [-c CLI_PATH] [-n NAMESPACE] [-k] [-h]
#
#   -u USER        test user to create and upload as (default: upload-tester)
#   -p PASSWORD    that user's password (default: a generated one, printed at the end)
#   -b BUCKET      bucket to create and upload into (default: testbucket)
#   -r ROUTE_PATH  path the gateway publishes (default: /upload)
#   -g GATEWAY     the API gateway's own listener, where the upload goes
#                  (default: http://localhost:8080 - euclid.modules.eag.listeners.<ns>.port)
#   -s SIZE        size of the test file in bytes (default: 3145728, i.e. 3 MB - enough to be
#                  split into parts, since the gateway's default part size is 5 MB... it is not,
#                  which is the point: see the note printed at the end)
#   -e ENDPOINT    euclid's own gateway, where euclid-cli talks (default: https://localhost:5566)
#   -c CLI_PATH    path to the euclid-cli binary (default: euclid-cli, resolved via PATH)
#   -n NAMESPACE   namespace to create everything in (default: the session's own)
#   -A ACCOUNT_ID  account to create the user in (default: the session's own, read from
#                  ~/.euclid/credentials)
#   -R REGION      region to create the user in (default: the session's own, likewise)
#   -k             keep what was created; without it the route, user, role and bucket are removed
#   -h             this help
#
# Requires: euclid-cli, jq, curl, and an *administrator* euclid-cli session - creating users,
# roles and routes is administrator-only. Run "euclid-cli eam login --user <admin> --password <pw>"
# first.

set -euo pipefail

USER_ID="upload-tester"
PASSWORD=""
BUCKET="testbucket"
ROUTE_PATH="/upload"
GATEWAY="http://localhost:8080"
SIZE=3145728
ENDPOINT="https://localhost:5566"
CLI="euclid-cli"
NAMESPACE=""
ACCOUNT_ID=""
REGION=""
KEEP=0

ROLE_NAME="http-upload"
ROUTE_ID="http-upload-example"

usage() {
    sed -n '7,38p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
}

while getopts "u:p:b:r:g:s:e:c:n:A:R:kh" opt; do
    case "$opt" in
        u) USER_ID="$OPTARG" ;;
        p) PASSWORD="$OPTARG" ;;
        b) BUCKET="$OPTARG" ;;
        r) ROUTE_PATH="$OPTARG" ;;
        g) GATEWAY="$OPTARG" ;;
        s) SIZE="$OPTARG" ;;
        e) ENDPOINT="$OPTARG" ;;
        c) CLI="$OPTARG" ;;
        n) NAMESPACE="$OPTARG" ;;
        A) ACCOUNT_ID="$OPTARG" ;;
        R) REGION="$OPTARG" ;;
        k) KEEP=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done

[[ "$ROUTE_PATH" == /* ]] || { echo "error: -r must start with '/'" >&2; exit 1; }
[[ "$SIZE" =~ ^[0-9]+$ ]] && [ "$SIZE" -ge 1 ] || { echo "error: -s must be a positive integer" >&2; exit 1; }

for tool in "$CLI" jq curl; do
    command -v "$tool" >/dev/null 2>&1 || { echo "error: '$tool' not found" >&2; exit 1; }
done

cli() { "$CLI" --pretty false --endpoint "$ENDPOINT" "$@"; }

ns_args=()
[ -n "$NAMESPACE" ] && ns_args=(--namespace "$NAMESPACE")

# A password nobody has to think about, and one that satisfies whatever the installation asks of
# one. Printed at the end rather than here, so it is next to the curl that uses it.
GENERATED_PASSWORD=0
if [ -z "$PASSWORD" ]; then
    GENERATED_PASSWORD=1
    PASSWORD="Upload-$(head -c 9 /dev/urandom | base64 | tr -dc 'A-Za-z0-9')1!"
fi

echo "==> Checking the euclid-cli session"
cli eam list-users --page-size 1 >/dev/null 2>&1 || {
    echo "error: no usable session. Run: $CLI --endpoint $ENDPOINT eam login --user <admin> --password <pw>" >&2
    echo "       (creating users, roles and routes is administrator-only)" >&2
    exit 1
}

# "eam register" names the account and region explicitly rather than taking the caller's - an
# administrator may well be creating a user somewhere other than where they themselves live. Here
# they are the same thing, so both come from the session, which euclid-cli writes down when it
# logs in. Override with -A/-R to put the test user somewhere else.
# What this run created, so that what it cleans up is exactly that. A failure half way through
# used to leave the pieces it had already made behind, and the next run then met a user whose
# password it could not know - which is how this script came to need a cleanup that runs on the
# way out rather than only on the way to the end.
CREATED_BUCKET=0
CREATED_ROLE=0
CREATED_USER=0
CREATED_ROUTE=0
GRANT_ID=""
TEST_FILE=""

cleanup() {
    local code=$?
    [ -n "$TEST_FILE" ] && rm -f "$TEST_FILE"

    if [ "$KEEP" -eq 1 ]; then
        [ "$code" -eq 0 ] && echo && echo "Kept (-k): route '$ROUTE_ID', user '$USER_ID', role '$ROLE_NAME', bucket '$BUCKET'."
        return $code
    fi

    # Only what this run made. A bucket that was already there is somebody else's, and a run that
    # reused it must not take it away on the way out.
    if [ "$CREATED_ROUTE$CREATED_USER$CREATED_ROLE$CREATED_BUCKET$GRANT_ID" != "0000" ]; then
        echo
        echo "==> Cleaning up what this run created"
        [ "$CREATED_ROUTE" -eq 1 ] && cli eag delete-route --route-id "$ROUTE_ID" >/dev/null 2>&1 || true
        [ -n "$GRANT_ID" ] && cli eam revoke-role --grant-id "$GRANT_ID" >/dev/null 2>&1 || true
        [ "$CREATED_USER" -eq 1 ] && cli eam delete-user --userId "$USER_ID" >/dev/null 2>&1 || true
        [ "$CREATED_ROLE" -eq 1 ] && cli eam delete-role --name "$ROLE_NAME" >/dev/null 2>&1 || true
        if [ "$CREATED_BUCKET" -eq 1 ] && [ -n "${bucket_ern:-}" ]; then
            cli esm purge-bucket --bucket "$bucket_ern" >/dev/null 2>&1 || true
            cli esm delete-bucket --bucket "$bucket_ern" >/dev/null 2>&1 || true
        fi
        echo "    done"
    fi
    return $code
}
trap cleanup EXIT

CREDENTIALS="${EUCLID_CREDENTIALS:-$HOME/.euclid/credentials}"
if [ -z "$ACCOUNT_ID" ] || [ -z "$REGION" ]; then
    [ -r "$CREDENTIALS" ] || {
        echo "error: cannot read $CREDENTIALS for the session's account and region; pass -A and -R" >&2
        exit 1
    }
    [ -z "$ACCOUNT_ID" ] && ACCOUNT_ID=$(jq -r '.accountId // empty' "$CREDENTIALS")
    [ -z "$REGION" ] && REGION=$(jq -r '.region // empty' "$CREDENTIALS")
fi
[ -n "$ACCOUNT_ID" ] && [ -n "$REGION" ] || {
    echo "error: no account or region in $CREDENTIALS; pass -A and -R" >&2
    exit 1
}
echo "    account $ACCOUNT_ID, region $REGION"

# ---------------------------------------------------------------------------------------------
# 1. The bucket the uploads land in
# ---------------------------------------------------------------------------------------------
echo "==> Bucket '$BUCKET'"
bucket_ern=$(cli esm get-bucket-ern --name "$BUCKET" 2>/dev/null | jq -r '.ern // empty') || true
if [ -z "$bucket_ern" ]; then
    bucket_ern=$(cli esm create-bucket --name "$BUCKET" | jq -r '.ern')
    CREATED_BUCKET=1
    echo "    created $bucket_ern"
else
    echo "    exists  $bucket_ern"
fi

# ---------------------------------------------------------------------------------------------
# 2. A role naming exactly what an HTTP upload does
# ---------------------------------------------------------------------------------------------
#
# Four actions, and no more. The gateway terminates the upload and then makes these calls *as the
# caller* - it never acts as itself - so what the user may write is decided by ESM against this
# role, exactly as it would be for the same calls from the CLI.
#
# abort-upload is in the list because the failure path needs it: a part ESM refuses, or a body
# that does not match the digest it was signed with, and the gateway throws the staged parts away
# rather than leaving them for somebody to find. A role without it uploads fine and litters.
echo "==> Role '$ROLE_NAME'"
if cli eam get-role --name "$ROLE_NAME" >/dev/null 2>&1; then
    echo "    exists"
else
    cli eam create-role --name "$ROLE_NAME" \
        --permission esm:create-upload \
        --permission esm:upload-part \
        --permission esm:complete-upload \
        --permission esm:abort-upload \
        --description "Send a file to a bucket over HTTP" >/dev/null
    CREATED_ROLE=1
    echo "    created with esm:create-upload, upload-part, complete-upload, abort-upload"
fi

# ---------------------------------------------------------------------------------------------
# 3. The user who uploads
# ---------------------------------------------------------------------------------------------
echo "==> User '$USER_ID'"
if cli eam get-user --user-id "$USER_ID" >/dev/null 2>&1; then
    # EAM has no action that changes somebody's password, so a user who was already here keeps
    # whatever password they were made with - and the one generated above is not it. Said now
    # rather than at the upload, which is where it used to surface as a bare 401.
    if [ "$GENERATED_PASSWORD" -eq 1 ]; then
        echo "error: user '$USER_ID' already exists, and its password cannot be read or changed." >&2
        echo "       Give it with -p, pick another user with -u, or remove it:" >&2
        echo "       $CLI --endpoint $ENDPOINT eam delete-user --userId $USER_ID" >&2
        exit 1
    fi
    echo "    exists, using the password given with -p"
else
    cli eam register --user "$USER_ID" --password "$PASSWORD" --email "$USER_ID@example.com" \
        --account-id "$ACCOUNT_ID" --region "$REGION" >/dev/null
    CREATED_USER=1
    echo "    created"
fi
user_ern=$(cli eam get-user --user-id "$USER_ID" | jq -r '.user.ern')
echo "    $user_ern"

# ---------------------------------------------------------------------------------------------
# 4. The grant: that role, that user, that bucket
# ---------------------------------------------------------------------------------------------
#
# Scoped to the bucket's ERN rather than left at "*". The difference is the whole point of
# granting rather than making somebody an administrator: this user may upload here and nowhere
# else, and ESM is what enforces it - the gateway route does not decide it and cannot widen it.
echo "==> Grant"
if cli eam list-grants --principal "$user_ern" | jq -e --arg r "$ROLE_NAME" '.grants[]? | select(.role == $r)' >/dev/null 2>&1; then
    echo "    exists"
else
    # grant-role answers {"grant": {...}}, so the id is a level down - the same Grant model
    # list-grants carries each of its own, rather than a shape of its own.
    GRANT_ID=$(cli eam grant-role --role "$ROLE_NAME" --principal "$user_ern" --resource "$bucket_ern" | jq -r '.grant.grantId // empty')
    echo "    granted '$ROLE_NAME' on $bucket_ern${GRANT_ID:+ (grantId: $GRANT_ID)}"
fi

# ---------------------------------------------------------------------------------------------
# 5. The route that publishes the bucket as a path
# ---------------------------------------------------------------------------------------------
#
# --type upload makes the gateway the endpoint rather than a way to one: the body is streamed
# into the bucket a part at a time and nothing is forwarded, so the size of what can be sent is a
# question about the bucket rather than about the gateway's memory.
#
# --authentication basic is what lets the curl below be one line. The gateway verifies the
# password against the same EAM user store everything else uses and then logs in as them to get
# the session its ESM calls carry - a real login, not a session it invented on their behalf. Use
# "euclid" instead for a caller holding a token or signing with RFC 9421; nothing else changes.
#
# --key-prefix confines every key this route writes, the way a transfer server's home directory
# does. Without it anybody who may upload at all may overwrite anything the bucket holds.
echo "==> Route '$ROUTE_ID' on $ROUTE_PATH"
if cli eag get-route --route-id "$ROUTE_ID" >/dev/null 2>&1; then
    echo "    exists"
else
    cli eag create-route --route-id "$ROUTE_ID" --path "$ROUTE_PATH" \
        --type upload \
        --bucket "$bucket_ern" \
        --authentication basic \
        --key-prefix "incoming/$USER_ID" \
        --max-bytes 1073741824 \
        --methods PUT \
        "${ns_args[@]}" >/dev/null
    CREATED_ROUTE=1
    echo "    created -> $bucket_ern, keys under incoming/$USER_ID/, up to 1 GB"
fi

# The gateway re-reads its routes every euclid.modules.eag.refresh-seconds (5 by default), so a
# route created a moment ago is not being served yet. Waited for rather than slept through, since
# a fixed sleep is either too short on a busy machine or wasted on an idle one.
echo "==> Waiting for the gateway to pick the route up"
key="$(date +%Y%m%d-%H%M%S).bin"
target="$GATEWAY$ROUTE_PATH/$key"
for _ in $(seq 1 30); do
    # curl prints 000 itself when it never got an answer, so there is nothing to add on failure -
    # doing both once produced a status of "000000", which is not a thing.
    status=$(curl -s -o /dev/null -w '%{http_code}' -u "$USER_ID:$PASSWORD" -X PUT --data-binary '' "$target" || true)

    # 000 is "no answer at all": nothing listening, or the wrong port. 404 is "no route for this
    # path" - the gateway is there and has not re-read its routes yet. Anything else is an answer
    # from the route itself: 201 for the empty body that just went, 401 if the password is wrong,
    # 413/415 if the route refused it. Only the last of those means it is serving, which is why
    # 000 is not simply "not 404" - reading it that way reported an unreachable gateway as ready
    # and then failed at the upload with nothing useful to say.
    case "$status" in
        000|404) sleep 1 ;;
        *) break ;;
    esac
done
case "$status" in
    000) echo "error: nothing answered at $GATEWAY - is eag running, and is that its listener port?" >&2
         echo "       ($CLI eag list-listeners shows the ports it answers on)" >&2
         exit 1 ;;
    404) echo "error: the gateway is up but is not serving $ROUTE_PATH." >&2
         echo "       It re-reads its routes every euclid.modules.eag.refresh-seconds; raise that wait if it is long." >&2
         exit 1 ;;
esac
echo "    serving (an empty PUT answered $status)"

# ---------------------------------------------------------------------------------------------
# 6. The upload itself - no euclid-cli, no SDK, one curl
# ---------------------------------------------------------------------------------------------
file="$(mktemp -t euclid-upload-XXXXXX)"
TEST_FILE="$file"
head -c "$SIZE" /dev/urandom > "$file"

echo "==> Uploading $SIZE bytes to $target"
body=$(curl -s -w '\n%{http_code}' -u "$USER_ID:$PASSWORD" \
            -H 'Content-Type: application/octet-stream' \
            -X PUT --data-binary "@$file" "$target")
status="${body##*$'\n'}"
body="${body%$'\n'*}"

if [ "$status" = "000" ] || [ -z "$status" ]; then
    echo "error: the upload never reached $GATEWAY" >&2
    exit 1
fi
if [ "$status" != "201" ]; then
    echo "error: upload answered $status: $body" >&2
    exit 1
fi
echo "    $body"

# ---------------------------------------------------------------------------------------------
# 7. It is really there
# ---------------------------------------------------------------------------------------------
stored_key=$(echo "$body" | jq -r '.key')

# complete-upload answers as soon as it has accepted the parts and assembles them in a background
# pass, so the object is UPLOADED with a size of zero for a moment and COMPLETED with its real
# size and checksum once that pass finishes. Waited for rather than read straight away, because
# reading it straight away shows the in-between state and looks like a bug.
echo "==> Waiting for ESM to assemble it"
for _ in $(seq 1 30); do
    object=$(cli esm list-objects --bucket "$bucket_ern" --prefix "$stored_key" | jq -c '.objects[0] // empty')
    [ -n "$object" ] && [ "$(echo "$object" | jq -r '.status')" = "COMPLETED" ] && break
    sleep 1
done

echo "==> The object ESM now holds"
echo "$object" | jq '{key, size, status, md5Sum}'

if [ "$(echo "$object" | jq -r '.size')" != "$SIZE" ]; then
    echo "error: stored size $(echo "$object" | jq -r '.size') is not the $SIZE that was sent" >&2
    exit 1
fi

echo
echo "Done. The pieces, and what each one decided:"
echo "  bucket   $bucket_ern"
echo "  role     $ROLE_NAME  (the four actions an upload makes, and nothing else)"
echo "  user     $USER_ID / $PASSWORD"
echo "  grant    that role, that user, that bucket - not '*'"
echo "  route    $ROUTE_PATH -> upload, basic auth, keys under incoming/$USER_ID/"
echo "  object   $stored_key"
echo
echo "Try it yourself:"
echo "  curl -u $USER_ID:$PASSWORD -X PUT --data-binary @somefile $GATEWAY$ROUTE_PATH/somefile"
echo
echo "Note on part size: the route's --part-size defaults to 5 MB, so a file under that is sent to"
echo "ESM in one part and a larger one in several. Either way the gateway holds one part at a time,"
echo "which is why the size of what you can send here is not bounded by its memory. Raise"
echo "--max-bytes on the route to allow more than the 1 GB this example set."

