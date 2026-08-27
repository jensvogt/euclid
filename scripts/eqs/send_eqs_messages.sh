#!/usr/bin/env bash
#
# Sends a batch of test messages, with a mix of priorities, to an SQS queue.
# Useful for exercising receive-messages' priority-weighted delivery.
#
# Usage: send_test_messages.sh [-n COUNT] [-q QUEUE_NAME] [-e ENDPOINT] [-c CLI_PATH] [-r]
#
#   -n COUNT       number of messages to send (default: 10)
#   -q QUEUE_NAME   queue to send to; created if it doesn't exist yet (default: test-queue)
#   -e ENDPOINT     euclid gateway endpoint (default: https://localhost:5566)
#   -c CLI_PATH     path to the euclid-cli binary (default: euclid-cli, resolved via PATH)
#   -r              assign priorities randomly instead of cycling LOW/MIDDLE/HIGH round-robin
#
# Requires: euclid-cli (built and on PATH, or pointed to via -c), jq, and a valid
# euclid-cli login session (run "euclid-cli access login --user <user> --password <password>" first).

set -euo pipefail

COUNT=10
QUEUE_NAME="test-queue"
ENDPOINT="https://localhost:5566"
CLI="euclid-cli"
RANDOM_PRIORITY=0

usage() {
    cat <<'EOF'
Usage: send_test_messages.sh [-n COUNT] [-q QUEUE_NAME] [-e ENDPOINT] [-c CLI_PATH] [-r]

  -n COUNT       number of messages to send (default: 10)
  -q QUEUE_NAME  queue to send to; created if it doesn't exist yet (default: test-queue)
  -e ENDPOINT    euclid gateway endpoint (default: https://localhost:5566)
  -c CLI_PATH    path to the euclid-cli binary (default: euclid-cli, resolved via PATH)
  -r             assign priorities randomly instead of cycling LOW/MIDDLE/HIGH round-robin

Requires: euclid-cli (built and on PATH, or pointed to via -c), jq, and a valid
euclid-cli login session (run "euclid-cli access login --user <user> --password <password>" first).
EOF
    exit 1
}

while getopts "n:q:e:c:rh" opt; do
    case "$opt" in
        n) COUNT="$OPTARG" ;;
        q) QUEUE_NAME="$OPTARG" ;;
        e) ENDPOINT="$OPTARG" ;;
        c) CLI="$OPTARG" ;;
        r) RANDOM_PRIORITY=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done

if ! [[ "$COUNT" =~ ^[0-9]+$ ]] || [ "$COUNT" -lt 1 ]; then
    echo "error: -n must be a positive integer" >&2
    exit 1
fi

command -v "$CLI" >/dev/null 2>&1 || { echo "error: euclid-cli not found (looked for '$CLI'); use -c to point at the binary" >&2; exit 1; }
command -v jq >/dev/null 2>&1 || { echo "error: jq is required" >&2; exit 1; }

cli() { "$CLI" --pretty false --endpoint "$ENDPOINT" "$@"; }

# Resolve the queue's ERN, creating the queue on the fly if it doesn't exist yet.
ern=$(cli eqs get-queue-ern --name "$QUEUE_NAME" 2>/dev/null | jq -r '.ern // empty')
if [ -z "$ern" ]; then
    echo "Queue '$QUEUE_NAME' not found, creating it..."
    ern=$(cli eqs create-queue --name "$QUEUE_NAME" | jq -r '.ern')
fi
echo "Sending $COUNT messages to '$QUEUE_NAME' (ern: $ern)"

priorities=(LOW MIDDLE HIGH)
counts_low=0
counts_middle=0
counts_high=0

for ((i = 1; i <= COUNT; i++)); do
    if [ "$RANDOM_PRIORITY" -eq 1 ]; then
        priority="${priorities[$((RANDOM % 3))]}"
    else
        priority="${priorities[$(((i - 1) % 3))]}"
    fi

    cli eqs send-message --queue "$ern" --body "test message $i" --priority "$priority" >/dev/null

    case "$priority" in
        LOW) counts_low=$((counts_low + 1)) ;;
        MIDDLE) counts_middle=$((counts_middle + 1)) ;;
        HIGH) counts_high=$((counts_high + 1)) ;;
    esac
    echo "  [$i/$COUNT] sent, priority: $priority"
done

echo "Done. Sent LOW=$counts_low MIDDLE=$counts_middle HIGH=$counts_high"
