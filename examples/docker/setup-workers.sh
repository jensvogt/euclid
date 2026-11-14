#!/usr/bin/env bash
# Makes each worker in docker-compose.yml a euclid principal and logs it in - once, after the first
# "docker compose up -d". Safe to run again: what already exists is left as it is.
#
# A worker is an ordinary euclid client (docs/worker-nodes.md §3.2): a principal of its own holding
# the built-in "node" role, logged in once. The login runs inside the worker's own container, as its
# own account, so the credentials land on the worker's credentials volume - where the waiting worker
# picks them up and starts.
#
#   ADMIN_PASSWORD   the administrator's password (the bootstrap one is "admin")
#   NODE_PASSWORD    the password the node principals are registered with
set -euo pipefail
cd "$(dirname "$0")"

ADMIN_PASSWORD="${ADMIN_PASSWORD:-admin}"
NODE_PASSWORD="${NODE_PASSWORD:-change-this-node-password}"
ACCOUNT="000000000000"          # euclid.account-ids in euclid.json
REGION="eu-central-1"           # euclid.region in euclid.json
WORKERS=(wrk-1 wrk-2)

manager() { docker compose exec -T euclid euclid-cli --ca-cert /usr/local/euclid/certs/euclid_cert.crt "$@"; }

echo "Waiting for the manager..."
until [ "$(docker compose ps --format '{{.Health}}' euclid 2>/dev/null)" = "healthy" ]; do sleep 2; done

manager eam login --user admin --password "$ADMIN_PASSWORD" >/dev/null

for node in "${WORKERS[@]}"; do
    echo "== $node"

    # Registered and granted the node role - and only that: exactly what a worker calls as itself.
    # Account-wide (no --namespace, no --resource), so it may fetch an artifact from any bucket.
    manager eam register --user "$node" --email "$node@example.invalid" --password "$NODE_PASSWORD" \
        --region "$REGION" --account-id "$ACCOUNT" >/dev/null 2>&1 \
        && echo "   registered" || echo "   already registered"
    manager eam grant-role --role node --principal "ern:eam:$REGION:$ACCOUNT:user:$node" >/dev/null 2>&1 \
        && echo "   granted the node role" || echo "   already holds the node role"

    # Logged in from its own container: the login writes a session token and a long-lived access key
    # to /usr/local/euclid-wrk/.euclid/credentials, and the worker signs with the key from then on.
    if docker compose exec -T "$node" test -s /usr/local/euclid-wrk/.euclid/credentials; then
        echo "   already logged in"
    else
        docker compose exec -T "$node" euclid-cli --endpoint https://euclid:5566 --ca-cert /certs/euclid_cert.crt \
            eam login --user "$node" --password "$NODE_PASSWORD" >/dev/null
        echo "   logged in"
    fi
done

echo
echo "Done. The workers register within a few seconds:"
echo "  docker compose exec -T euclid euclid-cli --ca-cert /usr/local/euclid/certs/euclid_cert.crt eap list-nodes"
