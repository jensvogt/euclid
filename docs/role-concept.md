# Role concept

**Status:** built. Steps 1–6 complete in euclid (§9); the client SDKs still carry the removed
`AccountGrant` field. §10 is the operator's procedure. Written 2026-09-12.

A model for granting rights per module action — `ens:create-topic`, `ens:publish-message`,
`ens:subscribe` — to users and to user groups, replacing the three overlapping mechanisms
authorization is spread across today.

---

## 1. Where we are

Authorization is currently three things that do not compose:

| Mechanism | Where | What it decides |
|---|---|---|
| `administrator` group membership | `Database::IsCachedEamAdmin()` | Everything. A global yes. |
| `User::accountGrants` — `{accountId, namespaces[], isAdmin}` | `HttpActionServer::Authenticate` via `WireGrantLookup` | **Where** a caller may work |
| `User::resourceGrants` — a list of ERNs | `HttpActionServer::IsResourceAllowed`, called by hand from some handlers | **Which** resources, inside that account |

Three problems, in order of how much they cost:

**It is permissive by default.** `resourceGrants` documents its own empty case as "unrestricted,
which is what every human and every user written before this field existed is". So the only thing
standing between a logged-in user and a module action is whether some handler remembered to ask.

**The asking is per handler.** `denyUngrantedQueue` in EQS, `denyUngrantedBucket` in ESM — each
handler that cares calls one, and a handler that does not call one is simply open. That is not
hypothetical: ESM's `delete-bucket` had no resource check at all until it was noticed on
2026-09-03. A gap in this design is silent by construction; nothing fails, nothing logs, the call
just succeeds.

**There is nothing between "may work in this account" and "administrator".** A user who may publish
to a topic may also delete every topic in the namespace, purge every queue, and read every secret.
There is no way to express "this service account may publish, and nothing else" — which is the thing
being asked for.

The surface this has to cover, counted from the modules' own dispatch tables:

| Module | Actions | | Module | Actions |
|---|---:|---|---|---:|
| esm | 37 | | ekv | 10 |
| eqs | 28 | | eap | 10 |
| eam | 27 | | emm | 9 |
| ens | 22 | | ess | 8 |
| ekm | 14 | | ets | 8 |
| | | | ees | 6 |
| | | | emo | 3 |

About 180 actions. That is the permission vocabulary, and it already exists — which is the first
design decision below.

---

## 2. Goals

- **Granular per action.** `ens:publish-message` grantable without `ens:delete-topic`.
- **Grantable to users and to user groups.** Groups are how this stays manageable; a user's rights
  are the union of their own bindings and those of every group they are in.
- **Deny by default**, once migration is complete. An action nobody granted is refused.
- **One gate, not 180 call sites.** A handler must not be able to be open by forgetting something.
- **Scoped the way euclid already is** — account, namespace, and where it matters, resource.
- **Migratable without a flag day.** A running installation must be able to move to this without
  everybody losing access on upgrade.

### Non-goals, deliberately

- **No deny rules.** Roles grant; nothing subtracts. Deny rules turn "what may this user do" into a
  question you have to *evaluate* rather than *read*, and nothing here needs them yet. If a case
  turns up, it can be added later — adding a deny to a union model is possible, removing one is not.
- **No hierarchy or inheritance between roles.** A role is a flat set of permissions. Composition is
  by binding two roles, not by subclassing one.
- **No nesting of user groups.** A group holds users, never other groups, which is what it does
  today. Nesting would turn "why can they do that" from a lookup into a graph walk, and make
  `check-permission` answer with a path rather than a binding.
- **No per-field or per-row rules.** Resource granularity stops at the ERN.

---

## 3. The model

### 3.1 Permissions are `<module>:<action>`

Exactly the pair already on the wire — `x-euclid-target` and `x-euclid-action`:

```
ens:create-topic
ens:publish-message
ens:subscribe
eqs:receive-messages
esm:put-object
```

This matters more than it looks. The permission vocabulary is **derived, not invented**: it is the
dispatch tables, which means a new action cannot be forgotten in a permission list, and a
permission cannot be granted for an action that does not exist. A build-time generator can emit the
canonical list from `modules/*/src/*Server.cpp` and a test can assert the two agree.

Two wildcards, and no more:

```
ens:*        every ENS action
*:*          every action of every bindable module
```

Read/write/admin groupings are expressed as **built-in roles** (§3.4), not as a second wildcard
syntax. `ens:read-*` looks helpful right up to the point where someone has to decide whether
`get-message-count` is a read.

**Two modules have no permissions at all.** `emm`'s nine actions expose every module's live process
pool and raw collections; `emd` *is* the document store, and dispatches `find-one`, `replace-one`,
`delete` and the rest against any collection by name — a caller who held `emd:replace-one` would
rewrite anything, past every check the owning module makes. Neither is in the vocabulary, and `*:*`
does not reach them. EMM is installation-administrator only (§4.1); EMD is reached by the `system`
principal (§4.2).

A permission nobody can bind would be a lie in the listing; leaving them out means
`list-permissions` describes exactly what can be granted, and `Permissions::UnbindableModules()` is
the one place that says why anything is missing.

### 3.2 A role is a named set of permissions

```json
{
  "name": "topic-publisher",
  "accountId": "000000000000",
  "description": "May publish to topics, and see which exist",
  "permissions": ["ens:publish-message", "ens:list-topics", "ens:get-topic-ern"],
  "ern": "ern:eam:eu-central-1:000000000000::role/topic-publisher"
}
```

**Roles are account-scoped, like namespaces.** A role belongs to exactly one account, and a binding
can only name a role of the account it grants in. Two accounts may each have a `topic-publisher`
meaning different things, and neither can see the other's — the same rule a topic name already
follows.

Built-in roles (§3.4) are the one exception, and are deliberately *not* stored per account. They are
defined once, in the same generated table as the permission vocabulary, and referenced by name from
any account. Copying seven roles into every account on creation would mean every account's
`operator` goes stale the day a module gains an action, and an installation with fifty accounts
would need a migration to fix each one. They are readable everywhere, writable nowhere.

### 3.3 A binding is a role, granted somewhere, to someone

```json
{
  "role": "ern:eam:eu-central-1:000000000000::role/topic-publisher",
  "principal": "ern:eam:eu-central-1:000000000000:user/order-service",
  "accountId": "000000000000",
  "namespaces": ["production"],
  "resources": ["ern:ens:eu-central-1:000000000000:production:topic:order-*"],
  "granted": "2026-09-12T10:00:00Z",
  "grantedBy": "jens"
}
```

- **`principal`** is a user ERN or a user-group ERN. One field, two kinds — the ERN says which.
- **`namespaces`** — `["*"]` for every namespace of the account.
- **`resources`** — ERN glob patterns; `["*"]` for all. This is where `resourceGrants` goes, but
  attached to a binding rather than to the user, so "this application may put objects into these two
  buckets" is one record instead of a permission set and a resource list that have to be kept in
  step by hand.

A binding is the *only* thing that grants, and it is the only thing that carries scope. Both of the
fields it replaces — `User::accountGrants` and `User::resourceGrants` — are **removed** rather than
kept alongside it (§5); a user's rights are its bindings and nothing else.

### 3.4 Built-in roles

Nobody should have to write out 189 permissions to get started.

| Role | Roughly |
|---|---|
| `account-administrator` | `*:*` within one account. Today's `AccountGrant.isAdmin`. Note that `*:*` does not include `emm`. |
| `operator` | Every action except `delete-*`, `purge-*` and `eam:*`. The on-call role. |
| `reader` | `list-*`, `get-*`, `describe-*` across all modules. |
| `publisher` | `ens:publish-message`, `eqs:send-message`, plus the `get-*-ern` lookups needed to address them. |
| `consumer` | `eqs:receive-messages`, `eqs:delete-message`, `eqs:set-visibility`, `ens:subscribe`, `ens:unsubscribe`. |
| `application` | What EAP hands a deployed application: `publisher` + `consumer` + `esm:get-object`/`esm:put-object`, always bound with an explicit `resources` list. This is what `resourceGrants` was reaching for. |
| `transfer` | Everything an FTP or SFTP client can do: the seven `ets:` transfer permissions of §4.3. Not `ets:start-server` and friends — a client that may upload must not be able to stop the server it uploads to. |

The first three are *computed* from the vocabulary by rule, so a module that gains an action gains
it in them on the next build. The last four are short lists, checked entry by entry against the
vocabulary by `BuiltinRoleTest`. `transfer` is a list because "everything a transfer client can do"
is not a shape the vocabulary has — and because the point of granting something narrower is to
grant fewer than all seven.

There is deliberately **no `administrator` role**. Installation administration is not a role binding
— it stays exactly what it is today, membership in the `administrator` user group, bootstrapped in
`modules/eam/src/main.cpp`. Two reasons: roles are per account, and an installation administrator is
by definition not, so expressing one would need a magic account to hold it; and EMM has to be
reachable by *something*, which with no `emm` permissions in the vocabulary can only be the group.
`account-administrator` is the strongest thing a role can say.

---

## 4. How a request is decided

```
allow(principal, target, action, namespace, resourceErn):

    if target == "emm":
        return isInstallationAdministrator(principal)      # group membership, nothing else

    if isInstallationAdministrator(principal):  ALLOW
    if principal is the system principal:       ALLOW      # §4.2

    bindings := bindingsOf(principal) ∪ ⋃ bindingsOf(group) for each group they are in

    for each binding b in bindings:
        if b.accountId ≠ request.accountId:                          skip
        if namespace ∉ b.namespaces and "*" ∉ b.namespaces:          skip
        if resourceErn is known and matches nothing in b.resources:  skip
        if "<target>:<action>" ∈ permissionsOf(b.role):              ALLOW

    DENY
```

Union, no ordering, no precedence — which is what makes "why can this user do that" answerable by
reading rather than by simulating. Note there is no `b.accountId == "*"` case any more: roles and
bindings are per account, so the only cross-account principals are the two short-circuits above, and
both are visible on the first three lines rather than hidden in a wildcard.

### 4.1 Two enforcement points, because there have to be

The module and action are in the headers; the resource is in the body, and only the handler knows
which field names it. So:

**The gate** — `HttpActionServer` checks `<target>:<action>` plus account and namespace scope
**before** `Dispatch` is called. Today `UnixSocketServer::Session::doRead` calls
`_server.Dispatch(req)`; making `HttpActionServer::Dispatch` final, authorizing there and calling a
new `DispatchAction` for subclasses means every module gets the check without any module opting in.
A new action is refused until somebody grants it — the failure mode is a 403, not silence.

**The resource check** — stays in the handler, because it cannot be anywhere else, but changes
shape. Instead of `IsResourceAllowed` returning "unrestricted" for an empty list, a registry says
which actions are resource-scoped:

```cpp
// One place, checkable by a test rather than by remembering.
RESOURCE_SCOPED = {
    {"esm", {"delete-bucket", "put-object", "get-object", ...}},
    {"ens", {"publish-message", "delete-topic", ...}},
};
```

The gate refuses any action in that registry whose handler did not call `AuthorizeResource(...)`
before answering 2xx. In debug builds that is an assertion; in release it is a log line and a
metric. A handler that forgets is then *noisy* rather than *open* — which is the single most
important difference from today.

### 4.2 The `system` principal

euclid calls itself: EAP starts an application, ESM notifies an ENS topic when an object lands, the
gateway forwards to a module. Those requests authenticate as the module rather than as a user, and
they cross accounts by design — an ESM bucket in one account notifying a topic in another is a
normal subscription.

Rather than exempting that traffic from the gate, it gets a **reserved principal**,
`ern:eam:<region>::system`, which:

- is allowed everything except `emm`, by the short-circuit above;
- cannot be created, bound, revoked or logged in as — it is not a row in `eam_user`, it is a
  constant, so there is no credential to leak and nothing an administrator can accidentally grant;
- is authenticated the way inter-module traffic already is, and by nothing else;
- is **logged on every use**, with target, action and resource.

The point is not that it is restricted — it is not — but that it is *nameable*. Today this traffic
is indistinguishable from a user's in the logs, and the honest answer to "did a person do this or
did euclid?" is that nobody knows. A reserved principal makes the internal path one line in a log
and one entry in `check-permission`'s answer instead of an unexplained allow.

### 4.3 FTP and SFTP are a third enforcement point

The two enforcement points above are both about HTTP requests on a module socket. A transfer server
speaks neither: an FTP verb arrives on a control connection and an SFTP request as a packet, and
neither reaches `Core::HttpActionServer` at all. Yet these are the callers most in need of being
restricted — an external supplier with an FTP password is the least trusted principal an
installation has.

So `euclid-ftp` and `euclid-sftp` check a permission themselves, before running each command, via
`Transfer::TransferAuthorizer` — the same `Database::Authorization::Allows()`, the same grants, the
same roles.

| Permission | FTP | SFTP |
|---|---|---|
| `ets:list-directory` | LIST, NLST, CWD, CDUP, SIZE, MDTM | OPENDIR, STAT, LSTAT |
| `ets:get-file` | RETR | OPEN for reading |
| `ets:put-file` | STOR | OPEN for writing, SETSTAT, FSETSTAT |
| `ets:rename-file` | — (no RNFR/RNTO) | RENAME |
| `ets:delete-file` | DELE | REMOVE |
| `ets:create-directory` | MKD | MKDIR |
| `ets:delete-directory` | RMD | RMDIR |

Four things decided here that are worth stating, because each could reasonably have gone the other
way:

- **They are `ets:`, not a module of their own.** They belong to the module whose servers they
  are, so one grant mechanism covers them and `list-permissions` is still the whole vocabulary.
  `PermissionVocabularyTest` derives them from the two session sources — `permitted("get-file")`
  literals — exactly as it derives the rest from the dispatch tables, and asserts the two `ets:`
  sets do not overlap.
- **The resource is the transfer server's ERN**, so a grant can be narrowed to one server.
  Deliberately *not* the path: a client is already confined to its home prefix, and a second
  path-shaped access model would be one too many to reason about.
- **Handles are checked once.** SFTP READ/WRITE/FSTAT/READDIR/CLOSE are not re-checked; the
  OPEN or OPENDIR that produced the handle is what decided it. Otherwise a download costs one
  evaluation per 32 KB block.
- **The client is never told why.** The reason names roles and grants; it goes to the log. The
  client gets `550 Permission denied` or `SSH_FX_PERMISSION_DENIED`.

ESM's own grants still apply underneath all of this — every call a session makes carries the
client's own token. But they apply to the *bucket*, and there is no ESM permission meaning "may
upload but not delete", because an overwrite and a delete both go through `esm:delete-object`. The
distinction a transfer server needs is between commands, so it is drawn where the commands are.

**This is a breaking change for any installation already running a transfer server.** Being listed
in a server's `userIds`/`userGroups` used to be the whole of authorization; now it only admits the
client. Grant the `transfer` role to the same users or groups the server already names:

```
euclid-cli eam grant-role --role transfer \
    --principal ern:eam:eu-central-1:000000000000:userGroup:suppliers \
    --namespace production
```

Many users need nothing: `reader` covers `ets:list-directory` and `ets:get-file` by the `get-`/
`list-` rule, and `operator` covers everything but the two `delete-`s.

---

## 5. Migration

This is the part that decides whether the concept is usable, because deny-by-default applied to an
installation built on permissive-by-default locks out everyone at once.

A setting, `euclid.authorization.mode`, with three values:

| Mode | Behaviour |
|---|---|
| `legacy` | Today exactly. The gate **evaluates nothing at all** — no lookup, no grants read. The default on upgrade. |
| `shadow` | Today's answer is what happens. The role answer is **computed and logged** whenever the two differ, with the user, the action and the binding that was missing. Nothing is refused. |
| `enforce` | The role answer is what happens. |

Originally `legacy` was going to evaluate and ignore the answer. It does not: it is what every
installation upgrades *into*, so it has to cost what it cost before roles existed — no grant store
read, no signature verified a second time. Evaluation starts at `shadow`, which is the mode somebody
turns on deliberately.

`shadow` is the point of the exercise. An operator runs it for a week, reads
`eam:authorization-gaps` — which is a listing of what *would* have been refused, grouped by user and
action — and grants what is genuinely needed before flipping. Nobody has to guess the binding list
from first principles, which nobody can do for a live system.

A one-off `eam:migrate-grants-to-roles` seeds the obvious translation:

- `administrator` group member → nothing to do; the group stays as it is and keeps meaning what it means
- `AccountGrant{isAdmin: true}` → binding of `account-administrator` at that account
- `AccountGrant{isAdmin: false, namespaces}` → binding of `operator` at that account, those namespaces
- non-empty `resourceGrants` → binding of `application`, with those ERNs as `resources`
- empty `resourceGrants` — today's "unrestricted" — → nothing. This is exactly the population
  `shadow` exists to size, and translating it to a wildcard binding would carry the permissive
  default into the new model and defeat the whole exercise.

That reproduces current behaviour closely enough to start, and `shadow` finds the rest.

### 5.1 Dropping the old fields is a wire break

`User::accountGrants` and `User::resourceGrants` are removed once an installation reaches `enforce`,
and that is not an internal change: `accountGrants` is a documented field of the `User` DTO in
euclid-cli and in all four SDKs, and two EAM actions exist only to write it.

| What goes | Replaced by |
|---|---|
| `User.accountGrants[]` on every `list-users` / `register` response | `list-grants` |
| `User.resourceGrants[]` | `list-grants`, with `resources` |
| `eam:grant-namespace-access` | `eam:grant-role` |
| `eam:revoke-namespace-access` | `eam:revoke-role` |

So the order has to be: ship roles **alongside** the old fields, migrate, run `shadow`, flip to
`enforce`, and only then remove the fields in a release that says so. Clients that read
`accountGrants` — euclid-rui shows them, and `AccountGrant` is exported from euclid-jdk, euclid-pdk,
euclid-ndk and euclid-cdk — need a release each in between. Removing the fields in the same release
that introduces roles would break every client at once, for no gain.

`User.isAdmin` and the credentials file's `isAdmin` are **unaffected**: both mean installation
administrator, which stays group membership.

---

## 6. API

New EAM actions, in the shape the module already uses:

| Action | |
|---|---|
| `create-role` | name, description, permissions |
| `list-roles` | paged, prefix, includes the built-ins |
| `get-role` | one role, its permissions expanded |
| `update-role` | replace the permission set |
| `delete-role` | refused while any binding references it |
| `grant-role` | role + principal + account/namespaces/resources |
| `revoke-role` | the binding |
| `list-grants` | by principal, or by role — "who can do this" and "what can they do" |
| `list-permissions` | the canonical vocabulary, generated from the dispatch tables. `emm` is absent |
| `check-permission` | would *this* user be allowed *that*? The answer, plus which binding decided |
| `authorization-gaps` | what `shadow` mode has been recording |

And two retired, once the fields go (§5.1): `grant-namespace-access` and `revoke-namespace-access`.
A namespace grant is now a role binding scoped to that namespace, which is the same statement with
the "what may they do there" part filled in rather than assumed.

`check-permission` earns its place: the first question asked about any permission system is "why can
they / why can't they", and a system that cannot answer it gets worked around by making everybody an
administrator.

CLI follows one-for-one — `euclid-cli eam grant-role --role topic-publisher --user order-service
--namespace production --resource 'ern:...:topic:order-*'`.

---

## 7. What this costs

- **A lookup per request.** Bindings and roles are cached the way the administrator group already is
  (`TtlCache` in `RepositoryFactory`), invalidated on grant/revoke. The gate runs on data already in
  memory.
- **Two new collections** — `eam_role`, `eam_grant` — plus `emm` export/import entries for both, or
  a backup restores an installation nobody can log into.
- **A generator and a test** keeping the permission vocabulary in step with the dispatch tables.
- **The SDKs** gain the EAM actions; the four of them have wrapped every EAM action so far.
- **A release each in five client repos** to stop reading `accountGrants` before it is removed —
  euclid-cli, euclid-rui, euclid-jdk, euclid-pdk, euclid-ndk, euclid-cdk. This is the largest single
  cost of the design and it is a consequence of §5.1, not of roles themselves.

---

## 8. Decisions

Settled 2026-09-12:

| | |
|---|---|
| **Roles span accounts?** | **No.** A role belongs to one account. Built-ins are defined once and referenced by name, not copied per account (§3.2). |
| **`accountGrants` / `resourceGrants`** | **Dropped**, not kept as a derived wire format. Staged as in §5.1, because it breaks five clients. |
| **`emm`** | **Not bindable** — and `emd` with it, which the vocabulary test found on its first run. Neither module's actions are in the vocabulary; EMM stays installation-administrator only, EMD is reached by the `system` principal (§3.1, §4). |
| **Inter-module traffic** | **A reserved `system` principal** — allowed everything but `emm`, un-bindable, un-revocable, logged on every use (§4.2). |
| **Nested user groups** | **No.** Groups stay flat: a group holds users, never groups (§2). |

Consequence worth noting: with roles per account and `emm` unbindable, there is no longer any role
that spans an installation, so the built-in `administrator` role proposed earlier is gone. The
`administrator` user group does that job and keeps doing it.

Nothing is open. What the model says a user may do is: the union of the bindings held by that user
and by each flat group they belong to, scoped to one account, its namespaces and its resource ERNs —
plus two named short-circuits, the `administrator` group and the `system` principal, both of which
are one line each in §4.

---

## 9. Building it

The order that keeps an installation working throughout:

1. **Vocabulary first.** ✅ **Done** — `Core::Permissions` (`core/include/euclid/core/Permissions.h`),
   182 permissions, held to the dispatch tables by `tests/PermissionVocabularyTest.cpp`. It is the
   thing everything else is written against, and it is independently useful — it is also a list of
   every action euclid has, which nothing produced before.
2. **Storage and API.** `eam_role`, `eam_grant`, the eleven EAM actions, the CLI, the `emm`
   export/import entries. Nothing enforces yet, so nothing can break.
   - ✅ **Model done** — `Core::BuiltinRoles` (six roles, the two rule-based ones computed from the
     vocabulary so they cannot go stale), `Entity::EAM::Role` and `Entity::EAM::Grant` with their
     BSON mapping, and `Database::Authorization::Allows()` — §4's algorithm, decided by
     `tests/AuthorizationTest.cpp`.
   - ✅ **Storage done** — `eam_role` and `eam_grant` on `IEamRepository` / `MongoEamRepository`
     (which serves both backends, via `Database::collection()`), and both collections added to
     EMM's `eam` export spec. `tests/RoleRepositoryTest.cpp`.
   - ✅ **API done** — ten EAM actions (`create-role`, `update-role`, `get-role`, `list-roles`,
     `delete-role`, `grant-role`, `revoke-role`, `list-grants`, `list-permissions`,
     `check-permission`) with their DTOs and one CLI command each. All administrator-only except
     `list-permissions`, which is the vocabulary rather than anybody's access.
   - ⬜ **`authorization-gaps` deferred to step 4** — it reads what `shadow` mode records, and
     nothing records anything yet. An action answering from an empty store would look like "no
     gaps", which is the most misleading answer it could give.
3. ✅ **The gate.** `HttpActionServer::Dispatch` is now `final` and runs `Authorize()` before
   calling the new `DispatchAction()`, which all sixteen servers implement instead. The decision
   comes from a lookup registered by `Database::WireAuthorizationLookup()`, wired at every module's
   startup. Default mode `legacy`, so nothing changes until somebody says so.
   `tests/AuthorizationGateTest.cpp`.
4. ✅ **Migrate and shadow.** `eam:migrate-grants-to-roles` (rehearses by default, `--apply` to
   write, safe to re-run) and `eam:authorization-gaps`, backed by an `eam_authorization_gap`
   collection the gate writes to in `shadow` mode — one counted row per distinct
   (user, account, namespace, module, action), most-seen first.
   `tests/AuthorizationGapTest.cpp`.

   Two things worth knowing before running it: shadow mode costs **one database write per
   would-be refusal**, which is why it is not a mode to leave on; and the gap collection is
   deliberately **not** in EMM's export spec — it is what shadow mode observed on *this*
   installation, not configuration, and restoring one euclid's observations into another would put
   somebody else's traffic in front of the person deciding what to grant.
5. **`enforce`.** The machinery is built; what remains is running it — see §10.
6. **Only then** the client releases that stop reading `accountGrants`, and the release that removes
   the fields.
   - ✅ **Prerequisite built.** `HttpActionServer::AuthorizeResource()` and its lookup, so the role
     model's `Grant::resources` is actually consulted — ESM's `denyUngrantedBucket` and EQS's
     `denyUngrantedQueue` now ask both models through one call, and which one answers depends on the
     mode. Until this existed, `Grant::resources` was stored and never read, and `resourceGrants`
     was the only resource authorization euclid had: removing it would have deleted resource
     authorization outright.
   - ✅ **Done in euclid.** `User::accountGrants`, `User::resourceGrants` and the `AccountGrant`
     struct are gone, with `SetGrantLookup`, `SetResourceLookup` and `IsResourceAllowed`. Retired
     with them: `grant-namespace-access`, `revoke-namespace-access`, `migrate-grants-to-roles` and
     `authorization-gaps` — the first two because a namespace grant is now a role binding, the last
     two because they were the migration scaffolding.
   - ✅ **The modes went too**, for the reason in §11: with no legacy mechanism, `legacy` and
     `shadow` would mean "authorize nobody" while reading as "not yet". The gate always enforces,
     and `HttpActionServer::RequireEnforcingAuthorization()` — called from the constructor, so
     every module gets it — refuses to start on a configuration file that still names either.
   - ✅ **The five client repos.** euclid-rui keeps the old QML-facing shape and builds it from
     roles; jdk, pdk, ndk and cdk dropped `AccountGrant` outright and gained
     `grant-role`/`revoke-role`/`list-grants`/`check-permission`/`list-permissions`.
7. **FTP and SFTP**, which the six steps above never covered because they are not HTTP — see §4.3.
   - ✅ **Done.** Seven `ets:` permissions checked in `FtpSession`/`SftpSession` through
     `Transfer::TransferAuthorizer`, plus the built-in `transfer` role so the migration is one
     grant per server rather than a hand-written role per installation.
   - This step *is* the breaking one for transfer users, and it has no shadow mode: there was no
     old mechanism to run beside, only an absence. An installation upgrading has to grant before
     its clients reconnect.

Steps 1–3 are additive and reversible. Step 5 is the one that can lock people out, which is what
step 4 exists to prevent. Step 7 locks out transfer clients specifically, and the only mitigation
is that `reader` and `operator` already cover most of what they do.

---

## 10. Runbook: getting to `enforce`

Everything below is `euclid-cli`. Nothing here needs a release — the setting is read per request, so
each step takes effect as soon as it is written, and every one of them is reversible until the last.

The setting is `euclid.authorization.mode`, top-level in `euclid.json` beside `database` and
`gateway` rather than under `modules.eam`: the gate runs in **every** module process, and all of them
read it.

### 0. Before anything

```bash
euclid-cli eam list-permissions              # what can be granted at all
euclid-cli eam list-roles                    # the six built-ins, plus any of your own
```

Confirm you are in the `administrator` user group, and that somebody else is too. Installation
administrators bypass the gate entirely and hold no grants, so that group is the way back in if a
later step goes wrong. An installation whose only administrator is on holiday is not ready for step 3.

### 1. Migrate what already exists

```bash
euclid-cli eam migrate-grants-to-roles              # rehearses, prints what it would write
euclid-cli eam migrate-grants-to-roles --apply
```

Read the rehearsal. Two numbers matter more than the list:

- **`skipped`** — grants that already existed. Zero on a first run; non-zero means somebody has
  already granted roles by hand, which is fine.
- **`unrestricted`** — users with nothing to migrate. **This is the number that decides how long
  step 2 takes.** Today those users are unrestricted because they have no restrictions; under roles
  they have nothing at all. What they actually need is what shadow mode is about to tell you.

### 2. Watch, in `shadow`

```json
"authorization": { "mode": "shadow" }
```

The gate now evaluates every request, records what it *would* have refused, and refuses nothing.

Leave it for a full business cycle — a week at least, and longer if anything runs monthly. A gap that
only appears when the month-end job runs is a gap that will appear the first time it runs under
`enforce` instead.

```bash
euclid-cli eam authorization-gaps --page-size 0      # everything, most-seen first
euclid-cli eam authorization-gaps --prefix esm:      # one module at a time
```

Each entry is one of two things, and telling them apart is the whole job:

- **a grant somebody has to write** — `euclid-cli eam grant-role --role publisher --principal
  ern:...:user/order-service --namespace production`
- **a call that should genuinely stop working** — leave it; that is the point of the exercise.

After writing a round of grants:

```bash
euclid-cli eam authorization-gaps --clear
```

so the next round shows what is still missing rather than what used to be. Then wait again.

**`shadow` costs a database write per would-be refusal.** It is a mode to pass through, not to live
in.

### 3. Enforce

When `authorization-gaps` has been empty across a full cycle:

```json
"authorization": { "mode": "enforce" }
```

Refusals are now 403s, and each one carries its reason — `no grant applies to this caller in account
X, namespace Y`, or `no role granted here holds 'ens:publish-message'`. The first usually means a
missing grant, the second the wrong role.

**Going back is one setting.** Write `shadow` again and nothing is refused; the gaps carry on being
recorded. Nothing is lost by trying `enforce` and retreating.

### What is not gated, and why

Worth knowing before reading a log and wondering:

| | |
|---|---|
| `emm`, `emd` | No role can name their actions. EMM is administrator-only; EMD is the document store and is reached over a local socket. |
| Unauthenticated requests | Left to the handler, so it can answer 401 rather than the gate answering 403 about permissions that were never going to be asked for. euclid's own `MetricsPusher` depends on this. |
| Installation administrators | Bypass the gate and hold no grants. This is the way back in. |
| Inter-module domain events | Not HTTP at all — EventBus is a collection, so it never reaches a gate. |

### 4. Afterwards

Only once an installation has actually reached `enforce` does §5.1 come due: removing
`accountGrants` and `resourceGrants`, retiring `grant-namespace-access` and
`revoke-namespace-access`, and the client releases that stop reading them. That is the one step that
cannot be reversed with a setting.


---

## 11. Why step 6 cannot be done early

Steps 1–5 are additive: every one of them can be shipped, run and reverted with a setting. Step 6 is
the one that cannot, and it is worth writing down exactly why, because "we may as well remove the
old fields now" is a reasonable-sounding thing to say at any point before it is safe.

**`accountGrants` and `resourceGrants` are what authorizes euclid today.** Not "the old way of
authorizing" — the only way, whenever `euclid.authorization.mode` is `legacy`, which is the default
and what every installation upgrades into. `WireGrantLookup` reads `accountGrants` for account and
namespace scope; `denyUngrantedBucket` and `denyUngrantedQueue` read `resourceGrants` for the one
resource check euclid has.

So removing them has a hard order:

1. The role model has to *be able* to answer both questions. Module and action: done in step 3.
   Resource: done above — before that, `Grant::resources` was written and never read.
2. An installation has to have actually reached `enforce`, so the role model is demonstrably
   answering them for real traffic.
3. Only then do the fields come out — and with them `migrate-grants-to-roles`, which reads
   `accountGrants` and has nothing to migrate from once it is gone.

Doing it in any other order produces a euclid that authorizes nothing: the old mechanism deleted,
the new one inert because the mode still says `legacy`. That failure is silent and total — every
call from every authenticated user succeeds.

**The `legacy` mode has to go at the same time**, for the same reason. Once there is no legacy
mechanism, a mode that means "use the legacy mechanism" means "use nothing". The release that
removes the fields is the release that removes the mode, and a module that finds `legacy` written in
its configuration afterwards should refuse to start rather than run unguarded.

That is why this step is last, and why it is the only one with a prerequisite outside the code:
somebody has to have run it.
