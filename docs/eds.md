# EDS - the domain name service

EDS is a DNS server. It runs as an ordinary module (`euclid-eds`), so the manager starts and supervises it. It does
three things:

1. **It answers for zones you store in it**, authoritatively. These are records you create through the API or the CLI,
   like Route 53 hosted zones.
2. **It names what euclid runs.** Every application, every live worker node and the manager get a name in
   `euclid.internal`, which is rebuilt every few seconds from what is actually running.
3. **It forwards everything else** to upstream resolvers, but only if you configure some, and only for clients on
   networks you allow.

It has two ports. Zones and records are managed over the module's Unix socket, with the usual credentials and
grants. DNS queries are served on UDP and TCP port 1053, and DNS has no credentials, so anybody who can reach that port
can ask.

## Managing zones

```bash
euclid-cli eds create-zone --name corp.example [--ttl 300] [--description "office network"]
euclid-cli eds list-zones
euclid-cli eds get-zone --name corp.example
euclid-cli eds delete-zone --name corp.example        # and every record in it
```

**A zone's name is unique across the installation**, not just within an account. Every other euclid resource is
named per account and namespace, but a DNS question carries no account, so two accounts each holding `corp.example`
would leave EDS no way to choose between them. The account and namespace say who owns the zone and may change it. The
zone's ERN, `ern:eds:<region>:<account>:<namespace>:zone:<name>`, is what grants are written against.

Asking for another account's zone gets a 404, the same as asking for one that doesn't exist.

Zones cannot be created inside `euclid.internal`, because that domain belongs to the service names.

## Records

Records are stored as **record sets**: every value of one type at one name. `put-record` replaces the whole set, so
there is never a moment when half the old addresses and half the new ones are served:

```bash
euclid-cli eds put-record --zone corp.example --name api  --type A     --value 10.1.2.3 --value 10.1.2.4
euclid-cli eds put-record --zone corp.example --name www  --type CNAME --value api.corp.example
euclid-cli eds put-record --zone corp.example --name @    --type MX    --value "10 mail.corp.example"
euclid-cli eds put-record --zone corp.example --name _ldap._tcp --type SRV --value "0 5 389 dc1.corp.example"
euclid-cli eds put-record --zone corp.example --name @    --type TXT   --value "v=spf1 -all"
euclid-cli eds put-record --zone corp.example --name "*.dev" --type A  --value 10.9.0.1 --ttl 30
euclid-cli eds list-records --zone corp.example [--name api] [--type A]
euclid-cli eds delete-record --zone corp.example --name api --type A
```

**Names** can be given in several forms:

| Form | Example | Meaning |
|------|---------|---------|
| Relative | `api` | `api.corp.example` |
| Fully qualified | `api.corp.example` | `api.corp.example` |
| Trailing dot | `api.corp.example.` | `api.corp.example` |
| Apex | `@` or nothing | `corp.example` |
| Wildcard | `*.dev` | `*.dev.corp.example` |

Everything is compared case-insensitively. An answer carries the name the client asked for, spelled the way it asked.

**Types and values** use zone-file presentation format, without the quoting:

| Type | Value |
|------|-------|
| A | `192.0.2.1` |
| AAAA | `2001:db8::1` |
| CNAME, NS, PTR | `target.example.com` |
| MX | `10 mail.example.com` |
| SRV | `10 5 8080 host.example.com` (priority, weight, port, target) |
| TXT | any text, up to 4000 bytes, split into 255-byte strings on the wire |

Values are checked when they are written, so a value the server couldn't serve is refused with a 400.

A **CNAME** must be the only record at its name, and the apex can never be a CNAME. Both rules are enforced with a 409
or 400.

Each zone gets an **SOA** and an **NS** record automatically. The SOA's serial is the time of the zone's last change
in seconds, so it moves forward with every write. The NS record names `euclid.modules.eds.nameserver` unless you store
NS records at the apex yourself.

`get-answer` shows what the server would reply, without needing a DNS client:

```bash
euclid-cli eds get-answer --name www.corp.example --type A
```

## How names are answered

- A name with records of the type asked for gets them.
- A name with a CNAME gets the CNAME, and the lookup continues at its target while the target is in one of EDS's
  zones. The chain is followed for up to eight links; anything longer is answered SERVFAIL as a loop.
- A name that exists but has no records of that type gets NOERROR with an empty answer and the zone's SOA (NODATA).
  This also applies to a name with nothing of its own but records below it ("empty non-terminal", RFC 8020).
- A name that doesn't exist is checked against a wildcard directly below its closest existing ancestor (RFC 4592). If
  there isn't one, it gets NXDOMAIN with the SOA.
- A name inside zones nested in each other is answered from the most specific zone, so `lab.corp.example` wins over
  `corp.example` for names below it.
- ANY returns every set at the name. Zone transfers (AXFR/IXFR) are refused: the zones live in the database, and
  anyone who needs them can read them through the API with a credential.

Answers over UDP are limited to 512 bytes, or to the size the client advertises with EDNS0, capped at 1232. A larger
answer is sent truncated with TC set, which tells the client to ask again over TCP.

## Service names

These are rebuilt from the running system every `refresh-seconds` and served with a TTL of `service-ttl` (5 seconds by
default), because instances come and go with the autoscaler:

| Name | Type | Contains |
|------|------|----------|
| `manager.euclid.internal` | A/AAAA | the manager (`manager-address`) |
| `<node>.node.euclid.internal` | A/AAAA | each live worker node's registered address |
| `<application>.app.euclid.internal` | A/AAAA | every address a RUNNING instance runs at |
| `_http._tcp.<application>.app.euclid.internal` | SRV | each instance's port, with its node (or manager) as target |

Names are lowercased, and anything that isn't a letter, digit or hyphen becomes a hyphen, so `Image_Processing`
becomes `image-processing`. An instance assigned to a node that has stopped renewing its lease is left out, since it
can't be reached. A node registered under a host name rather than an IP address gets no address record.

The address is the node's, or the manager's for instances the manager runs. A port is only in the SRV record, so a
client that needs the port should look up the SRV.

Set `service-domain` to `""` to turn the service names off.

## Forwarding

A name in none of EDS's zones is forwarded only when all of these hold:

- the query asks for recursion (RD set),
- `forwarders` lists at least one upstream, and
- the client's address is in `allow-recursion`.

Otherwise it is answered REFUSED.

The default `allow-recursion` list is loopback plus the private ranges. Leave it that way unless you know why you
shouldn't. A forwarder that answers anybody is an open resolver, and open resolvers get used as traffic amplifiers
against third parties. Authoritative answers are given to every client regardless of this list.

Upstreams are tried in order, each for `forward-timeout-ms`. Each forwarded query uses its own socket on a fresh
ephemeral port, and a reply is only accepted if it comes from the upstream it was sent to and carries the query's ID.

```json
"forwarders": ["192.168.178.1", "1.1.1.1:53", "[2606:4700:4700::1111]:53"]
```

## Configuration

Under `euclid.modules.eds`:

| Key | Default | |
|-----|---------|-|
| `address` | `0.0.0.0` | address to bind; `::` for every interface |
| `port` | `1053` | UDP and TCP |
| `io-threads` | `2` | threads for the sockets |
| `worker-threads` | `8` | threads that answer; a forwarded query holds one while it waits |
| `service-domain` | `euclid.internal` | `""` turns the service names off |
| `manager-address` | (first local address) | what `manager.<domain>` answers |
| `nameserver` | `localhost` | SOA MNAME and default NS |
| `service-ttl` | `5` | TTL of the service names |
| `refresh-seconds` | `5` | how often the zones are reloaded |
| `forwarders` | `[]` | upstream resolvers |
| `allow-recursion` | loopback, RFC 1918, ULA | who may have names forwarded |
| `forward-timeout-ms` | `2000` | per upstream |
| `tcp-idle-seconds` | `10` | an idle TCP connection is closed after this |

Keep `minInstances` and `maxInstances` at 1. Two instances can't share the port, and the second would only log that it
couldn't bind.

**Why 1053, not 5353:** 5353 is the usual "unprivileged DNS" port, but it is also multicast DNS. Avahi and Bonjour
already hold it on most machines, and a socket bound beside them would be handed every mDNS packet on the network and
answer it.

## Putting it on port 53

euclid runs unprivileged, and binding a port below 1024 needs `CAP_NET_BIND_SERVICE`. You can either:

- give the manager the capability (its children inherit it) with a systemd drop-in, then set `"port": 53`:

  ```ini
  # /etc/systemd/system/euclid.service.d/dns.conf
  [Service]
  AmbientCapabilities=CAP_NET_BIND_SERVICE
  ```

  Port 53 is often taken by `systemd-resolved`'s stub on 127.0.0.53. Bind EDS to a specific address rather than
  `0.0.0.0` to avoid it.

- or leave EDS on 1053 and have the local resolver forward to it. With `systemd-resolved`:

  ```ini
  # /etc/systemd/resolved.conf.d/euclid.conf
  [Resolve]
  DNS=127.0.0.1:1053
  Domains=~euclid.internal ~corp.example
  ```

  This sends only those domains to EDS and leaves everything else as it was.

## How it works

A query never touches the database. EDS answers from a snapshot of every zone, built in memory and swapped as a
whole, which a background thread rebuilds every `refresh-seconds`. A write through the API rebuilds it immediately on
the instance that took the write.

If the database stops answering, the last snapshot stays in place: the zones go stale rather than disappearing. A
stored value that can no longer be encoded is skipped and logged once, instead of taking its zone down with it.

Zones and records are kept in `eds_zone` and `eds_record` and are included in EMM's backup. The service names aren't
stored anywhere.

DNS metrics are counted as `eds-query-count`, labelled by outcome (`NOERROR`, `NXDOMAIN`, `REFUSED`, `FORWARDED`,
`TRUNCATED`, ...). The API calls are counted as `eds-service-count`/`eds-service-time`, labelled by action.
