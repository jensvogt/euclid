#!/usr/bin/env bash
#
# Sends a huge file to an storage bucket.
# Useful for exercising upload-file.
#
# Usage: upload-huge-file.sh [-s SIZE] [-b BUCKET_NAME] [-e ENDPOINT] [-c CLI_PATH] [-j CONCURRENCY]
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
#
# Requires: euclid-cli (built and on PATH, or pointed to via -c), jq, and a valid
# euclid-cli login session (run "euclid-cli access login --user <user> --password <password>" first).

set -euo pipefail

UUID=$(uuidgen)
SIZE=10737418240
BUCKET_NAME="test-bucket"
FILE_NAME="/tmp/$UUID.asc"
OBJECT_KEY="tmp/$UUID.asc"
ENDPOINT="https://localhost:5566"
CLI="euclid-cli"
CONCURRENCY=""

usage() {
    cat <<'EOF'
Usage: upload-huge-file.sh [-s SIZE] [-b BUCKET_NAME] [-e ENDPOINT] [-c CLI_PATH] [-j CONCURRENCY]

  -s SIZE         size of the file in bytes to upload (default: 10GB)
  -b BUCKET_NAME  name of the bucket to upload to; created if it doesn't exist yet (default: test-bucket)
  -e ENDPOINT     euclid gateway endpoint (default: https://localhost:5566)
  -c CLI_PATH     path to the euclid-cli binary (default: euclid-cli, resolved via PATH)
  -j CONCURRENCY  number of parts to upload in parallel, passed through to "upload-file
                  --concurrency" (default: euclid-cli's own default, currently 4)

Requires: euclid-cli (built and on PATH, or pointed to via -c), jq, and a valid
euclid-cli session (run "euclid-cli eam login --user <user> --password <password>" first).
EOF
    exit 1
}

while getopts "s:b:e:c:j:h" opt; do
    case "$opt" in
        s) SIZE="$OPTARG" ;;
        b) BUCKET_NAME="$OPTARG" ;;
        e) ENDPOINT="$OPTARG" ;;
        c) CLI="$OPTARG" ;;
        j) CONCURRENCY="$OPTARG" ;;
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

fallocate -l "$SIZE" "$FILE_NAME"

upload_args=(esm upload-file --bucket-ern "$ern" --key "$OBJECT_KEY" --file "$FILE_NAME")
if [ -n "$CONCURRENCY" ]; then
    upload_args+=(--concurrency "$CONCURRENCY")
fi
result=$(cli "${upload_args[@]}")

echo "Done. Uploaded file with size $SIZE to bucket $BUCKET_NAME. Result:"
echo "$result"

rm -rf "$FILE_NAME"
