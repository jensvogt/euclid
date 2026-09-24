# Hardening

**Status:** built. Warnings, hardening flags and a sanitizer build are in `CMakeLists.txt`; four
fuzz targets are in `fuzz/`; CI runs every test suite twice, once normally and once under
AddressSanitizer and UndefinedBehaviorSanitizer, and smokes the fuzz targets. Written 2026-09-20.

What the compiler, the loader and the test suite are asked to do about the mistakes a review
misses. None of it replaces reading the code; all of it catches things reading the code did not.

---

## 1. Warnings

On for GCC and Clang, not for MSVC, whose existing options are left alone.

| Flag | Catches |
|---|---|
| `-Wall -Wextra` | the usual set |
| `-Wcast-qual` | casting away `const` — how a "read-only" argument gets written to |
| `-Wconversion` | a narrowing that silently changes a value: the arithmetic half of a size bug |
| `-Wsign-conversion` | signed/unsigned mix-ups in index and size arithmetic |
| `-Wnon-virtual-dtor` | deleting through a base pointer with no virtual destructor |
| `-Woverloaded-virtual` | an "override" that overrides nothing and is never called |
| `-Wdouble-promotion` | a float silently widened, usually in a format string |
| `-Wformat=2` | a format string that does not match its arguments |
| `-Wimplicit-fallthrough` | a `switch` case falling through without saying so |
| `-Wshadow` | a name hiding another — how the wrong variable gets assigned |

**Not `-Werror`, deliberately.** A warning that stops the build is one somebody silences in a
hurry. The point of turning these on is that they are read.

**`-Wnull-dereference` is off**, behind `-DEUCLID_WARN_NULL_DEREFERENCE=ON`. GCC raises it from the
optimizer, and the optimizer does not consult the `-isystem` marking that keeps third-party headers
quiet — so a header inlined into a euclid translation unit reports against itself, and 334 of them do.
Not restricted to Debug instead, which is the tempting half-measure: the warning needs `-O2` to fire at
all, so a Debug-only setting is not weaker coverage but none, wearing the appearance of some. See §6.

**`-Wno-missing-field-initializers`**, and not because it is inconvenient: GCC raises it for a
designated initializer that omits a member *which has a default member initializer*, which is the
whole point of writing one. Every occurrence in this codebase was that, and leaving it on buried
the thirty warnings worth reading under two hundred that said nothing.

The first run produced 309 warnings. Suppressing that one class removed 198; changing four
`static` definitions at namespace scope in `MessagePriority.h` to `inline` removed 56 more — a
`static` function in a header gives every translation unit its own copy and warns in each one that
does not call it. **55 remained**, and they were the ones worth triaging. All 55 have since been
worked through. **euclid's own sources build warning-free**, which is what makes the table below history rather
than a to-do list:

| | was | now | |
|---|---|---|---|
| `-Wsign-conversion` | 26 | 0 | triaged below |
| `-Wconversion` | 1 | 0 | triaged below |
| `-Wshadow` | 12 | 0 | triaged below |
| `-Wreorder` | 6 | 0 | one constructor, `ProxyServer`; initializer list reordered to match the declarations |
| `-Wunused-*` | 10 | 0 | triaged below |

### The conversion triage

All 27 sites, in eight files. One was a real defect; the rest were correct but said so only by
accident, and are now explicit.

| Site | Verdict |
|---|---|
| `PriorityWeights.cpp` ×19 | `int i` indexing a `std::array<…,3>`. Bounded by the loop condition, never negative. Benign — now `std::size_t`. |
| `Configuration.cpp:215-216` | `std::string pad(indent * 2, ' ')` with `indent` an `int`. A recursion depth, so never negative, but a negative one would ask for a ~2⁶⁴-byte string rather than a short one. `prettyPrintValue` now takes `std::size_t`. |
| `PasswordUtils.cpp:53` | `diff \|= a[i] ^ b[i]` — `unsigned char ^ unsigned char` promotes to `int` and narrows back. Values are 0–255, so nothing is lost; cast made explicit. |
| `SigV4.cpp:52`, `Controller.cpp:67` | `for (const unsigned char c : someString)`. Correct *because* of the conversion, not despite it: a byte over `0x7f` is a negative `char`, which would make `uriEncode` emit `%FFFFFF80` and `sanitizeForLog`'s `c >= 0x20` accept the control bytes it exists to escape. Rewritten as an explicit cast with a comment, because this is the kind of right answer somebody later "simplifies" into a wrong one. |
| `SamlTest.cpp:171` | `std::string(ptr, size)` with libxml2's `int` length. Test-only. Floored at 0. |
| **`UnixSocketServer.cpp:132`, `GatewayServer.cpp:661`** | **`_workers.reserve(_threads)` with `_threads` an `int` straight from configuration.** See below. |

The last one was the real find. `HttpActionServer::ConfiguredWorkerThreads` clamps its count to
[1, 256] and warns — but the gateway never calls it. It reads
`euclid.gateway.http.max-thread` itself and passes the number to the constructor, where it sizes a
`std::vector`. A negative count is not a small size there, it is a very large unsigned one: the
`reserve` throws `length_error`, and `main.cpp` reports "Failed to start gateway" without a word
about the typo behind it. `GatewayEventIngest` reached `UnixSocketServer` unclamped by the same
route.

The clamp moved down to `UnixSocketServer::ClampWorkerThreads`, which both constructors and
`ConfiguredWorkerThreads` now share, so it covers every way a count gets in rather than the one
that went through the configuration reader. Covered by `WorkerThreadsConfigTest`.

### The shadow triage

Seven of the twelve were one root cause, and it was in euclid's own header.
`BOOST_LOG_ATTRIBUTE_KEYWORD` declares a variable at whatever scope you invoke it in, and
`LogStream.h` invoked it seven times at **global** scope: `process_id`, `thread_id`, `timestamp`,
`line`, `file`, `function`, `channel`. That header reaches nearly every translation unit in
euclid, so `auto file = ...` or a parameter called `line` shadowed a global anywhere in the
codebase — which is how four tests and three more in `SinkLevelTest` came to warn about a name
they had every right to use.

It had already caused a workaround rather than merely threatening one: the record filter in
`LogStream.cpp` was written `attributes[::channel]`, with an explicit global-scope qualifier,
because the parameter beside it is also called `channel`. The keywords now live in
`Euclid::Core::Log`, which took two call sites to update — they were only ever subscripted twice,
everything else extracts attributes by string name.

The rest:

| Site | Verdict |
|---|---|
| `MonitoringData.cpp:16` ×2 | `for (const auto &[name, value] : labels)` shadowing the `name` and `value` **members**, which are serialised five and ten lines later as `kvp("name", name)` and `kvp("value", value)`. Correct today only because the loop closes first. Renamed `labelName`/`labelValue`. |
| `Certificate.cpp:23` | The same shape: `for (const auto &name : subjectAltNames)` over a member `name` that is serialised just below. Renamed `altName`. |
| `MessageRetentionTest`, `TopicRetentionTest` | A parameter `seconds` over a file-scope `using seconds = std::chrono::seconds`. Renamed `period`. |

### The unused triage

| Site | Verdict |
|---|---|
| `SystemUtils::RunShellCommand` | **Deleted.** No callers anywhere in euclid or the sibling repos, and broken as written: it takes a `shellcmd` argument, ignores it, and unconditionally runs whatever `find_executable("aws")` returns. Its two log lines — the only places `shellcmd` appeared — were commented out under `// TODO: fix me`. A shell-command runner that silently substitutes a different command is not a thing to keep warm for a future caller. |
| `EamServer::alreadyGranted` | **Deleted.** The orphan of a grant migration that was removed before it landed; `MigrateGrantsRequest`/`Response` are gone from the tree too. |
| `TransferStorage::readFile` | **Deleted.** Plain dead code. |
| `Controller.cpp:1458` `fresh` | **Deleted, and the open question above it closed** — see below. |
| `GatewayServer.cpp:285` `ok` | **Deleted.** An unused response helper next to the `err` one that is used. |
| `CheckScope(…, subject)` | `[[maybe_unused]]`. A placeholder for the per-user grant check, wired when `GrantLookup` is. |
| `RedeployRefusal(…, version, …)` | `[[maybe_unused]]`. The comment three lines down explains at length why the version is deliberately not checked. |
| `SftpSession::spoolPathFor(key)` | `[[maybe_unused]]`. Deliberately spools under a UUID rather than the key, for reasons the comment gives. |

**The freshness cutoff at `Controller.cpp:1458` was flagged here as an open autoscaling question.
It is not one.** Reading the fallback path properly closes it: `LoadFreshnessSeconds()` is 45
seconds, sized for an application's own reporting interval, and its docstring says outright that
the figure "no longer travels through EMO's buckets". Nothing on the fallback path reports
directly — every figure there has been through EMO, which writes one bucket per averaging period,
300 seconds by default. Measuring a 300-second bucket against a 45-second cutoff would discard
every sample and the fallback would find nothing at all, for every pool it exists to serve.
Staleness on that path is bounded by the query window instead, and the loop below already says so:
*"Same rule as the direct road, but keyed on whether EMO has a sample for the instance rather than
on how fresh its own report is."* The variable was left behind by the refactor that introduced the
direct road. Deleting it is the correct resolution; applying it would have been the bug.

### What the two unused helpers were hiding

The `applyMetadata()` pair — one in `EkmServer.cpp`, one in `EqsServer.cpp`, each three lines
copying the authenticated user into a response's `user`, `accountId` and `region` — turned out to
be the visible end of a gap rather than dead code, and have since been wired in rather than
deleted.

`BaseDto` carries those three fields and its docstring says they are "serialized as a nested
`metadata` object". Inheriting them is not the same as sending them: a DTO's `value_from` writes
the keys it names and nothing else. Across EKM and EQS, 21 of the 22 response types were missing
one or both halves — five EKM types did not derive from `BaseDto` at all, most of the rest derived
but never emitted a `metadata` key, and `EQS::CreateQueueResponse` had the emit line **commented
out since the initial commit**. Two EQS responses did emit it, and shipped
`{"region":"","user":"","accountId":""}` on every call, because nothing populated it.

Nothing failed at any point. An empty string serializes as happily as a full one, and a key that
is never written is a key no caller sees missing. That is the shape worth naming: the helper, the
inheritance and the serializer each looked correct on their own, and the feature only existed if
all three lined up.

Now wired end to end, and covered by `ResponseMetadataTest` — which asserts the *wire format*
rather than the members, since the members were never the part that was broken. `GetQueueMetadataResponse`
is deliberately excluded: its top-level `region` and `accountId` describe the queue, not the
caller, and giving it a `BaseDto` base would hide them behind identically named inherited fields.

## 2. Hardening flags

Every flag is tested with `check_cxx_compiler_flag` / `check_linker_flag` rather than assumed.
euclid builds for Linux, macOS on arm64 and Windows, and half of these are x86-only or GNU-only —
a flag the compiler rejects is a build that fails on somebody else's machine.

| Flag | What it does |
|---|---|
| `-D_GLIBCXX_ASSERTIONS` | bounds and precondition checks inside libstdc++: `vector::operator[]`, `string::front()` on an empty string, an invalid iterator range. The most useful of these for reading past the end of a container, and it needs no source change to benefit |
| `-fstack-protector-strong` | a canary between the locals and the return address, for any frame with an array or an address-taken local. `strong` is the setting worth having: `all` costs more than it returns, plain `-fstack-protector` covers too little |
| `-fstack-clash-protection` | a large stack allocation cannot skip past the guard page into whatever is next — what turns "allocated too much" into "wrote somewhere else" |
| `-fcf-protection=full` | indirect branch tracking and a shadow stack where the hardware has them, which makes a corrupted return address unusable rather than merely wrong. x86 only |
| `-D_FORTIFY_SOURCE=3` | **optimized builds only.** It rewrites calls whose sizes the optimizer can see, so at `-O0` it does nothing except warn that it is doing nothing |
| `-z relro`, `-z now` | the GOT and relocation tables become read-only once the loader is done, rather than staying writable for the life of the process. `now` is what makes `relro` cover the GOT at all |
| `-z noexecstack` | |
| `CMAKE_POSITION_INDEPENDENT_CODE` | so the loader can put the image somewhere an attacker has to find first |

### Checking a binary

The flags being on the command line is not the same as the protections being in the artifact:

```sh
bin=build/bin/euclid-mgr
readelf -lW  "$bin" | grep -q GNU_RELRO        && echo "relro"
readelf -dW  "$bin" | grep -q BIND_NOW         && echo "full relro"
readelf -lW  "$bin" | grep GNU_STACK | grep -qv RWE && echo "nx stack"
readelf -hW  "$bin" | grep -q DYN              && echo "pie"
readelf -sW  "$bin" | grep -q __stack_chk_fail && echo "stack canary"
```

All five hold for the current build.

## 3. Sanitizers

```sh
cmake -B build-asan -DCMAKE_BUILD_TYPE=Debug -DEUCLID_SANITIZE=address,undefined
cmake --build build-asan
ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build-asan --output-on-failure
```

Off by default, and not a build type of its own so that any build can be repeated with them on
without a second configuration to keep in step. A sanitized build is slower and uses more memory,
which is why it belongs in CI over the test suites rather than in anybody's working build.

`detect_leaks=0` because leak detection reports what a process still holds at exit, which for a
test that never shuts a singleton down is noise rather than a bug.

### What it found on the first run

Two memory errors, in the first two suites run:

**A heap-use-after-free in production code**, `core/src/LogStream.cpp`, in the JSON log formatter:

```cpp
if (const auto *level = out.if_contains("level"); …) {
    out["log.level"] = *level;          // inserts → may reallocate → level dangles
```

`level` points into `out`, and `operator[]` on a key that is not there inserts one, which can
reallocate the object's storage. The value written would then be whatever the freed memory held.
The effect would have been a corrupted `log.level` on exactly the application records the
splicing exists for — silent, and plausible enough to survive review. Fixed by copying the value
out before inserting.

**A heap-buffer-overflow in a test**, `tests/HttpSignatureTest.cpp`:

```cpp
hmacSha256({std::string(kSecret).begin(), std::string(kSecret).end()}, *base)
```

Two separate temporaries, so the iterator pair is not a range at all: it reads from the first
until it passes the address of the second's end.

After both, all 66 suites pass clean under ASan and UBSan.

## 4. Fuzzing

Four targets, one per parser that reads bytes a stranger chose — the code an unauthenticated
caller reaches before anything has vouched for them:

| Target | What it parses |
|---|---|
| `euclid-fuzz-signature-input` | the RFC 9421 `Signature-Input` header, parsed before the caller is known |
| `euclid-fuzz-sigv4` | SigV4's `Authorization` header and the query-string canonicalisation |
| `euclid-fuzz-query` | `ParseQueryParameters` — the percent-decoding an OIDC or SAML redirect goes through |
| `euclid-fuzz-upload-key` | `ResolveUploadKey` — a request target turned into an object key |

```sh
fuzz/build.sh                                    # clang + libFuzzer + ASan + UBSan
fuzz/smoke.sh                                    # 20k runs each, what CI does
cmake-build-debug/fuzz/euclid-fuzz-upload-key fuzz/corpus/upload-key -runs=100000000
```

The upload-key target does more than watch for crashes. A traversal that returns *cleanly* with a
key outside the route's prefix is the bug that matters there, and no crash would mark it — so the
target asserts the property directly:

```cpp
if (resolved.valid) {
    if (!resolved.key.starts_with("incoming/tester/")) __builtin_trap();
    if (resolved.key.find("/../") != std::string::npos) __builtin_trap();
}
```

**Why it is a script and not part of the CMake build.** libFuzzer is a clang runtime; euclid is
built with GCC 14. Configuring the project with clang makes vcpkg rebuild every dependency under
the new compiler, which takes about an hour and fails on `boost-cobalt`. So `fuzz/build.sh`
compiles the few euclid sources each target needs directly with clang, and borrows only the
*headers* an ordinary build already installed. Nothing under test comes from the GCC build: every
file a target exercises is compiled by clang and instrumented.

300,000 runs of each found nothing. That is not a clean bill of health — it is a baseline, and the
corpora in `fuzz/corpus/` are what a later run starts from.

## 5. CI

`.github/workflows/test.yml` has two jobs:

- **test** — builds and runs every suite.
- **sanitizers** — the same, under `address,undefined`, with `halt_on_error=1` so a finding fails
  the job rather than being printed and passed over.

Until this change CI built `--target euclid-tests` and ran `-R euclid-tests`: **one of the
sixty-six suites.** The other sixty-five were neither compiled nor run, so a pull request could
break any of them and still go green. Both of the memory errors above were in suites CI never
touched.

The sanitizer job roughly doubles CI time and builds a second copy of everything. If that is too
slow for every pull request it is a reasonable candidate for a nightly schedule — but the
"run every suite" change belongs on every pull request regardless.

## 6. Not done

- **`-Wnull-dereference` is off by default**, which is a decision rather than an omission. Measured on
  a full Release build of this tree with GCC 14.2: **334 findings, none of them in euclid's own code** —
  127 in `boost/beast/http/impl/fields.hpp`, 108 in `boost/asio/detail/impl/scheduler.ipp`, 72 in
  `boost/asio/io_context.hpp`, and 27 in libstdc++. With it off, a full Release build is at zero
  warnings, the same as Debug.

  Three things were checked before turning it off, because each of them looks like a fix and is not:

  | | |
  |---|---|
  | `-isystem` | already in use — vcpkg's include arrives that way. The warning comes out of the optimizer, which does not consult it. |
  | Debug only | the warning needs `-O2` to fire at all. At `-O0` GCC does not report even a plainly null dereference, so this is no coverage dressed as some. |
  | clang | accepts the flag, reports no unknown option, and implements no such analysis. It never warned on anything, whatever the flags said. |

  What does work, if it is ever wanted permanently, is `#pragma GCC diagnostic ignored` around the
  offending includes: GCC checks the pragma state at the location it *reports*, so the suppression
  applies where `-isystem` does not. It needs asio and beast wrapped everywhere they are included, and
  one new file including either directly brings the noise back, which is why it is a switch instead:

  ```
  cmake -B build -DEUCLID_WARN_NULL_DEREFERENCE=ON
  cmake --build build 2>&1 | grep -v vcpkg_installed | grep -v '/c++/'
  ```
- **`-Werror` is still not set.** It is now possible for euclid's own sources — they are
  warning-free in both configurations, Release included now that the paragraph above is settled —
  where before it would have failed on 55 findings. Worth doing only with a decision about non-GCC builds: a
  different compiler or a newer GCC finds warnings this one does not, and `-Werror` turns each of
  those into a build that does not compile rather than a build that complains. The middle option
  is to set it for the CI job and not for local builds.
- **The gateway's request path is not fuzzed.** The four targets cover the parsers that take a
  `std::string`; `ProxyServer`'s reading of a request is stateful and asynchronous, and fuzzing it
  properly means driving a socket rather than calling a function.
- **No long campaign has been run**, and no corpus has been minimised. The smoke pass catches
  regressions; finding something new needs hours, not seconds.
