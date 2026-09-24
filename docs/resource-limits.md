# Resource limits per application

**Status:** proposal. Nothing built. Written 2026-09-24.

A CPU and a memory ceiling per application instance, so that one application cannot take a host down
and an operator can say in advance what each one is allowed to cost.

Listed as out of scope in [worker-nodes.md](worker-nodes.md) §11 — "a worker that overcommits is an
operator's problem until there is evidence it needs to be euclid's". This is that proposal. It stands
on its own: nothing here needs worker nodes, and a single-host installation gets all of it.

Every figure below was measured on the development host, which is where the case for this is easiest
to see: euclid's slice is holding **4,230 tasks and 21.3 GB** with no ceiling on any of it.

---

## 1. What a limit is for

Two different goals, and they want different defaults:

**Containment.** One application must not be able to take down the installation. Today it can: eight
applications and every euclid module run in one unbounded cgroup as one user, so a leak in any of them
ends with the kernel's OOM killer choosing a victim — and the kernel picks by badness score, not by
whose fault it was. The application that caused it is not reliably the one that dies.

**Predictability.** An operator deploying an application wants to say what it costs, and wants the
answer not to change because something else got busy. This is also what makes a host's capacity a
number somebody can plan with rather than discover.

Containment wants a hard kill. Predictability wants throttling, which degrades rather than fails.
Memory gets the first and CPU the second, for reasons that are physical rather than chosen — §4.

## 2. Where we are

There are no limits of any kind. An application is `fork()`ed at `Controller.cpp:451` and `execvp()`ed
at `Controller.cpp:493` with the manager's own resource context inherited wholesale. Between those two
calls the child sets its working directory, its environment and `setsid()`, and nothing else. No
`setrlimit`, no cgroup, no job object.

`Application` (`database/include/euclid/database/entity/eap/Application.h`) has `minInstances`,
`maxInstances`, `readyTimeoutMs` and `logLevel` — everything about *how many* and nothing about *how
big*.

The manager does notice an application dying: `onChildExit` logs `killed by signal N`
(`Controller.cpp:2123`) and the slot goes to `PENDING_RESTART` with backoff. An OOM kill is a SIGKILL,
so it is visible — and indistinguishable from the manager's own `ForceKill`, which is also a SIGKILL.
§5.

## 3. The mechanism

| | Memory | CPU | Verdict |
|---|---|---|---|
| **cgroup v2** | `memory.max` — hard, OOM-kills inside the group; `memory.high` — throttles first | `cpu.max` — quota/period, throttles | **This one.** |
| `setrlimit` | `RLIMIT_AS`/`RLIMIT_DATA` | `RLIMIT_CPU` | Wrong tool — see below |
| Windows job objects | `JOB_OBJECT_LIMIT_JOB_MEMORY` | `JOBOBJECT_CPU_RATE_CONTROL_INFORMATION` | Works, later |
| macOS | — | — | Nothing equivalent. Unsupported, said openly. |

Verified on this host: the unified hierarchy is mounted (`cgroup2fs`), the root has
`cpu memory io pids cpuset` among its controllers, and a service cgroup carries `memory.max`,
`memory.high`, `memory.swap.max`, `cpu.max`, `pids.max`, `memory.events` and `cpu.stat`.

### Why not setrlimit, which would be so much easier

It is four lines between `fork()` and `execvp()` and needs no privileges at all, so it deserves a
straight answer rather than being passed over.

- **`RLIMIT_AS` limits virtual address space, not resident memory.** A JVM reserves vastly more
  address space than it ever commits — the heap, the metaspace, code cache, thread stacks and mapped
  files are all reservations. Setting `RLIMIT_AS` to a figure an operator would recognise as "2 GB of
  memory" makes the JVM fail to start, and setting it high enough for the JVM to start makes it
  useless as a limit. Every application euclid runs today is a JVM.
- **`RLIMIT_CPU` is cumulative seconds, not a rate.** It does not cap how much CPU a process uses at
  once; it kills it once it has used that much CPU in total. Applied to a long-running service it is a
  timer to a SIGKILL. There is no rate limit in `setrlimit` at all.
- **The JVM does not read rlimits when sizing itself.** It reads cgroups. Which is the next section,
  and is the argument that settles it.

`RLIMIT_NOFILE` and `RLIMIT_NPROC` are a different matter and are worth having regardless — they cap
descriptor and thread leaks, which is a real class of runaway, and they cost nothing. §10.

### The JVM makes cgroups do the right thing for free

Measured on this host with JDK 25:

```
UseContainerSupport  = true            {product} {default}
MaxRAMPercentage     = 25.000000       {product} {default}
MaxHeapSize          = 32178700288     {ergonomic}     # 25% of 125 GB
```

`UseContainerSupport` is **on by default**: since JDK 10 for cgroup v1, and since JDK 15 for the
v2 hierarchy this host uses — so it covers every runtime euclid offers, and would not have covered
a JDK 11. When the JVM is in a cgroup with
`memory.max` set, it sizes `MaxHeapSize` from the *limit* rather than from the host's RAM,
automatically, with no flag from us. That single fact decides the mechanism: a cgroup limit is a limit
the application cooperates with, and an rlimit is one it walks into.

It also creates a trap, which §6 is about.

## 4. Memory kills, CPU throttles

Not a preference. Over-CPU is a queue — the work waits and arrives later. Over-memory is not: there is
nothing to wait for, the allocation either succeeds or the program has to cope with failure it almost
certainly does not handle. So:

- **`memory.max`** is the hard ceiling. Exceeding it OOM-kills a process *inside the group*, which is
  the whole point: the kernel kills the offender rather than choosing a victim by badness score across
  the host.
- **`memory.high`** is set below it, and is the interesting one. It throttles allocation and pushes
  reclaim before anything dies, so an application drifting toward its ceiling slows down and becomes
  visible instead of vanishing. Proposed default: `memory.high` at 90% of `memory.max`.
- **`memory.swap.max = 0`.** An application swapping is an application whose latency has become
  unrelated to its code, and it hides the condition this exists to surface.
- **`cpu.max`** is a quota per period. `cpuLimit = 1.5` becomes `150000 100000` — 150 ms of CPU per
  100 ms of wall clock, i.e. one and a half cores' worth, throttled and never killed.

## 5. An OOM kill must not look like a crash

This is the part that goes wrong if it is not designed, and it is worth more attention than the
limits themselves.

An application killed for exceeding `memory.max` dies by SIGKILL. The manager already handles that —
`PENDING_RESTART`, backoff, `maxRestarts` — which means the default behaviour of adding a memory limit
is **a restart loop that reads as a crashing application**. The operator sees `killed by signal 9`, no
stack trace, nothing in the application's own log because it never got to write one, and no indication
that euclid did it on purpose.

So:

1. **Read `memory.events`.** Its `oom_kill` counter is the discriminator between "killed for its
   memory limit" and "killed by anything else", including the manager's own `ForceKill`. Read the
   counter when a child exits by signal and compare it to the value at spawn.
2. **Say so.** A distinct log line and a distinct state — `OOM_KILLED`, not `CRASHED` — so it is
   greppable and shows up in `emm list-modules` and `eap get-application` as what it is.
3. **Count it separately from crashes.** An instance OOM-killed three times in a row is not a flaky
   process, it is an application whose limit is wrong, and restarting it a fourth time will not help.
   Proposed: an OOM kill consumes the restart budget at a higher rate, or trips its own threshold,
   after which the pool stops and says why rather than flapping.
4. **A metric.** `application-oom-kills`, counted per application, because the condition worth alerting
   on is the first one rather than the hundredth.

The same reasoning does not apply to CPU: throttling produces no event, kills nothing, and shows up as
the application reporting higher utilisation — which is exactly what the autoscaler is for. §7.

## 6. The JVM heap trap

`MaxRAMPercentage` defaults to **25**. Set `memoryLimitMb = 2048` on a Java application and the JVM
gives itself a 512 MB heap, then dies of `OutOfMemoryError` with three quarters of its allowance
untouched. The limit will have made the application fail in a way it did not fail before, which is the
worst possible first impression for a feature meant to increase reliability.

Three ways to handle it, in order of how much euclid presumes:

1. **Document it and do nothing.** The operator adds `-XX:MaxRAMPercentage=75` to the application's
   `arguments`. Honest, and guarantees somebody hits it first.
2. **Add the flag for `JAVA*` runtimes when a memory limit is set, unless the arguments already name
   one.** euclid knows the runtime — `Runtime::JAVA`, `JAVA21`, `JAVA25` — and knows it has just
   imposed a limit the JVM would otherwise read too conservatively. This is the useful one.
3. **Make it configurable** — `euclid.modules.eap.java-max-ram-percent`, default 75 — so an
   installation that disagrees can say so without editing every application.

Proposed: (2) with (3) as the knob, and a log line saying it was added. Silently injecting JVM flags is
the kind of help that turns into an afternoon of confusion, so it has to announce itself. An
application that names `-XX:MaxRAMPercentage` or `-Xmx` itself is left alone, on the general rule that
what the deployment says explicitly wins.

## 7. What the autoscaler does about it

Two limits and a scaler that multiplies instances need to agree on what they are counting.

**The limits are per instance, not per pool.** A pool-wide budget divided among instances would shrink
every instance as the pool grew, so scaling up would make each instance slower — the exact opposite of
what scaling is for, and a feedback loop: slower instances report higher utilisation, which adds
instances, which makes them slower. Per instance means an application's total cost is
`maxInstances × limit`, which is a number an operator can compute and an admission check can enforce
(§8).

**CPU throttling and the autoscaler cooperate, by accident and correctly.** A throttled instance takes
longer per unit of work and reports higher utilisation; `IsSaturated` sees it and the pool grows. That
is right — the application genuinely needs more CPU than one instance is allowed — and it converges at
`maxInstances`, where the operator's ceiling is doing its job.

**`memory.high` does the same thing, less obviously.** Allocation stalls show up as latency, which shows
up as utilisation. An application approaching its memory ceiling will therefore tend to scale out
before it scales into a wall, which is the behaviour anybody would want and is worth knowing is
happening for that reason rather than assuming it was designed.

**What does not work is scaling a pool that is limited in aggregate**, which is the third reason the
limits are per instance.

## 8. Admission

Once limits exist, `minInstances × memoryLimitMb` is a promise the host may not be able to keep. Two
checks, both cheap:

- **At deploy.** `create-application` and `update-application` refuse a limit that cannot be met — the
  sum of every application's `minInstances × memoryLimitMb` against the host's RAM less a reserve for
  euclid itself. Refused at deploy with a reason, in the house style of `ScaleRefusal` and
  `RedeployRefusal`: a pure function that says what is wrong and can be unit-tested without a host.
- **At scale-up.** The autoscaler declines to add an instance that will not fit, and says so rather
  than spawning one to be OOM-killed. This is a new reason for a pool to stop growing and has to be
  visible, or it looks like the autoscaler is broken.

Overcommit should be allowed but deliberate: `euclid.modules.eap.memory-overcommit-ratio`, default
`1.0`. Memory limits are ceilings rather than reservations, and an installation whose applications are
sized for their worst case will want to book more than it has.

## 9. What an operator sees

Limits are worthless without the numbers to set them from. `memory.current`, `memory.peak` and
`cpu.stat` (with `throttled_usec`) are already there per cgroup, and the existing metric path already
carries per-module gauges — `euclid-memory-usage-real-mb` and `euclid-cpu-usage` are pushed from
`HttpActionServer` (`core/src/HttpActionServer.cpp:143` and `:165`).

Per-application, per-instance, on the same path:

| Metric | From | Answers |
|---|---|---|
| `application-memory-mb` | `memory.current` | how much it uses |
| `application-memory-peak-mb` | `memory.peak` | what to set the limit to |
| `application-memory-limit-mb` | `memory.max` | what it is set to |
| `application-cpu-throttled-percent` | `cpu.stat` `throttled_usec` | whether the CPU limit is actually biting |
| `application-oom-kills` | `memory.events` `oom_kill` | §5 |

`memory.peak` is the one that makes this usable: it turns "what should this limit be?" from a guess
into a reading. Worth exposing before the limits themselves, so an operator can size them from a week
of observation rather than setting one and finding out.

## 10. The unit file is the only privileged change

The manager runs as `User=euclid` with `NoNewPrivileges=true` and `ProtectSystem=strict`
(`dist/linux/systemd/system/euclid.service`), and the comment there is explicit that nothing euclid
does needs root. That should stay true.

Verified on this host — euclid's cgroup is not writable by euclid:

```
drwxr-xr-x 2 root root  /sys/fs/cgroup/system.slice/euclid.service     # Delegate=no
drwxr-xr-x 6 vogje01 vogje01  /sys/fs/cgroup/.../user@1000.service     # delegated
```

`systemctl show euclid.service` confirms `Delegate=no`. One line fixes it:

```ini
Delegate=memory cpu pids
```

systemd then chowns the unit's cgroup subtree to `User=`, and euclid can create a subgroup per
application instance and write `memory.max` and `cpu.max` inside its own slice — and **only** inside
it. No root, no `AmbientCapabilities`, no setuid helper, no relaxing of `NoNewPrivileges` or
`ProtectSystem`. The blast radius is exactly the subtree systemd handed over.

The proposed layout mirrors the pool structure, so a human reading `/sys/fs/cgroup` sees what euclid
sees:

```
euclid.service/
  applications/
    parser/              # per application: nothing set, a place to read totals
      <instanceId>/      # per instance: memory.max, memory.high, cpu.max, pids.max
```

The child joins its own group by writing its pid to `cgroup.procs` between `fork()` and `execvp()`, at
`Controller.cpp:458`. That is one write to an already-open descriptor and is async-signal-safe, which
matters because everything between `fork()` and `exec()` in a multithreaded process does.

Two consequences worth stating rather than discovering:

- **An installation not started by systemd gets no limits.** Run by hand, in a container, or under
  another init, there is no delegated subtree. Limits must degrade to "not applied, and said so once"
  rather than refusing to start — otherwise this breaks every development setup.
- **cgroup v1, or a host with `memory` not delegable, is the same case.** Detected at start-up,
  logged once, and `eap get-application` reports the limit as configured-but-not-enforced. A limit
  silently not being applied is the one outcome worse than having none.

## 11. Where it lives

Two fields on `Application`:

| Field | Type | Meaning |
|---|---|---|
| `cpuLimit` | `double`, cores | `1.5` = one and a half cores per instance. `0` = unlimited. |
| `memoryLimitMb` | `long` | per instance. `0` = unlimited. |

`0` meaning unlimited keeps every existing application unchanged with no migration, the same way an
empty `host` does in [worker-nodes.md](worker-nodes.md) §3.4.

Surface, following what `scale-application` established:

- `eap:set-application-limits`, writing only those two fields by hand rather than through
  `upsertApplication` — which stamps `modified` and makes the manager restart the pool. Changing a
  limit *should* eventually restart instances, because a cgroup's limits can be changed live but the
  JVM's heap sizing cannot; but that should be a deliberate rolling restart, not a side effect of
  saving the row.
- `--cpu-limit` and `--memory-limit` on `create-application` and `update-application`.
- the CLI, all four SDKs, and a man page.

A pure `LimitRefusal(cpuLimit, memoryLimitMb, hostCores, hostMemoryMb, reserved)` for §8, testable
without a host, beside `ScaleRefusal` and `RedeployRefusal`.

## 12. Phasing

| Step | Scope | Proves |
|---|---|---|
| 1 | `memory.peak`/`memory.current`/`cpu.stat` read and published as metrics; no limits, no `Delegate=` — reading a cgroup needs no delegation | operators can size limits from observation before any limit can hurt them |
| 2 | `Delegate=memory cpu pids`; the subgroup tree; detection and the "not enforced" path | the privileged change, on its own, with nothing depending on it yet |
| 3 | `cpuLimit` only, throttling, no kills | the plumbing, with a failure mode that cannot lose data |
| 4 | `memoryLimitMb` with `memory.high`, `memory.swap.max=0`, the `oom_kill` discriminator and `OOM_KILLED` | §5 before anything can be killed silently |
| 5 | JVM `MaxRAMPercentage` injection for `JAVA*` runtimes | §6 |
| 6 | admission at deploy and at scale-up | §8 |
| 7 | `RLIMIT_NOFILE`/`RLIMIT_NPROC`, which need no cgroups at all | cheap, and independent of everything above |

Step 3 before step 4 deliberately: a CPU limit set too low makes an application slow, and a memory
limit set too low makes it die. The reversible one should be the one that finds the bugs.

## 13. Not in scope

- **Disk and network limits.** `io.max` needs the block device and a per-application I/O budget nobody
  has asked for; network shaping needs `tc` and is genuinely privileged.
- **cpuset pinning.** `cpuset.cpus` is there, and pinning is a NUMA and latency tool rather than a
  containment one. Different problem.
- **Limits on euclid's own modules.** The same mechanism would apply, and the risk is different: a
  module OOM-killed for its limit takes out part of the control plane. Worth doing, and worth doing
  after applications have proved the machinery.
- **Windows and macOS.** Windows is a job object per instance and is a real follow-up. macOS has no
  equivalent and should report limits as not enforced rather than pretending.
- **Reservations.** These are ceilings. Nothing guarantees an application *gets* its memory, only that
  it may not exceed it. Guarantees need `memory.min`, and want a scheduler that understands them.

## 14. Open questions

1. **Should a limit change restart the pool?** A cgroup's `memory.max` can be raised live and the
   application benefits immediately; a JVM's heap cannot be resized, so the benefit is partial until a
   restart. Raise live and restart lazily, or restart on change and be predictable?
2. **`memory.high` at 90% — of what evidence?** It is a guess. Step 1 of §12 exists partly to replace
   it with a reading.
3. **What is euclid's own reserve** in the admission check? The manager, thirteen modules and the
   gateway are currently 21.3 GB on this host, and nothing measures how much of that is the floor.
4. **Does an OOM-killed instance count against the autoscaler's desired count** while it is being
   restarted, or should the pool grow to cover it? Growing may multiply the problem if the limit is
   simply too low.
5. **Per-instance or per-application cgroup for the limit?** Per instance above. A per-application
   group with the limit set there would cap the pool in aggregate, which §7 argues against — but it is
   the thing an operator asking "what may this application cost?" probably means, and the two answers
   should at least be presented together.
