# ext#59 — per-request `define()` constant isolation collapse (fixed 0.3.60)

**Status:** FIXED in ext-zealphp 0.3.60.
**Symptom (0.3.53–0.3.59):** in `coroutine-legacy`, a request constant defined via
`define()` was read by *peer* coroutines/requests after an I/O yield — cross-request
value disclosure that climbed from ~42% (0.3.52) to ~97% (0.3.59) — plus an
intermittent corrupted-zval `~137 TB` OOM worker crash under single-worker
concurrency (a memory-safety escalation over the value leak).

## Regression bisect

Empirical sweep (OpenSwoole 26.2.0, PHP 8.3.6, `worker_num=4`, 40-concurrent bursts,
`superglobals(true) + globalScopeInclude(true) + defineIsolation(true)`):

| tag | leak | note |
|---|---|---|
| v0.3.52 | ~52% | pre-regression baseline (process-wide tracker, no clean-on-resume) |
| **v0.3.53** | **~62–92%** | **regressing commit `1530b97` (ext#16 fix)** — added `zend_hash_clean(&zealphp_request_constants)` on every **resume** |
| v0.3.54–0.3.59 | 90–97% | leak persists; 0.3.59 Option A op_array cache widens the warm-`FETCH_CONSTANT` window |

The regression is **not** the Option A op_array cache (a plausible suspect); it is the
0.3.53 "reset the process-wide request-constant tracker on every resume" line.

## Root cause

Two coupled facts:

1. **The request-constant *tracker* was process-wide.** `define()` recorded names into
   one process-global `zealphp_request_constants` table. Under coroutine interleaving,
   a peer's resume ran `zend_hash_clean()` on that shared table (0.3.53), wiping coroutine
   A's pending names *before* A reached its own yield. A's `snapshot_save` then had
   nothing to orphan, so A's constant stayed live in the shared `EG(zend_constants)`.

2. **`os_get_cid()` is not reliable in `on_resume`.** The 0.3.53 attempt to make the
   tracker per-coroutine (refill it on resume from the snapshot's saved names) keyed the
   refill by `os_get_cid()` — which the codebase already documents as reliable in
   `on_yield`/in-coroutine execution but **not** in `on_resume`. So the *real* coroutine's
   name list never got refilled after a yield, the request-end `zealphp_constants_clear()`
   found an empty list, removed nothing, and the constant stuck in `EG` **worker-lifetime**.
   Every later request guarded by `if (!defined(X)) define(X, …)` then adopted the stale
   value — the dominant 42%→97% leak.

Instrumented proof (single worker, 20-request burst): only **6** `define()` calls fired
across 20 requests, and every `clear` ran with `lane_n=0/-1` while `eg_has_REQ_CONST=1`.

## The fix — per-coroutine constant *lanes* owned by reliable-cid contexts

`zealphp_coro_reqconst_lanes`: `cid -> {name: 1}`, one lane per coroutine, keyed by the
**real** coroutine id (`os_get_cid()`), touched **only** where that id is reliable:

- **`define()`** (in-coroutine): add the name to *this* coroutine's lane.
- **`snapshot_save`** (`on_yield`, cid reliable): read the lane, orphan those names out of
  `EG` into the pointer-keyed snapshot. **Does not clear the lane** — it persists for the
  whole request.
- **`snapshot_restore`** (`on_resume`, cid **un**reliable): re-install the orphaned structs
  from the **pointer-keyed** snapshot only. **Never touches the lane** — this is the crux.
- **`constants_clear`** (request end, in-coroutine, cid reliable): read the lane, orphan the
  names from `EG` into the deferred-free set, then clear the lane.
- **`on_close`**: free the deferred constants and delete the lane.

Because the lane is only ever read/written where `os_get_cid()` is reliable, and restore
relies on the already-reliable coroutine pointer, the request-end clear always finds its
own names → the constant leaves `EG` at request end → later requests see it absent.

The process-wide `zealphp_request_constants` table is kept **only** for the sync /
non-coroutine path (`cid <= 0`), where there is no cross-coroutine sharing.

## Validation

phpt: full suite 67/67 (2 skips); `016`, `045`, `067` (ext#16 same-name shadow) green;
new `tests/071-constant-clear-after-yield.phpt` pins the clear-after-yield contract.

Canonical concurrent validation is the **HTTP burst harness** (phpt cannot exercise the
server's `on_resume` cid-unreliability — the `Timer`-driven resume in-process keeps the cid
reliable, same limitation documented for phpredis-SUBSCRIBE). Harness under `spike/ext59/`:

| cell | before (0.3.59) | after (0.3.60) |
|---|---|---|
| `defineIsolation(true)`, w=4, 40×3 | ~93% leak | **0/120 leak, 0 fatal** |
| `defineIsolation(true)`, w=1 (corruption cell), 40×3 | HANG + ~137 TB OOM | **0/120 leak, 0 fatal** |
| stress w=1 60×8 / w=4 80×8 / k2 60×5 | — | **0/480, 0/640, 0/300 leak, 0 fatal** |
| control (`putenv`/`ini_set`/`$_SESSION`) | 0 | 0 |

**Note on `defineIsolation(false)`:** without S10 (opt-in), a `define()` persists
worker-lifetime — confirmed **97.5% at concurrency=1 (sequential)**, i.e. plain constant
persistence, not a race. This is by design; enable `App::defineIsolation(true)` for
PHP-FPM-style per-request constants.
