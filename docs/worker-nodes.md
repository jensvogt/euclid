# Worker nodes

**Status:** proposal. Nothing built. Written 2026-09-24.

A second kind of host that runs EAP applications but not euclid. The manager stays the only thing
that decides what runs and how much of it; a worker is the thing that carries the decision out on
hardware the manager does not own.

The aim is the smallest change that gets an application off the manager's host, not a cluster
scheduler. Everything euclid already does to run an application — the artifact, the credentials, the
environment, the port, the load report, the autoscaler — stays as it is. What changes is *where* the
process ends up, and five specific places in the code that currently cannot express "somewhere
else".

---

## 1. Where we are

One host runs everything. `euclid-mgr` supervises euclid's own modules and, on the same watchdog
tick, reconciles EAP applications (`Controller.cpp:1049`, called from `Controller.cpp:2179`). For
each application whose desired state is RUNNING it:

| Step | Code | Where the bytes are |
|---|---|---|
| finds the artifact | `materializeArtifact` (`Controller.cpp:753`) | reads **ESM's own data directory** off local disk |
| mints a token | `writeApplicationCredentials` (`Controller.cpp:902`) | HMAC over `HttpActionServer::JwtSecret()`, held by the manager |
| builds the environment | `applicationEnvironment` (`Controller.cpp:967`) | local paths, `EUCLID_CREDENTIALS_FILE` |
| picks a port | `claimHttpPort` (`Controller.cpp:268`) | a process-local `std::map` (`Controller.cpp:247`) |
| forks the process | `spawnInstance` (`Controller.cpp:330`) | this host |
| records the instance | `ModuleInstance` in the `emm` collection | `pid`, `socketPath`, `httpPort` — **no host** |

The autoscaler then reads each instance's self-reported load back out of those same records
(`reconcileApplicationLoad`, `Controller.cpp:1421`) and grows or shrinks the pool
(`Controller.cpp:2473`). The gateway reads the records to find backends (`Backends::refresh`,
`eag/src/Backends.cpp:17`) and proxies to them.

It works, and none of it is wrong. It is simply written throughout as though there were one machine,
because there was.

## 2. What a worker is

A new executable, `euclid-worker`, which:

- is **a euclid client**, not a euclid module. It authenticates through the gateway like any
  application does, signs with RFC 9421, and holds a role. It has no MongoDB credentials, no EMD, no
  Unix sockets anyone else connects to, and nothing listening that the internet can reach.
- runs a reconcile loop of its own: *what am I supposed to be running, what am I actually running,
  make the second match the first.* The same shape as `reconcileApplications()`, one host down.
- reports what it is running, and renews a lease on it.

And explicitly is **not**:

- a place euclid's own modules run. EQS, ESM, EAM and the rest stay on the manager's host. That is a
  much larger problem — they share a database, a gateway and a socket directory — and nothing here
  depends on solving it.
- a scheduler. It never decides how many instances exist, or that it should run one.
- a storage node. It holds cached artifacts and log files, nothing durable.

## 3. The five things that assume one host

Each of these has to change. They are listed separately because each is independently testable, and
because the fourth is the one that decides whether the whole thing is safe.

### 3.1 The artifact comes off ESM's filesystem

`materializeArtifact` resolves the object row, then reads the file out of
`euclid.modules.esm.data-dir` with `DirUtils::FindFilePath` (`Controller.cpp:770`). A worker has no
such directory.

A worker downloads instead, through `esm:get-object` on the artifact bucket, into the same
`applicationDir(runtimeName)` layout. The existing freshness rule carries over unchanged and is the
reason this is cheap: the local copy is re-fetched only when its content hash differs from the
object's `md5Sum` (`Controller.cpp:799`), so a worker that already ran this build downloads nothing.
Comparing content rather than size was already deliberate — a rebuild landing on the same byte count
would otherwise keep running the old build — and it matters more on a worker, where the copy is the
only copy.

Needs: `esm:get-object` on the artifact bucket, granted to the worker's principal.

### 3.2 The credentials token is minted locally

`writeApplicationCredentials` mints the application's bearer token itself, with
`Core::JwtUtils::CreateToken(application.userId, Core::HttpActionServer::JwtSecret(), ...)`
(`Controller.cpp:902`). The comment says why: it is the same HMAC a login would produce and the
manager already holds the secret.

**A worker must not hold that secret.** Anything holding it can mint a token for any principal in the
installation, including `admin`, offline and unloggably. Distributing it would make every worker host
a full compromise of the control plane.

So the master keeps minting and the worker receives. The worker asks for credentials for an instance
it has been assigned, gets back exactly the JSON blob that goes on disk today — token, `expiresAt`,
`userId`, `accountId`, `region`, `namespace`, `endpoint` — and writes it with the same
write-beside-and-rename that is already there (`Controller.cpp:938`), for the same reason: a process
reading the file must never see half of one.

The refresh rule moves with it. `credentialsNeedRefresh` replaces the file once less than half the
TTL is left (`Controller.cpp:949`), so the worker asks again on its own tick and an application
always has at least half a lifetime in hand. `euclid.modules.eap.credentials-ttl-seconds` is
unchanged, and so is the reason the credentials are a file rather than an environment variable.

### 3.3 The gateway connects to 127.0.0.1

`Backends` stores a bare `std::vector<int>` of ports (`Backends.cpp:19`) and `ProxyServer` builds the
endpoint as `asio::ip::make_address("127.0.0.1")` (`ProxyServer.cpp:910` and `:974`), sets
`Host: 127.0.0.1:<port>` (`:865`) and SNI `localhost` (`:905`).

A backend becomes a host and a port. `Backends::next` returns both; the endpoint is resolved from the
instance record rather than assumed.

The TLS note at `ProxyServer.cpp:105` becomes load-bearing rather than incidental: hostname checking
is deliberately absent because the connection is to loopback. Once it crosses a network that
reasoning expires, and the hop needs either real verification or a network that is itself trusted.
See §8.

### 3.4 An instance record does not say where it is

`ModuleInstance` carries `instanceId`, `pid`, `state`, `socketPath`, `httpPort` and the load fields —
and nothing about which machine any of it refers to. A `pid` from another host is not merely useless,
it is actively dangerous: `killLeftoverInstances` (`main.cpp:616`) matches a recorded pid against
`/proc/<pid>/exe` to decide whether to kill it, and its own comment says a pid alone is not proof.
Run two hosts against one collection and the manager will eventually find a local process wearing a
worker's pid.

Add to `ModuleInstance`:

| Field | Meaning |
|---|---|
| `host` | which node the instance is on. **Empty means the manager's own host**, so every existing record keeps its current meaning and no migration is needed. |
| `assignedTo` | which node has been told to run this slot, as distinct from where it *is* running |
| `leaseExpiresAt` | when the assignment stops being valid — see §5 |

Every place that reads a pid gains a host check. `killLeftoverInstances` skips records that name
another host; each worker sweeps its own on start-up, the same logic with a different filter.

### 3.5 Port allocation is per process

`claimHttpPort` walks a file-local `std::map<std::string, int>` looking for a free port in
`euclid.modules.eap.http-port-min`..`max` (`Controller.cpp:268`). Two hosts will both hand out 9000,
which is correct — they are different hosts — and harmless the moment the record says which host it
is. Each node allocates within its own range and nothing needs to coordinate. No change beyond §3.4.

## 4. Who decides what

| Decision | Who | Why not the other one |
|---|---|---|
| how many instances an application runs | **master** | the thresholds are global. `IsSaturated`, `IsWorking` and `InstancesForBacklog` read the whole pool; a worker evaluating them over its own share would make pool size a function of how many workers exist. |
| which node the next instance goes on | **master** | §6 |
| when to stop an instance | **master**, except on lease expiry | the least-recently-idle choice (`Controller.cpp:2447`) compares instances across the whole pool |
| fetching the artifact | worker | it is the one that needs the bytes |
| writing the credentials file | worker, from a token the master minted | §3.2 |
| forking and reaping the process | worker | it owns the process tree |
| capturing the application's output | worker | §9 |

The autoscaler does not change. It already works entirely off instance records in the database and
never touches a process itself: `reconcileApplicationLoad` reads `utilisation`, `backlog` and
`loadReportedAt` out of `Module.instances` and sets `desiredCount`; the scale pass turns that into
"add a slot" or "stop that one". A slot that is assigned to a worker rather than spawned locally is a
change to one branch, not to the policy.

The load report needs no change either. `eap report-load` already goes from the application straight
to its own instance record (`IEmmRepository::reportInstanceLoad`), through the gateway, which the
application reaches over the network whatever host it is on.

## 5. Assignment is a lease, and that is the whole safety argument

The obvious design — the master notices a worker has gone quiet and starts the instances somewhere
else — is the classic way to end up running two of something that must only run once. A worker that
has lost its connection has not necessarily lost its processes. The network broke; the JVM is still
consuming the queue.

So the assignment is a lease the worker renews, and the guarantee runs the other way:

- the master writes `assignedTo` and `leaseExpiresAt` on the slot.
- the worker renews on every tick. A renewal is also the heartbeat and the "what should I be
  running" poll — one call, so there is no way to be heartbeating and not reconciling.
- **a worker that cannot renew stops the instances itself**, before the lease expires. Not when it
  decides the master is gone; when its own lease runs out. That is a local clock against a local
  deadline, and needs nobody's agreement.
- the master re-places a slot only after `leaseExpiresAt` has passed, plus a margin for clock skew.

The property that buys: at any moment after expiry, either the worker has already stopped the
instances or the worker is not executing at all. Both are safe to re-place on top of. It costs an
outage window of one lease period for work that was on a partitioned worker, which is the right
trade for a queue consumer and an explicitly wrong one for anything that must never stop — see §11.

Lease period should be a multiple of the tick and configurable; something like a 10 s tick and a 45 s
lease matches `LoadFreshnessSeconds()` and gives three chances to renew before giving up.

## 6. Placement

The master picks a node when it creates a slot. Kept deliberately dull to begin with:

1. nodes whose lease is live and whose labels satisfy the application's constraints, if it has any
2. of those, the one running fewest instances of *this* application — spread beats pack, because the
   reason for a second instance is usually that the first is saturated
3. tie broken by the node's one-minute load average, which `SystemUtils::ReadLoadAverage()` already
   reports and EMO already collects, normalised by `cpuCount`
4. tie broken by node name, so the choice is deterministic and a test can assert it

No bin-packing, no resource requests, no affinity beyond labels. Those are real features and none of
them is needed to get an application onto a second machine; adding them later does not change
anything above.

One constraint is worth having from the start: an application may name `nodes: []` or a label
selector, because the reason to add a worker is often that a particular application needs a
particular machine — a GPU, a licence dongle, a network it can reach.

## 7. The worker's side

Four actions, on EAP, because everything here is about applications. Each is an ordinary signed
request through the gateway, so the audit trail, the role concept and the permission vocabulary all
apply without a new mechanism.

| Action | Direction | What it does |
|---|---|---|
| `eap:register-node` | worker → master | announces the node: name, labels, cpu count, euclid version. Idempotent; a restarted worker re-registers. |
| `eap:renew-node` | worker → master | one call that renews every lease this node holds and answers with the node's current assignment — the desired set of `(instanceId, applicationId, revision)`. The heartbeat and the poll are the same call on purpose. |
| `eap:issue-instance-credentials` | worker → master | the credentials blob for one assigned instance. Refused for an instance not assigned to the caller. |
| `eap:report-node-instance` | worker → master | the instance's state, pid, host and port, written onto its record — what `spawnInstance` writes locally today. |

Plus, for operators: `eap:list-nodes`, and `eap:drain-node` to stop placing on a node and let its
instances move off as they are replaced.

`report-load` is unchanged and is still sent by the **application**, not by the worker. The worker
does not know how busy an application is; that was the whole point of the application reporting it.

### Why polling rather than a pushed connection

A pushed command channel means the master holds a connection per worker, workers need inbound
reachability or a persistent socket, and both sides need reconnection logic. Polling means a worker
behind NAT works, a master restart needs no recovery at all, and the worker's loop is the same
reconcile-toward-desired-state shape that `reconcileApplications`, `reconcileTransferServers` and
`reconcileModuleSettings` already are. The cost is latency bounded by the tick, which for starting a
process is not a cost.

## 8. The network in between

Today every hop that carries a credential is either a Unix socket or loopback. That stops being true
here, in two places:

- **worker → gateway.** Already TLS, already signed. Nothing new; this is the path an application on
  a remote host would take anyway.
- **gateway → application instance** (§3.3). This is the one that changes character. The proxy
  currently skips hostname verification because it connects to 127.0.0.1, and that reasoning does not
  survive a network hop.

The honest options, in order of how much they ask for:

1. require the worker network to be trusted — a private subnet, a VPN, a WireGuard mesh — and leave
   the hop as it is. Cheapest, and it is what most single-tenant deployments of this shape actually
   do.
2. terminate TLS on the worker with a certificate the gateway verifies, issued by EKM, which already
   issues certificates (`ekm:create-certificate`).
3. mutual TLS, so the application also knows it is being called by the gateway.

This proposal assumes (1) for a first version and says so in the configuration, because pretending
otherwise would put a security claim in the documentation that the code does not make. (2) is the
natural follow-up and needs no design change above.

## 9. Logs

Application output is captured by the manager and re-emitted on channel `app.<runtimeName>`, which is
why an application's log lines currently appear in the manager's log with the manager's own pid.

On a worker, the worker captures them and writes them to its own log with the same channel name. It
does **not** ship them to the master. The installation already runs Fluent Bit against the log
directory and ships to Elasticsearch (`dist/observability/fluent-bit.conf`), and pointing the same
configuration at a worker's log directory gets a worker's application logs into the same index with
no euclid code involved at all. Relaying them through the master would add a second transport for
something that already has one, and would make log volume a control-plane concern.

Per-application log levels keep working: `applyApplicationLogLevel` (`Controller.cpp:1031`) reads
`Application::logLevel` on every reconcile, and the worker's reconcile does the same against the
definition it already fetches.

## 10. Phasing

Each step is independently useful and independently revertible.

| Step | Scope | Proves |
|---|---|---|
| 1 | `host` on `ModuleInstance`, empty meaning "here"; host check in `killLeftoverInstances` and everywhere a pid is read | the record can express a second host, with no behaviour change on a single one |
| 2 | `Backends` and `ProxyServer` carry host + port | the gateway can reach a backend that is not loopback — testable with a fake backend on a second address on the same machine |
| 3 | artifact by download instead of by filesystem, with the md5 cache; used by the manager too | one code path for fetching an artifact, exercised on the host where it is easy to debug |
| 4 | `euclid-worker` with register/renew/report and the lease, no placement — it runs what it is told, and the master is told by hand | the loop, the lease, and the safety argument in §5 |
| 5 | placement in the master; `eap:list-nodes`, `eap:drain-node` | end to end |
| 6 | credentials issued rather than minted locally | the secret stays on the master |

Step 6 is last only because steps 4 and 5 can be tested with an application that never calls back
into euclid. It is not optional, and a worker must refuse to start without it.

## 11. Not in scope

Said plainly, because each of these is a thing somebody will reasonably expect:

- **euclid's own modules on a worker.** Different problem, much larger.
- **more than one manager.** There is exactly one master. Making the master itself
  highly available is a separate proposal and needs an election, which this needs none of.
- **an application that must never have two instances running.** §5 buys "at most one after the lease
  expires", not "at most one always". Anything needing the stronger property needs fencing — a token
  the application presents on every write, refused once the lease is gone — and that is an
  application-visible contract, not something the worker can add underneath.
- **moving a running instance.** Instances are replaced, not migrated.
- **resource limits.** No cgroups, no memory caps. A worker that overcommits is an operator's problem
  until there is evidence it needs to be euclid's.
- **Windows workers.** The design has nothing POSIX-specific in it, but `spawnInstance` already has
  two implementations (`Controller.cpp:330` and `:419`) and the worker would need the same care. Not
  in a first version.

## 12. Open questions

1. **EAP or a new module?** The actions are in EAP above because a worker only ever runs EAP
   applications. If euclid's own modules are ever distributed, the instance-level parts of this
   belong to EMM, which already owns the `Module` collection. Splitting them later is a rename;
   guessing now is not obviously better.
2. **Does the worker need `esm:get-object` on every artifact bucket, or one bucket per installation?**
   The narrower grant is better and may not be expressible if applications keep artifacts in their own
   buckets.
3. **Clock skew.** The lease compares a worker's local clock against a deadline the master wrote. How
   much margin, and should the worker refuse to run at all if it finds itself too far from the
   master's clock?
4. **What does `emm list-modules` show** once instances are spread across hosts — one row per module
   as now, with a host column on each instance, or a node view beside it?
5. **Draining on shutdown.** A worker asked to stop should let long polls finish, the way the
   autoscaler's scale-down deliberately does not kill an instance mid-long-poll
   (`Controller.cpp:2497`). What is the bound?
