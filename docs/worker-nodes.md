# Worker nodes

**Status:** all six steps of §10 are built. Written 2026-09-24.

Built but **unproven**: all of it is covered by tests, and no worker has yet run an application
against a live installation. Treat the first one as an experiment on a host nothing depends on — the
things a test cannot reach are exactly the ones a first run finds, and one of them has already
turned up (see below).

**Found by running it rather than testing it.** `euclid-wrk` died with a stack-buffer-overrun
fast-fail the first time it was pointed at a gateway it could not verify: `CLI::HttpClient` throws
when it cannot reach an endpoint, and nothing in the worker caught it. That is the one failure the
lease exists to survive, and the crash would have left the worker's instances orphaned — running,
unsupervised, and holding a lease nobody would renew, which is precisely the state §5 is built to
make impossible. Every call a worker makes now goes through a POST that cannot throw, and an
unreachable master is status 0 rather than a terminated process. Worth remembering when reading the
rest of this: the tests were green through all of it.

Two things are deliberately missing, both from §11: running applications on a Windows worker
(`euclid-wrk` installs as a service and registers there, and refuses to start a process rather than
half-doing it), and a CLI for some of the operator actions — `assign-instance` and `drain-node` are
reachable over the API only. `list-nodes`, `get-node` and `delete-node` are `euclid-cli eap`
commands.

Step 4 is built but **unproven**: every part of it is covered by tests, and none of it has run
against a live installation. The lease arithmetic, the drain/renew race and the partition case are
pinned as decisions over their inputs — which is the only practical way to test the case that
matters — but a first real run will find things a test could not: the shape of a spawn that fails,
what a JVM does with the credentials file, how a renewal behaves across a gateway restart. Treat the
first worker as an experiment on a host nothing depends on.

Two things are deliberately missing, both out of §11: running applications on a Windows worker
(`euclid-wrk` installs as a service and registers there, and refuses to start a process rather than
half-doing it) and a CLI for some of the operator actions — `assign-instance` and `drain-node` are
reachable over the API only.

`ModuleInstance::host` exists, is written by every manager, and is honoured by the two operations
that were destructive across hosts — the start-up leftover sweep and the start-up clear. A backend
is now a host and a port: `Backends` resolves the recorded host when it refreshes and `ProxyServer`
connects to that address rather than to `127.0.0.1`.

An artifact is fetched through `esm:get-object` (or `create-download`/`download-part` when it is
larger than one call), by the manager as well — so the path a worker will take is the one that runs
today. The transport that makes that possible, `Core::ModuleClient`, moved out of the transfer
servers' library into core, which is the lowest layer the manager, the transfer servers and a future
worker share.

So the records can express a second machine, the gateway can reach one, and the bytes an application
needs no longer come off a local disk. What is still missing is anything that *puts* an application
there: the manager has no notion of a node, no placement, and no lease, and every manager still runs
every application whose desired state is RUNNING. Running more than one manager remains the wrong
thing to do — it is now merely non-destructive rather than safe. Steps 4 to 6 are what make it
useful.

One behaviour change worth knowing about, since §3.1 traded it away deliberately: an application
whose artifact is missing or whose build changed cannot start while ESM is down. It was previously
readable off ESM's data directory whether ESM was running or not.

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

A new executable, `euclid-wrk`, which:

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

**One thing step 3 could not finish, found when the worker came to need it.** The fetcher built
there (`Manager::Artifact`) speaks the right *protocol* — `get-object`, or
`create-download`/`download-part`/`complete-download` above one call — but over the wrong
*transport*: it resolves ESM's Unix socket out of the module repository and calls it directly, and a
worker has neither a module socket nor a database. The protocol is shared and the transport is not,
so the fetcher has to take the call as a parameter — a module socket for the manager, a signed
gateway request for the worker — rather than choosing one. That is a small change to a tested
component, and it belongs with the rest of 4c rather than being a surprise inside it.

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

Every node also carries an `os` label — `linux`, `windows` or `macos` — and an `arch` label —
`x86_64`, `aarch64`, `arm`, … — that the worker reports when it registers rather than reads from its
configuration, so a selector of `os=linux, arch=aarch64` cannot be satisfied by a mislabelled
machine. Both are needed for a native build: a Raspberry Pi and a PC are both `linux`. Configured
labels named `os` or `arch` are overridden.

### Naming one is also how an application opts in

As built, that constraint does more than narrow the candidates: **it is what makes an application a
node application at all.** One that names neither `nodes` nor `nodeLabels` is run by the manager on
its own host, exactly as it was before any of this existed, and never reaches placement.

This was not the original intent — §6 reads as though placement applies to everything — and the
reason for the change is worth recording. Making every application dual-mode means every existing
pool takes a new path through `reconcileApplications` on the strength of whether a worker happens to
be registered, and an installation with no workers (which is every installation today) would be
taking that new path to the same destination. Opting in per application makes adopting workers a
decision about one application rather than a property of the installation, which is also how anybody
would want to try the first one.

What it costs: placing an application takes an edit to its definition. If that turns out to be the
wrong trade, the condition is one function — `isNodeApplication` — and nothing else depends on the
distinction.

An application whose constraint is added while it is running locally has its local pool stopped
first. Leaving it would mean the application running both on the manager and wherever it gets
placed, which is the one outcome every part of this design exists to prevent.

The reverse holds too. `nodeLabels` is part of the application as `eap:list-applications` returns
it, and `eap:update-application` replaces it whole — `{}` clears it. An application whose
constraints are cleared is the manager's again: its slots on nodes are removed, each worker stops
its share on the next renewal, and the manager starts the pool itself.

## 7. The worker's side

Four actions, on EAP, because everything here is about applications. Each is an ordinary signed
request through the gateway, so the audit trail, the role concept and the permission vocabulary all
apply without a new mechanism.

| Action | Direction | What it does |
|---|---|---|
| `eap:register-node` | worker → master | announces the node: name, IP address, labels, cpu count, euclid version, operating system, architecture. Idempotent; a restarted worker re-registers. |
| `eap:renew-node` | worker → master | one call that renews every lease this node holds and answers with the node's current assignment — the desired set of `(instanceId, applicationId, revision)`. The heartbeat and the poll are the same call on purpose. |
| `eap:issue-instance-credentials` | worker → master | the credentials blob for one assigned instance. Refused for an instance not assigned to the caller. |
| `eap:report-node-instance` | worker → master | the instance's state, pid, host and port, written onto its record — what `spawnInstance` writes locally today. |

Plus, for operators: `eap:list-nodes` and `eap:get-node`, `eap:delete-node` (administrators only)
to free a node name for another principal, and `eap:drain-node` to stop placing on a node and let its
instances move off as they are replaced.

And `eap:assign-instance`, which this list missed. Step 4 has no placement — "the master is told by
hand" — and there has to be something that tells it. It gives one slot to a node with a lease on the
claim, or takes it back to the manager by naming no node. It is what placement will call in step 5
rather than something step 5 replaces.

### One thing §7 left open, and should not have

A worker announces a node name of its own choosing. On its own that means any principal holding
`eap:register-node` can register under a name another worker already uses — and then receive that
worker's instance assignments and, through `issue-instance-credentials`, the application credentials
that go with them. One worker reading another's secrets is a larger hole than anything the lease
protects against.

So the first registration of a name binds it to the principal that made it
(`Entity::EAP::Node::principal`), and every worker action checks the caller against it. Moving a node
name means deleting the registration first, which is a deliberate act and leaves the leases alone.

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
| 1 ✅ | `host` on `ModuleInstance`, empty meaning "here"; host check in `killLeftoverInstances` and everywhere a pid is read | the record can express a second host, with no behaviour change on a single one |
| 2 ✅ | `Backends` and `ProxyServer` carry host + port | the gateway can reach a backend that is not loopback — testable with a fake backend on a second address on the same machine |
| 3 ✅ | artifact by download instead of by filesystem, with the md5 cache; used by the manager too | one code path for fetching an artifact, exercised on the host where it is easy to debug |
| 4 ✅ | `euclid-wrk` with register/renew/report and the lease, no placement — split into 4a/4b/4c below, because one step turned out to be three | the loop, the lease, and the safety argument in §5 |
| 4a ✅ | the records: `Entity::EAP::Node`, `assignedTo` and `leaseExpiresAt` on `ModuleInstance`, the lease and liveness rules, node storage on the EAP repository | the lease arithmetic and the drain/renew race, without a worker to run them |
| 4b ✅ | the EAP actions: `register-node`, `renew-node`, `issue-instance-credentials`, `report-node-instance`, `list-nodes`, `drain-node`, plus `assign-instance` which "told by hand" needs and §7 does not name. No CLI yet. | a node can be registered, renewed and assigned to by hand |
| 4c ✅ | `euclid-wrk`: the decision rule (`Worker::Reconciler::Decide`), the tick, the four gateway calls, the credentials file, spawning and reaping. POSIX only — see §11. Never run against a live installation. | §5 as a decision over four inputs, including the partition case a live installation cannot easily be made to reproduce |
| 5 ✅ | placement: the rule (`Manager::Placement::Choose`), the node load average it reads, the application's `nodes`/`nodeLabels` constraints, placement when a slot is created (`reconcileNodeApplication`) and re-placement when a lease lapses (`reconcileNodeLeases`). `list-nodes` and `drain-node` landed with 4b. | §6 in order, that the answer does not depend on iteration order, and that an expired lease needs no "exclude the previous holder" branch |
| 6 ✅ | credentials issued rather than minted locally; the worker refuses to start if it finds the signing secret, and replaces each instance's credentials halfway through their life | the secret stays on the master |

Step 6 is last only because steps 4 and 5 can be tested with an application that never calls back
into euclid. It is not optional, and a worker must refuse to start without it.

As built, the refusal is on the *presence* of `euclid.modules.eam.jwt-secret` in the worker's
configuration, whatever its value — because the realistic way that key arrives on a worker host is
somebody copying the manager's configuration file, and that file holds a great deal more than this
one secret. The error says so, so the operator checks the rest of it too.

The refresh rule moved with the credentials, and had to: the worker wrote them once at start, which
meant every placed application stopped working one TTL after it started. It now replaces them
halfway through their life, on every tick, derived from the blob's own `expiresAt` rather than from a
configured TTL — the TTL is the master's setting, and a copy of it on the worker would only be a
second place for the two to disagree.

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
- **Running applications on a Windows worker.** The design has nothing POSIX-specific in it, but
  `spawnInstance` already has two implementations (`Controller.cpp:330` and `:419`) and the worker
  would need the same care. `WorkerClient::Apply` refuses on Windows and says so in the log rather
  than half-doing it.

  What *is* built there is everything around it: `euclid-wrk` runs as a Windows service, registers,
  renews its lease, reports and stops cleanly, and ships as an MSI
  (`dist/win32/msi/euclid-wrk.wxs`, `--install`/`--uninstall`/`--foreground` for a tree without a
  package). So the remaining gap is exactly one function, and a Windows host can be deployed and
  watched registering before anything is placed on it. Installing it on a host the master will
  actually place work on is still premature.

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

## 13. Proposal: every application on a worker

*Status: proposed, not started.*

### The problem

There are two ways an application gets run, and they are two implementations of the same thing.
The manager runs an application itself unless it names a node or a label (`isNodeApplication`), in
which case it is placed and a worker runs it. Both paths fetch the artifact, build the command
line, write the credentials file and spawn the process — separately.

The cost has been concrete. In one pass over the worker path, every defect found was the worker's
copy having drifted from the manager's: `PYTHON` and `NODEJS` started as `java -jar`, a `BINARY`
never made executable, a redeploy that went on running the old build, no working directory. And
the split itself needs code of its own: a hand-over in each direction (a local pool stopped when a
constraint is added, node slots removed when it is cleared), each a place for an application to
end up running twice.

There is also a security cost the worker was designed to avoid and the manager's path does not.
`euclid.service` runs as `euclid` with `ReadWritePaths=/usr/local/euclid`, and the manager drops no
privileges when it spawns — so an application it runs can read, and write, `euclid.json`: the
signing secret, the database credentials. §3.2 exists so that code euclid runs on somebody's
behalf never holds that secret. Today that holds on every host except the manager's.

### The proposal

The manager's host runs a worker too, and every application is run by a worker:

- **EMM** supervises euclid's own modules, and nothing else. The manager's application code goes.
- **EAP** holds the application definitions, places instances and grants leases — all of them.
- **`euclid-wrk`** runs applications, on every host that runs any, the manager's included.

`isNodeApplication` disappears with it. `nodes` and `nodeLabels` become constraints only: an
application naming neither may be placed on any node, the manager's host's among them. A single-host
installation is a manager and one worker on the same machine, and the worker path stops being the
one that is only exercised by installations with a second machine.

### What the worker has to do first

Moving an application onto the worker must not lose anything it has on the manager. Today the
worker lacks:

| Gap | On the manager today |
|---|---|
| Running applications on Windows (§11) | `spawnInstance`'s Windows implementation |
| An HTTP port per instance, and routing to it — the worker reports `httpPort: 0` | port allocation, `applicationEndpoints`, the gateway's backends |
| Supervision: readiness, restart on crash, `maxRestarts` | `ServiceController`'s module supervision |
| Application output on `app.<runtimeName>`, and `set-log-level` while it runs (§9) | captured and re-emitted by the manager |
| Zero-touch setup on the manager's host | nothing to set up |

The last one matters most for adoption: a single-host install must not need a worker installed and
logged in by hand. The server package would ship and start a local worker, provisioned with a
principal of its own at first start, running as its own user — never as `euclid`.

### What changes in behaviour

The lease applies to every application. An application on the manager's host stops when its worker
cannot reach the gateway for a lease period, where today it would keep running. In exchange it
survives a manager restart — today it is a child of the manager and goes with it.

### Phasing

| Step | Scope | Proves |
|---|---|---|
| 13.1 | One implementation of "start an application" in `euclidcore`, which both the manager and the worker link: the command line (`Worker::CommandLine`, today checked against `Runtime.h` by a test), artifact freshness, the exec bit, the credentials file. The manager's path uses it too. | the two paths cannot drift — useful even if nothing below happens |
| 13.2 | worker parity: the table above, Windows first | an application loses nothing by moving onto a worker |
| 13.3 | a local worker shipped and provisioned by the server package | a single-host install runs its applications through a worker with no setup |
| 13.4 | the split removed: unconstrained applications placed on any node, `isNodeApplication` and the manager's application code deleted; existing installations migrate through the hand-over that already stops a local pool | one path |

13.2 is the real work, and the step that decides whether the rest is worth doing.
