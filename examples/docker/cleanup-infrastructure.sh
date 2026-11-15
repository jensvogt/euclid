#!/usr/bin/env bash
# Removes everything setup-infrastructure.sh created: the file-delivery application with its role
# and queue, the FTP and SFTP transfer servers, the buckets together with every file in them, the
# queues, the FTP/SFTP users, their user group and the role granted to it. Safe to run again: what
# is already gone is reported and skipped.
#
#   ADMIN_PASSWORD   the administrator's password (the bootstrap one is "admin")
#   -y               do not ask before deleting
set -euo pipefail

# Started by a double click on Windows, the script runs in a window of its own that closes the
# moment it exits - with "set -e", before anyone can read why it stopped. So on Windows it waits
# for Enter first, however it ends.
case "${OSTYPE:-}" in
    msys* | cygwin*)
        trap 'status=$?; echo; [ "$status" -ne 0 ] && echo "Failed (exit code $status)."; read -rp "Press Enter to close..." _ || true; exit "$status"' EXIT
        ;;
esac

# Git Bash rewrites every argument that looks like a Unix path into a Windows one before docker.exe
# sees it - /usr/local/euclid/... becomes C:/Program Files/Git/usr/local/euclid/... - but these
# paths are inside the container. Turned off; elsewhere the variables mean nothing.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

cd "$(dirname "$0")"

# Compose v2 as a docker plugin ("docker compose") where it is installed, else the standalone
# "docker-compose" - which is all some distributions ship (Ubuntu's docker.io has no plugin unless
# docker-compose-v2 is installed too). Checked up front: the wait below hides docker's errors, so a
# missing compose would otherwise just wait forever.
if docker compose version >/dev/null 2>&1; then
    compose() { docker compose "$@"; }
elif command -v docker-compose >/dev/null 2>&1; then
    compose() { docker-compose "$@"; }
else
    echo "Neither 'docker compose' nor 'docker-compose' is available - install Docker Compose." >&2
    exit 1
fi

# Keep these equal to the ones in setup-infrastructure.sh.
ADMIN_PASSWORD="${ADMIN_PASSWORD:-admin}"
ACCOUNT="000000000000"          # euclid.account-ids in euclid.json
REGION="eu-central-1"           # euclid.region in euclid.json
USERS=("ftpuser1" "ftpuser2")
GROUP="ftp-users"
BUCKETS=("transfer-server" "file-delivery" "apps")
SERVERS=("ftp-server" "sftp-server")
TOPICS=("protocol")
QUEUES=("file-delivery-pdf" "file-delivery-image" "file-delivery-xml" "file-delivery-json" "file-delivery-misc")
APP_ID="file-delivery"
# Created by the application itself on its first start, not by setup-infrastructure.sh.
APP_QUEUE="$APP_ID-queue"
PRINTER_ID="protocol-printer"
PRINTER_QUEUE="$PRINTER_ID-queue"
SPLITTER_ID="xml-splitter"
SPLITTER_OUTPUT="splitting-output"

manager() { compose exec -T euclid euclid-cli --ca-cert /usr/local/euclid/certs/euclid_cert.crt "$@"; }

# The ids of every grant of role $2 that principal $1 holds, one per line.
grant_ids() {
    manager eam list-grants --principal "$1" --role "$2" 2>/dev/null \
        | grep -oE '"grantId"[[:space:]]*:[[:space:]]*"[^"]+"' | sed -E 's/.*"([^"]+)"$/\1/' || true
}

# Runs a manager command, reporting either what it did or that there was nothing to do. A failure
# is most often the latter, but its message is shown too, so a real one does not pass unnoticed.
step() {
    local done="$1" already="$2" output; shift 2
    if output="$(manager "$@" 2>&1)"; then
        echo "   $done"
    else
        echo "   $already?"
        sed 's/^/      /' <<<"$output"
    fi
}

if [ "${1:-}" != "-y" ]; then
    echo "This deletes the applications $APP_ID, $PRINTER_ID and $SPLITTER_ID, the transfer servers ${SERVERS[*]},"
    echo "the buckets ${BUCKETS[*]} with every file in them, the queues, the topics ${TOPICS[*]},"
    echo "the users ${USERS[*]} and the user group $GROUP."
    read -rp "Continue? [y/N] " answer
    case "$answer" in
        y | Y | yes | YES) ;;
        *) echo "Nothing deleted."; exit 0 ;;
    esac
fi

echo "Waiting for the manager..."
deadline=$((SECONDS + 300))
until [ "$(compose ps --format '{{.Health}}' euclid 2>/dev/null)" = "healthy" ]; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "The manager is not healthy after 5 minutes - is the stack up? (compose ps; compose logs euclid)" >&2
        exit 1
    fi
    sleep 2
done

# Every command below needs administrator privileges.
manager eam login --user admin --password "$ADMIN_PASSWORD" --account "$ACCOUNT" --namespace development >/dev/null

# The application first, so nothing is still reading the queue or writing to the buckets while they
# go. Its grants are revoked while its principal still exists; the principal, app-$APP_ID, is
# deleted with the application.
echo "== application $APP_ID"
step "stopped" "not running" eap stop-application --application-id "$APP_ID"
appPrincipal="ern:eam:$REGION:$ACCOUNT:user:app-$APP_ID"
for role in "$APP_ID" "$APP_ID-cleanup" "$APP_ID-protocol"; do
    for grant in $(grant_ids "$appPrincipal" "$role"); do
        step "$role role revoked ($grant)" "could not revoke grant $grant" eam revoke-role --grant-id "$grant"
    done
done
step "deleted" "already gone" eap delete-application --application-id "$APP_ID"
for role in "$APP_ID" "$APP_ID-cleanup" "$APP_ID-protocol"; do
    step "role $role deleted" "role $role already gone" eam delete-role --name "$role"
done

# The protocol-printer application, the same way: its grants, then itself, then its role.
echo "== application $PRINTER_ID"
step "stopped" "not running" eap stop-application --application-id "$PRINTER_ID"
printerPrincipal="ern:eam:$REGION:$ACCOUNT:user:app-$PRINTER_ID"
for grant in $(grant_ids "$printerPrincipal" "$PRINTER_ID"); do
    step "$PRINTER_ID role revoked ($grant)" "could not revoke grant $grant" eam revoke-role --grant-id "$grant"
done
step "deleted" "already gone" eap delete-application --application-id "$PRINTER_ID"
step "role $PRINTER_ID deleted" "role $PRINTER_ID already gone" eam delete-role --name "$PRINTER_ID"

# The xml-splitter application, the same way.
echo "== application $SPLITTER_ID"
step "stopped" "not running" eap stop-application --application-id "$SPLITTER_ID"
splitterPrincipal="ern:eam:$REGION:$ACCOUNT:user:app-$SPLITTER_ID"
for grant in $(grant_ids "$splitterPrincipal" "$SPLITTER_ID"); do
    step "$SPLITTER_ID role revoked ($grant)" "could not revoke grant $grant" eam revoke-role --grant-id "$grant"
done
step "deleted" "already gone" eap delete-application --application-id "$SPLITTER_ID"
step "role $SPLITTER_ID deleted" "role $SPLITTER_ID already gone" eam delete-role --name "$SPLITTER_ID"

# The subscriptions on the topics - protocol-printer's queue on "protocol", and any a developer's
# local run left there: they name queues that go below.
for topic in "${TOPICS[@]}"; do
    echo "== subscriptions on topic $topic"
    subscriptions="$(manager ens list-subscriptions --topic "$topic" 2>/dev/null \
        | grep -oE '"ern"[[:space:]]*:[[:space:]]*"[^"]+"' | sed -E 's/.*"([^"]+)"$/\1/' || true)"
    if [ -z "$subscriptions" ]; then
        echo "   none"
    fi
    for subscription in $subscriptions; do
        step "unsubscribed ($subscription)" "could not unsubscribe $subscription" ens unsubscribe --subscription "$subscription"
    done
done

# The subscriptions the application put on the transfer bucket: they name its queue, which goes
# below, and would otherwise be left pointing at nothing.
echo "== subscriptions on transfer-server"
subscriptions="$(manager esm list-subscriptions --bucket transfer-server 2>/dev/null \
    | grep -oE '"ern"[[:space:]]*:[[:space:]]*"[^"]+"' | sed -E 's/.*"([^"]+)"$/\1/' || true)"
if [ -z "$subscriptions" ]; then
    echo "   none"
fi
for subscription in $subscriptions; do
    step "unsubscribed ($subscription)" "could not unsubscribe $subscription" esm unsubscribe --subscription "$subscription"
done

# The servers next: deleting one stops it, and nobody can log in or upload while the rest goes.
for server in "${SERVERS[@]}"; do
    echo "== transfer server $server"
    step "deleted" "already gone" ets delete-server --server-id "$server"
done

# Takes every object in the bucket with it - the files clients uploaded through the servers.
for bucket in "${BUCKETS[@]}"; do
  echo "== bucket $bucket"
  step "deleted" "already gone" esm delete-bucket --bucket "$bucket"
done

# Deletes the topics, with every message they still hold.
for topic in "${TOPICS[@]}"; do
  echo "== topic $topic"
  step "deleted" "already gone" ens delete-topic --topic "$topic"
done

# Deletes the created queues, includes all messages existing in the queue
for queue in "${QUEUES[@]}" "$APP_QUEUE" "$PRINTER_QUEUE" "$SPLITTER_OUTPUT"; do
  echo "== queue $queue"
  step "deleted" "already gone" eqs delete-queue --queue "$queue"
done

# Delete the users
for user in "${USERS[@]}"; do
    echo "== user $user"
    step "deleted" "already gone" eam delete-user --userId "$user"
done

# Grants are not removed with the group they name, so they are revoked first, by id - every
# "transfer" grant the group holds, should setup-infrastructure.sh have run more than once.
echo "== user group $GROUP"
principal="ern:eam:$REGION:$ACCOUNT:userGroup:$GROUP"
grants="$(grant_ids "$principal" transfer)"
if [ -z "$grants" ]; then
    echo "   holds no transfer role"
fi
for grant in $grants; do
    step "transfer role revoked ($grant)" "could not revoke grant $grant" eam revoke-role --grant-id "$grant"
done
step "deleted" "already gone" eam delete-user-group --name "$GROUP"

echo
echo "Done."
