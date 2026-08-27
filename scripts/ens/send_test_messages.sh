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
TOPIC_NAME="test-topic"
ENDPOINT="https://localhost:5566"
CLI="euclid-cli"

usage() {
    cat <<'EOF'
Usage: send_test_messages.sh [-n COUNT] [-t TOPIC_NAME] [-e ENDPOINT] [-c CLI_PATH] [-r]

  -n COUNT       number of messages to send (default: 10)
  -t TOPIC_NAME  topic to send to; created if it doesn't exist yet (default: test-topic)
  -e ENDPOINT    euclid gateway endpoint (default: https://localhost:5566)
  -c CLI_PATH    path to the euclid-cli binary (default: euclid-cli, resolved via PATH)

Requires: euclid-cli (built and on PATH, or pointed to via -c), jq, and a valid
euclid-cli login session (run "euclid-cli access login --user <user> --password <password>" first).
EOF
    exit 1
}

while getopts "n:q:e:c:rh" opt; do
    case "$opt" in
        n) COUNT="$OPTARG" ;;
        q) TOPIC_NAME="$OPTARG" ;;
        e) ENDPOINT="$OPTARG" ;;
        c) CLI="$OPTARG" ;;
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

# Resolve the topic's ERN, creating the topic on the fly if it doesn't exist yet.
ern=$(cli ens get-topic-ern --name "$TOPIC_NAME" 2>/dev/null | jq -r '.ern // empty')
if [ -z "$ern" ]; then
    echo "Topic '$TOPIC_NAME' not found, creating it..."
    ern=$(cli ens create-topic --name "$TOPIC_NAME" | jq -r '.ern')
fi
echo "Sending $COUNT messages to '$TOPIC_NAME' (ern: $ern)"

for ((i = 1; i <= COUNT; i++)); do

    cli ens publish-message --topic "$ern" --body "test message $i" >/dev/null
    echo "  [$i/$COUNT] sent"
done

echo "Done. Sent $COUNT messages"
