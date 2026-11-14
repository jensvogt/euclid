#!/bin/sh
# The euclid-wrk image's entrypoint: the environment a docker-compose file sets, turned into the
# worker's command line - the configuration file has no environment overrides of its own.
#
#   EUCLID_ENDPOINT           the gateway, e.g. https://euclid:5566        (--endpoint)
#   EUCLID_NODE               this node's name; defaults to the container's host name  (--node)
#   EUCLID_LABELS             placement labels, comma separated: gpu,zone=a  (--label, repeated)
#   EUCLID_ADDRESS            the address the gateway reaches this node at  (--address)
#   EUCLID_CA_CERT            a CA certificate to trust, e.g. the manager's euclid_cert.crt  (--ca-cert)
#
# And the credentials, unless a file is mounted at /usr/local/euclid-wrk/.euclid/credentials (the
# one "euclid-cli eam login" writes): this node's principal's access key, which is all a worker
# signs with - the same shape the manager writes for its local node.
#
#   EUCLID_ACCESS_KEY_ID, EUCLID_SECRET_ACCESS_KEY    required together
#   EUCLID_USER_ID, EUCLID_ACCOUNT_ID, EUCLID_REGION  the principal they belong to
#
# Anything passed to the container ("command:" in compose) is appended, so every euclid-wrk option
# is still available.
set -eu

CREDENTIALS=/usr/local/euclid-wrk/.euclid/credentials

# A value goes into JSON unescaped, so refuse what would need escaping rather than write a file the
# worker cannot parse. Keys, ids and region names never contain these.
json_safe() {
    case "$2" in
        *'"'* | *'\'*) echo "error: $1 must not contain a quote or a backslash" >&2; exit 2 ;;
    esac
}

if [ -n "${EUCLID_ACCESS_KEY_ID:-}" ] || [ -n "${EUCLID_SECRET_ACCESS_KEY:-}" ]; then
    : "${EUCLID_ACCESS_KEY_ID:?EUCLID_SECRET_ACCESS_KEY is set but EUCLID_ACCESS_KEY_ID is not}"
    : "${EUCLID_SECRET_ACCESS_KEY:?EUCLID_ACCESS_KEY_ID is set but EUCLID_SECRET_ACCESS_KEY is not}"
    for name in EUCLID_ACCESS_KEY_ID EUCLID_SECRET_ACCESS_KEY EUCLID_USER_ID EUCLID_ACCOUNT_ID EUCLID_REGION; do
        eval "value=\${$name:-}"
        json_safe "$name" "$value"
    done
    umask 077
    printf '{"token":"","userId":"%s","accountId":"%s","region":"%s","accessKeyId":"%s","secretAccessKey":"%s","isAdmin":false,"namespace":""}\n' \
        "${EUCLID_USER_ID:-}" "${EUCLID_ACCOUNT_ID:-}" "${EUCLID_REGION:-eu-central-1}" \
        "$EUCLID_ACCESS_KEY_ID" "$EUCLID_SECRET_ACCESS_KEY" > "$CREDENTIALS.new"
    mv "$CREDENTIALS.new" "$CREDENTIALS"
fi

# Whatever the container was given, then the options from the environment - appended, so the
# container's own arguments keep their quoting; the order means nothing to the option parser. Set
# an option one way or the other, not both: given twice it is refused. --wait-for-config so a
# credentials file mounted or copied in after the start is waited for rather than a restart loop.
set -- "$@" --config /usr/local/euclid-wrk/etc/euclid-wrk.json --wait-for-config
[ -n "${EUCLID_ENDPOINT:-}" ] && set -- "$@" --endpoint "$EUCLID_ENDPOINT"
[ -n "${EUCLID_NODE:-}" ] && set -- "$@" --node "$EUCLID_NODE"
[ -n "${EUCLID_ADDRESS:-}" ] && set -- "$@" --address "$EUCLID_ADDRESS"
[ -n "${EUCLID_CA_CERT:-}" ] && set -- "$@" --ca-cert "$EUCLID_CA_CERT"
if [ -n "${EUCLID_LABELS:-}" ]; then
    OLD_IFS=$IFS; IFS=,
    for label in $EUCLID_LABELS; do
        [ -n "$label" ] && set -- "$@" --label "$label"
    done
    IFS=$OLD_IFS
fi

exec /usr/local/euclid-wrk/bin/euclid-wrk "$@"
