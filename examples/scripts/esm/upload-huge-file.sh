#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

#
# Sends a huge file to an storage bucket.
# Useful for exercising upload-file.
#
# Usage: upload-huge-file.sh [-s SIZE] [-b BUCKET_NAME] [-e ENDPOINT] [-c CLI_PATH] [-j CONCURRENCY]
#                            [-w SECONDS]
#
#   -s SIZE         size of the file in bytes to upload (default: 10GB)
#   -b BUCKET_NAME  name of the bucket to upload to; created if it doesn't exist yet (default: test-bucket)
#   -e ENDPOINT     euclid gateway endpoint (default: https://localhost:5566)
#   -c CLI_PATH     path to the euclid-cli binary (default: euclid-cli, resolved via PATH)
#   -j CONCURRENCY  number of parts to upload in parallel, passed through to "upload-file
#                   --concurrency" (default: euclid-cli's own default, currently 4). The storage
#                   pool can never have more instances busy at once than there are concurrent
#                   requests in flight, so bump this to actually exercise scale-up toward
#                   maxInstances.
#   -w SECONDS      how long to wait for the server to finish assembling the object before giving
#                   up (default: 600). complete-upload answers as soon as it has accepted the
#                   parts and does the assembling and hashing in a background pass, so a large
#                   object is UPLOADED with no checksum for a while and COMPLETED afterwards.
#                   0 skips the wait and reports whatever complete-upload said.
#
# Requires: euclid-cli (built and on PATH, or pointed to via -c), jq, and a valid
# euclid-cli login session (run "euclid-cli eam login --user <user> --password <password>" first).

set -euo pipefail

UUID=$(uuidgen)
SIZE=10737418240
BUCKET_NAME="test-bucket"
FILE_NAME="/tmp/$UUID.asc"
OBJECT_KEY="tmp/$UUID.asc"
ENDPOINT="https://localhost:5566"
CLI="euclid-cli"
CONCURRENCY=""
WAIT_SECONDS=600

usage() {
    cat <<'EOF'
Usage: upload-huge-file.sh [-s SIZE] [-b BUCKET_NAME] [-e ENDPOINT] [-c CLI_PATH] [-j CONCURRENCY]
                           [-w SECONDS]

  -s SIZE         size of the file in bytes to upload (default: 10GB)
  -b BUCKET_NAME  name of the bucket to upload to; created if it doesn't exist yet (default: test-bucket)
  -e ENDPOINT     euclid gateway endpoint (default: https://localhost:5566)
  -c CLI_PATH     path to the euclid-cli binary (default: euclid-cli, resolved via PATH)
  -j CONCURRENCY  number of parts to upload in parallel, passed through to "upload-file
                  --concurrency" (default: euclid-cli's own default, currently 4)
  -w SECONDS      how long to wait for the server to finish assembling the object (default: 600;
                  0 skips the wait)

Requires: euclid-cli (built and on PATH, or pointed to via -c), jq, and a valid
euclid-cli session (run "euclid-cli eam login --user <user> --password <password>" first).
EOF
    exit 1
}

while getopts "s:b:e:c:j:w:h" opt; do
    case "$opt" in
        s) SIZE="$OPTARG" ;;
        b) BUCKET_NAME="$OPTARG" ;;
        e) ENDPOINT="$OPTARG" ;;
        c) CLI="$OPTARG" ;;
        j) CONCURRENCY="$OPTARG" ;;
        w) WAIT_SECONDS="$OPTARG" ;;
        h) usage ;;
        *) usage ;;
    esac
done

if ! [[ "$SIZE" =~ ^[0-9]+$ ]] || [ "$SIZE" -lt 1 ]; then
    echo "error: -s must be a positive integer" >&2
    exit 1
fi

if [ -n "$CONCURRENCY" ] && { ! [[ "$CONCURRENCY" =~ ^[0-9]+$ ]] || [ "$CONCURRENCY" -lt 1 ]; }; then
    echo "error: -j must be a positive integer" >&2
    exit 1
fi

if ! [[ "$WAIT_SECONDS" =~ ^[0-9]+$ ]]; then
    echo "error: -w must be a non-negative integer" >&2
    exit 1
fi

command -v "$CLI" >/dev/null 2>&1 || { echo "error: euclid-cli not found (looked for '$CLI'); use -c to point at the binary" >&2; exit 1; }
command -v jq >/dev/null 2>&1 || { echo "error: jq is required" >&2; exit 1; }

cli() { "$CLI" --pretty false --endpoint "$ENDPOINT" "$@"; }

# Resolve the bucket's ERN, creating the bucket on the fly if it doesn't exist yet.
ern=$(cli esm get-bucket-ern --name "$BUCKET_NAME" 2>/dev/null | jq -r '.ern // empty') || true
if [ -z "$ern" ]; then
    echo "Bucket '$BUCKET_NAME' not found, creating it..."
    ern=$(cli esm create-bucket --name "$BUCKET_NAME" | jq -r '.ern')
fi
echo "Uploading a file with size $SIZE to '$BUCKET_NAME' (ern: $ern)"

trap 'rm -f "$FILE_NAME"' EXIT
fallocate -l "$SIZE" "$FILE_NAME"

upload_args=(esm upload-file --bucket "$ern" --key "$OBJECT_KEY" --file "$FILE_NAME")
if [ -n "$CONCURRENCY" ]; then
    upload_args+=(--concurrency "$CONCURRENCY")
fi
result=$(cli "${upload_args[@]}")
key=$(echo "$result" | jq -r '.key')

echo "Accepted: $result"

# complete-upload answers as soon as the parts are all in, and assembles and hashes them in a
# background pass. Until that finishes the object is UPLOADED, its size is whatever has been
# written so far and its md5Sum is empty - so a script that reported the first answer as the
# finished article would be describing a file nobody can read yet. For a 10 GB upload the pass
# is not instant, which is the whole reason this waits rather than sleeping once and hoping.
if [ "$WAIT_SECONDS" -eq 0 ]; then
    echo "Not waiting for assembly (-w 0). The object is COMPLETED once the server's background pass finishes."
    exit 0
fi

echo "Waiting up to ${WAIT_SECONDS}s for the server to assemble and hash it..."
started=$(date +%s)
while :; do
    object=$(cli esm list-objects --bucket "$ern" --prefix "$key" | jq -c '.objects[0] // empty')
    status=$(echo "$object" | jq -r '.status // empty')
    [ "$status" = "COMPLETED" ] && break

    elapsed=$(( $(date +%s) - started ))
    if [ "$elapsed" -ge "$WAIT_SECONDS" ]; then
        echo "error: still '$status' after ${elapsed}s - the object is not readable yet." >&2
        echo "       It may still finish; raise -w, or check with:" >&2
        echo "       $CLI --endpoint $ENDPOINT esm list-objects --bucket $ern --prefix $key" >&2
        exit 1
    fi
    # Only on a terminal: a carriage return in a log or a pipe leaves every tick on one
    # unreadable line, which is how this read the first time it was run into "tail".
    if [ -t 1 ]; then
        printf '\r  %ss elapsed, status %s   ' "$elapsed" "${status:-unknown}"
    fi
    sleep 2
done
# Blanks the progress line rather than just returning to its start, or the line below lands on
# top of it and the two are read as one sentence.
[ -t 1 ] && printf '\r%*s\r' 45 ''

echo "Done. Uploaded file with size $SIZE to bucket $BUCKET_NAME after $(( $(date +%s) - started ))s of assembly:"
echo "$object" | jq '{key, size, status, md5Sum, contentType}'

# The one check worth making: what the server holds is the size that was sent. An assembly that
# lost a part would otherwise be a COMPLETED object nobody looked twice at.
stored=$(echo "$object" | jq -r '.size')
if [ "$stored" != "$SIZE" ]; then
    echo "error: stored size $stored is not the $SIZE that was uploaded" >&2
    exit 1
fi

