#!/usr/bin/env bash
# Sets up an FTP and an SFTP transfer server in front of one ESM bucket, with two users allowed to log
# in through a user group - once, after the first "docker compose up -d". Safe to run again: what
# already exists is left as it is.
#
#   ADMIN_PASSWORD   the administrator's password (the bootstrap one is "admin")
#   FTP_PASSWORD     the password the FTP/SFTP users are registered with
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

ADMIN_PASSWORD="${ADMIN_PASSWORD:-admin}"
FTP_PASSWORD="${FTP_PASSWORD:-change-this-ftp-password}"
ACCOUNT="000000000000"          # euclid.account-ids in euclid.json
REGION="eu-central-1"           # euclid.region in euclid.json
USERS=(ftpuser1 ftpuser2)
GROUP="ftp-users"
BUCKET="transfer-server"

# Manager connection
manager() { docker compose exec -T euclid euclid-cli --ca-cert /usr/local/euclid/certs/euclid_cert.crt "$@"; }

# Runs a manager command, reporting either what it did or that it had been done already. A failure
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

echo "Waiting for the manager..."
until [ "$(docker compose ps --format '{{.Health}}' euclid 2>/dev/null)" = "healthy" ]; do sleep 2; done

# Every command below needs administrator privileges.
manager eam login --user admin --password "$ADMIN_PASSWORD" --account 000000000000 --namespace development >/dev/null

echo "== user group $GROUP"
step "created" "already exists" \
    eam create-user-group --name "$GROUP" --description "User group for the FTP/SFTP users"

# Being in the group only admits a user to the servers: each command they send is checked against
# their EAM grants as well. The built-in "transfer" role covers all of them.
step "granted the transfer role" "already holds the transfer role" \
    eam grant-role --role transfer --principal "ern:eam:$REGION:$ACCOUNT:userGroup:$GROUP"

# Create the users and add them to the FTP/SFTP user group
for user in "${USERS[@]}"; do
    echo "== user $user"
    step "registered" "already registered" \
        eam register --user "$user" --password "$FTP_PASSWORD" --email "$user@example.invalid" \
            --account-id "$ACCOUNT" --region "$REGION"
    step "added to $GROUP" "already in $GROUP" \
        eam user-group-add-user --user-group "$GROUP" --user "$user"
done

# Create the transfer server bucket
echo "== bucket $BUCKET"
step "created" "already exists" esm create-bucket --name "$BUCKET"

# Both servers admit the members of the group and root each user at a home of their own in the
# bucket. They are created stopped, so each one is started as well. The FTP server's passive data
# ports are the range docker-compose.yml publishes.
for server in "ftp-server FTP 2121 --pasv-min 6000 --pasv-max 6010" "sftp-server SFTP 2222"; do
    read -r id protocol port options <<<"$server"
    echo "== $protocol server $id on port $port"
    # shellcheck disable=SC2086 # options is split into its flags on purpose
    step "created" "already exists" \
        ets create-server --server-id "$id" --protocol "$protocol" --port "$port" \
            --bucket "$BUCKET" --groups "$GROUP" ${options:-} --directories incoming,outgoing
    step "started" "could not be started" ets start-server --server-id "$id"
done

echo
echo "Done. Log in as ${USERS[0]} with the password in FTP_PASSWORD:"
echo "  sftp -P 2222 ${USERS[0]}@localhost"
echo "  ftp localhost 2121"