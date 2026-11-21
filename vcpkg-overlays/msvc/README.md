# vcpkg overlay ports for MSVC

`libbson` and `mongo-c-driver` 2.5.5, exactly as pinned by the `builtin-baseline` in `vcpkg.json`,
plus one patch: `msvc-c4047-warning-not-error.patch`.

## Why

mongo-c's build turns MSVC warning C4047 into an error (`msvc:/we4047` in
`build/cmake/MongoC-Warnings.cmake`). MSVC 19.52, the compiler on the `windows-latest` runner since
November 2026, raises C4047 when a pointer is passed to a `bool` parameter. libbson does this in
`mlib_check(ptr)`, e.g. `src/libbson/src/bson/validate.c:253`:

```
error C4047: 'function': 'bool' differs in levels of indirection from 'const char *const '
```

Converting a pointer to `bool` is valid C (non-null is true), so the code is correct; only the
escalation is removed, and C4047 stays a warning. The patch touches nothing else, and the
`-Werror=int-conversion` that GCC and Clang get is unchanged.

Neither 2.5.7 nor upstream `master` (checked 2026-11-21) changes these lines, so a baseline bump does
not fix it.

## Where it is used

Only by the Windows job in `.github/workflows/release.yml`, through `VCPKG_OVERLAY_PORTS`. A local
Windows build with Visual Studio 2026 / MSVC 19.52 needs the same:

```powershell
$env:VCPKG_OVERLAY_PORTS = "$PWD\vcpkg-overlays\msvc"
```

## When to remove

When mongo-c-driver stops escalating C4047, or stops passing pointers to `bool` in `mlib_check`, and
the baseline is moved to that version. The ports here must be kept at the baseline's version until
then: an overlay replaces the port whatever version the baseline names, so a baseline bump alone
would silently keep 2.5.5 on Windows.
