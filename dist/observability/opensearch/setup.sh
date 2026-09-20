#!/usr/bin/env bash
# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.

#
# Puts the index templates, the retention policy and the two write aliases into OpenSearch.
#
# Run once against a cluster, before Fluent Bit ships anything to it. Running it again is
# harmless: the templates and the policy are replaced, and an alias that already exists is left
# where it is rather than bootstrapped a second time.
#
# Usage: setup.sh [-u URL] [-U USER] [-P PASSWORD] [-k]
#
#   -u URL       OpenSearch base URL (default: https://localhost:9200)
#   -U USER      user to authenticate as (default: $OPENSEARCH_USER). Leave it empty for a
#                cluster with the security plugin turned off, which is what a development
#                installation usually is - no credentials are then sent at all.
#   -P PASSWORD  its password (default: $OPENSEARCH_PASSWORD)
#   -k           do not verify the cluster's certificate; for a development cluster with a
#                self-signed one, and not for anything else
#

set -euo pipefail

URL="https://localhost:9200"
USER="${OPENSEARCH_USER:-}"
PASSWORD="${OPENSEARCH_PASSWORD:-}"
INSECURE=0

while getopts "u:U:P:kh" opt; do
    case "$opt" in
        u) URL="$OPTARG" ;;
        U) USER="$OPTARG" ;;
        P) PASSWORD="$OPTARG" ;;
        k) INSECURE=1 ;;
        h) sed -n '7,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
        *) sed -n '7,20p' "$0" | sed 's/^# \{0,1\}//'; exit 1 ;;
    esac
done

command -v curl >/dev/null 2>&1 || { echo "error: curl is required" >&2; exit 1; }
command -v jq   >/dev/null 2>&1 || { echo "error: jq is required" >&2; exit 1; }
# No credentials is a valid answer, not a missing one: a cluster with the security plugin off
# refuses a request that carries an Authorization header it has no way to check. Naming a user
# without a password is the mistake worth catching.
if [ -n "$USER" ] && [ -z "$PASSWORD" ]; then
    echo "error: -U names a user but there is no password; pass -P or set OPENSEARCH_PASSWORD" >&2
    exit 1
fi

here="$(cd "$(dirname "$0")" && pwd)"

os() {
    local method="$1" path="$2" body="${3:-}"
    local args=(-s -w '\n%{http_code}' -X "$method" "$URL$path" -H 'Content-Type: application/json')
    [ -n "$USER" ] && args+=(-u "$USER:$PASSWORD")
    [ "$INSECURE" -eq 1 ] && args+=(-k)
    [ -n "$body" ] && args+=(--data-binary "@$body")
    curl "${args[@]}"
}

check() {
    local what="$1" answer="$2"
    local status="${answer##*$'\n'}" payload="${answer%$'\n'*}"
    if [ "$status" -lt 200 ] || [ "$status" -ge 300 ]; then
        echo "error: $what failed ($status): $payload" >&2
        exit 1
    fi
    echo "    $what"
}

echo "==> $URL"

check "index template euclid-logs"     "$(os PUT /_index_template/euclid-logs     "$here/euclid-logs-template.json")"
check "index template euclid-app-logs" "$(os PUT /_index_template/euclid-app-logs "$here/euclid-app-logs-template.json")"

# The retention policy applies itself to any index matching its ism_template, which is why it has
# to be in place before the first index is created rather than after.
check "retention policy euclid-logs" "$(os PUT /_plugins/_ism/policies/euclid-logs "$here/euclid-logs-ism-policy.json")"

# Fluent Bit writes to "euclid-logs", which has to be an alias pointing at a real index with
# is_write_index set - otherwise rollover has nothing to roll and the alias is just a name for one
# index that grows forever.
bootstrap() {
    local alias="$1"
    local answer status
    answer=$(os GET "/_alias/$alias")
    status="${answer##*$'\n'}"
    if [ "$status" = "200" ]; then
        echo "    alias $alias exists"
        return
    fi
    local body
    body=$(mktemp)
    printf '{"aliases":{"%s":{"is_write_index":true}}}' "$alias" > "$body"
    check "alias $alias -> $alias-000001" "$(os PUT "/$alias-000001" "$body")"
    rm -f "$body"
}

bootstrap euclid-logs
bootstrap euclid-app-logs

echo
echo "Done. Point Fluent Bit at this cluster and set, in euclid.json:"
echo '  "logging": { "format": "json", "file-active": true }'
