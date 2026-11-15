#!/usr/bin/env bash
# Removes everything setup-infrastructure.sh created: the FTP and SFTP transfer servers, the bucket
# behind them together with every file uploaded to it, the FTP/SFTP users, their user group and the
# role granted to it. Safe to run again: what is already gone is reported and skipped.
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

# Keep these equal to the ones in setup-infrastructure.sh.
ADMIN_PASSWORD="${ADMIN_PASSWORD:-admin}"
ACCOUNT="000000000000"          # euclid.account-ids in euclid.json
REGION="eu-central-1"           # euclid.region in euclid.json
USERS=("ftpuser1" "ftpuser2")
GROUP="ftp-users"
BUCKETS=("transfer-server" "file-delivery")
SERVERS=("ftp-server" "sftp-server")
QUEUES=("file-delivery-pdf" "file-delivery-image" "file-delivery-xml" "file-delivery-json")

manager() { docker compose exec -T euclid euclid-cli --ca-cert /usr/local/euclid/certs/euclid_cert.crt "$@"; }

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
    echo "This deletes the transfer servers ${SERVERS[*]}, the buckets with every file in it,"
    echo "the users ${USERS[*]} and the user group $GROUP."
    read -rp "Continue? [y/N] " answer
    case "$answer" in
        y | Y | yes | YES) ;;
        *) echo "Nothing deleted."; exit 0 ;;
    esac
fi

echo "Waiting for the manager..."
until [ "$(docker compose ps --format '{{.Health}}' euclid 2>/dev/null)" = "healthy" ]; do sleep 2; done

# Every command below needs administrator privileges.
manager eam login --user admin --password "$ADMIN_PASSWORD" --account "$ACCOUNT" --namespace development >/dev/null

# The servers first: deleting one stops it, and nobody can log in or upload while the rest goes.
for server in "${SERVERS[@]}"; do
    echo "== transfer server $server"
    step "deleted" "already gone" ets delete-server --server-id "$server"
done

# Takes every object in the bucket with it - the files clients uploaded through the servers.
for bucket in "${BUCKETS[@]}"; do
  echo "== bucket $bucket"
  step "deleted" "already gone" esm delete-bucket --bucket "$bucket"
done

# Deletes the created queues, includes all messages existing in the queue
for queue in "${QUEUES[@]}"; do
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
grants="$(manager eam list-grants --principal "$principal" --role transfer 2>/dev/null \
    | grep -oE '"grantId"[[:space:]]*:[[:space:]]*"[^"]+"' | sed -E 's/.*"([^"]+)"$/\1/' || true)"
if [ -z "$grants" ]; then
    echo "   holds no transfer role"
fi
for grant in $grants; do
    step "transfer role revoked ($grant)" "could not revoke grant $grant" eam revoke-role --grant-id "$grant"
done
step "deleted" "already gone" eam delete-user-group --name "$GROUP"

echo
echo "Done."
