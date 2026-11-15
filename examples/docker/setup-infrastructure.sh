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

ADMIN_PASSWORD="${ADMIN_PASSWORD:-admin}"
FTP_PASSWORD="${FTP_PASSWORD:-change-this-ftp-password}"
ACCOUNT="000000000000"          # euclid.account-ids in euclid.json
REGION="eu-central-1"           # euclid.region in euclid.json
USERS=("ftpuser1" "ftpuser2")
GROUP="ftp-users"
BUCKETS=("transfer-server" "file-delivery" "apps")
TOPICS=("protocol")
QUEUES=("file-delivery-queue" "file-delivery-pdf" "file-delivery-image" "file-delivery-xml" "file-delivery-json" "file-delivery-misc")

# The file-delivery application (examples/applications/cpp), deployed from apps/: its binary is
# uploaded to the "apps" bucket and run by the workers. It subscribes file-delivery-queue to the
# transfer-server bucket itself.
APP_ID="file-delivery"
APP_FILE="apps/file-delivery"
APP_BUCKET="apps"
APP_VERSION="1.0.0"

# Manager connection
manager() { compose exec -T euclid euclid-cli --ca-cert /usr/local/euclid/certs/euclid_cert.crt "$@"; }

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
deadline=$((SECONDS + 300))
until [ "$(compose ps --format '{{.Health}}' euclid 2>/dev/null)" = "healthy" ]; do
    if [ "$SECONDS" -ge "$deadline" ]; then
        echo "The manager is not healthy after 5 minutes - is the stack up? (compose ps; compose logs euclid)" >&2
        exit 1
    fi
    sleep 2
done

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

# Create the buckets
for bucket in "${BUCKETS[@]}"; do
  echo "== bucket $bucket"
  step "created" "already exists" esm create-bucket --name "$bucket"
done

# Both servers admit the members of the group and root each user at a home of their own in the
# bucket. They are created stopped, so each one is started as well. The FTP server's passive data
# ports are the range docker-compose.yml publishes.
for server in "ftp-server FTP 2121 --pasv-min 6000 --pasv-max 6010" "sftp-server SFTP 2222"; do
    read -r id protocol port options <<<"$server"
    echo "== $protocol server $id on port $port"
    # shellcheck disable=SC2086 # options is split into its flags on purpose
    step "created" "already exists" \
        ets create-server --server-id "$id" --protocol "$protocol" --port "$port" \
            --bucket "transfer-server" --groups "$GROUP" ${options:-} --directories incoming,outgoing
    step "started" "could not be started" ets start-server --server-id "$id"
done

# The EQS queues: file-delivery-queue receives the transfer bucket's upload events, and the
# file-delivery application announces each file it delivers on the queue of its kind -
# file-delivery-pdf, -image, -xml or -json - which it tells by the file's magic bytes (libmagic) -
# and every other file on file-delivery-misc.
for queue in "${QUEUES[@]}"; do
  echo "== queue $queue"
  step "created" "already exists" eqs create-queue --name "$queue"
done

# The ENS topics: "protocol" receives one message for every step the file-delivery application
# takes - picked up, downloaded, unpacked, delivered, announced, removed, failed. Subscribe a queue
# to it to follow the uploads (ens subscribe).
for topic in "${TOPICS[@]}"; do
  echo "== topic $topic"
  step "created" "already exists" ens create-topic --name "$topic"
done

# The file-delivery application. If the prebuild binary is missing, built it first (examples/applications/cpp/README.md).
echo "== application $APP_ID"
if [ ! -f "$APP_FILE" ]; then
    echo "   skipped: no $APP_FILE - build examples/applications/cpp and copy the binary there"
else
    # The workers are Alpine, which has no glibc: a binary linked against it (built on Debian or
    # Ubuntu without -DFILE_DELIVERY_FULLY_STATIC=ON on Alpine) is deployed but cannot start there.
    if command -v file >/dev/null 2>&1 && file "$APP_FILE" | grep -q 'ld-linux'; then
        echo "   warning: $APP_FILE needs glibc and will not start on the Alpine workers - build it fully static on Alpine"
    fi

    # Uploaded on every run, so running this again deploys a rebuilt binary. euclid-cli runs inside
    # the manager's container, so the file is copied in first.
    compose cp "$APP_FILE" "euclid:/tmp/$APP_ID" >/dev/null
    step "uploaded to $APP_BUCKET" "could not be uploaded" \
        esm upload-file --bucket "$APP_BUCKET" --key "$APP_ID" --file "/tmp/$APP_ID"
    compose exec -T euclid rm -f "/tmp/$APP_ID"

    # Its own principal, app-file-delivery, narrowed to the two buckets and the queues: the one it
    # reads, and the ones it announces delivered files on.
    step "created" "already exists" \
        eap create-application --application-id "$APP_ID" --runtime BINARY \
            --bucket "$APP_BUCKET" --artifact "$APP_ID" --version "$APP_VERSION" \
            --min-instances 1 --max-instances 4 \
            --buckets transfer-server,file-delivery --queues "$(IFS=,; echo "${QUEUES[*]}")"

    # The built-in "application" role, which create-application grants, covers neither looking a
    # bucket up by name nor transfers in parts (files over 5 MiB): a role of its own for both, on
    # the two buckets only.
    step "role $APP_ID created" "role $APP_ID already exists" \
        eam create-role --name "$APP_ID" \
            --permission esm:get-bucket-ern \
            --permission esm:create-download --permission esm:download-part --permission esm:complete-download \
            --permission esm:create-upload --permission esm:upload-part --permission esm:complete-upload \
            --description "Look up and move large files between the transfer and delivery buckets"
    step "granted the $APP_ID role" "already holds the $APP_ID role" \
        eam grant-role --role "$APP_ID" \
            --principal "ern:eam:$REGION:$ACCOUNT:user:app-$APP_ID" \
            --namespace development \
            --resource "ern:esm:$REGION:$ACCOUNT:development:bucket:transfer-server*" \
                       "ern:esm:$REGION:$ACCOUNT:development:bucket:file-delivery*"

    # And removing an upload from the transfer bucket once it is delivered: listing it, to check
    # it is still the upload that was delivered, and deleting it. A role of its own, granted on
    # transfer-server alone, so the application can never delete a file it has delivered.
    step "role $APP_ID-cleanup created" "role $APP_ID-cleanup already exists" \
        eam create-role --name "$APP_ID-cleanup" \
            --permission esm:list-objects --permission esm:delete-object \
            --description "Remove delivered uploads from the transfer bucket"
    step "granted the $APP_ID-cleanup role" "already holds the $APP_ID-cleanup role" \
        eam grant-role --role "$APP_ID-cleanup" \
            --principal "ern:eam:$REGION:$ACCOUNT:user:app-$APP_ID" \
            --namespace development \
            --resource "ern:esm:$REGION:$ACCOUNT:development:bucket:transfer-server*"

    # And publishing its protocol. --buckets and --queues narrowed the application to those
    # resources, and create-application has no option to name a topic, so a role of its own,
    # granted on the protocol topic alone.
    step "role $APP_ID-protocol created" "role $APP_ID-protocol already exists" \
        eam create-role --name "$APP_ID-protocol" \
            --permission ens:get-topic-ern --permission ens:publish-message \
            --description "Publish the file-delivery protocol"
    step "granted the $APP_ID-protocol role" "already holds the $APP_ID-protocol role" \
        eam grant-role --role "$APP_ID-protocol" \
            --principal "ern:eam:$REGION:$ACCOUNT:user:app-$APP_ID" \
            --namespace development \
            --resource "ern:ens:$REGION:$ACCOUNT:development:topic:protocol"

    step "started" "could not be started" eap start-application --application-id "$APP_ID"
fi

echo
echo "Done. Log in as ${USERS[0]} with the password in FTP_PASSWORD:"
echo "  sftp -P 2222 ${USERS[0]}@localhost"
echo "  ftp localhost 2121"