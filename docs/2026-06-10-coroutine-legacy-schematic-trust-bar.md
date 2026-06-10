# coroutine-legacy "old PHP just works" — in-request schematic trust bar (2026-06-10)

Goal: verify that `MODE_COROUTINE_LEGACY` isolates **every** old-PHP request-state
primitive per coroutine under real concurrency, so unmodified request-style PHP
runs correctly. Measured on the test VM (PHP 8.4.21, OpenSwoole 26.2.0,
ext-zealphp **v0.3.42**) with a probe that sets each primitive to a per-request
unique token, **yields twice** (Timer-based, forcing interleave), and re-reads.

## Result — IN-REQUEST CONTRACT: 100%

```
600 requests, peak_concurrency=66, double-yield per request
ALL ISOLATED: 0 leaks
```

All 22 probed primitives kept their own per-request value across both yields,
with up to 66 coroutines interleaving:

| Primitive | Isolated |
|---|---|
| `$_GET` `$_POST` `$_REQUEST` `$_COOKIE` `$_FILES` `$_SERVER` `$_SESSION` | ✅ |
| `$_SERVER` re-read BETWEEN the two yields (the #42 multi-yield angle) | ✅ |
| class static property | ✅ |
| `$GLOBALS` / `global $x` | ✅ |
| `define()` constant (defineIsolation) | ✅ |
| `ini_set()` | ✅ |
| function-local `static $x` (Stage 5, default-on) | ✅ |
| `putenv()` / `getenv()` | ✅ |
| `http_response_code()` (set 207, read back) | ✅ |
| `error_reporting()` level | ✅ |
| output buffering `ob_start`/`ob_get_contents`/`ob_get_level` | ✅ |
| `require_once` re-exec (Stage 7 — included file re-runs, sets per-request global) | ✅ |
| `chdir()`/`getcwd()` (Stage CWD) | ✅ |
| `setlocale()` (Stage locale) | ✅ |
| bootstrap-time global stays visible | ✅ |

This is a superset of `TrustBarIsolationTest`'s original 14 primitives — it adds
`http_response_code`, `error_reporting`, output buffering, `require_once`
re-exec, and a **double yield** with a mid-yield server re-check (the #42
"server lost on the 2nd yield" angle). All clean.

**Conclusion:** the in-request execution path — the one a legacy request-style
PHP app actually runs on (top-to-bottom in the request coroutine, no `go()`) —
is memory-safe and leak-free under heavy interleaving. "Old PHP just works" holds
for the in-request contract. A request coroutine spawning a *naive* `go()` child
also does NOT corrupt the parent (verified separately).

## Known boundary — child coroutines do NOT inherit request superglobals

A `go()` child (or `App::parallel`/`parallelLimit` task) gets its own
per-coroutine `RequestContext` (empty) and reads the live `$_SERVER`, which is
cleared/owned by the parent — so the child sees empty/foreign superglobals:

```
parent (request coro):  $_SERVER has X_TOKEN=req1   ✓
go() child:             $_SERVER empty / foreign    ✗   (NOSNAP on resume — never registered as a request coro)
parent after join:      X_TOKEN=req1                ✓   (parent unaffected by a NAIVE child)
```

Root cause: the superglobal save gate skips non-owner coroutines (correct — stops
a child *stealing* the parent's state, #332), and the restore gate only replays
snapshots for *registered request* coroutines (#40). A child is neither, so it
has no snapshot to restore. Making the child *claim ownership* fixes the child
but STEALS from the parent (re-introduces #332 — verified: parent loses its
server after the child claims).

This is not an "old PHP" pattern (legacy apps have no coroutines), but it affects
ZealPHP's own concurrency primitives (`App::parallel`) and any app that renders
inside `go()`. The correct fix is a non-stealing *adopt* primitive: register the
child as a request-context-bearing coroutine with its OWN snapshot lane (seeded
from the parent's superglobals) without touching the single global owner — i.e.
treat the child as "just another request coroutine," which the multi-request
machinery already isolates correctly (proven above at 66-way concurrency). Design
+ implementation tracked separately (the save gate moves from owner-based to
request-coro-registration-based; `App::parallel` captures parent superglobals and
the child registers + restores them at entry).
